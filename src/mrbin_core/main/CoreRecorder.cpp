#include "CoreRecorder.h"
#include "CoreConfig.h"
#include "CoreCrypto.h"
#include "CoreGPIO.h"
#include "CoreH264Buffer.h"
#include "CoreSD.h"
#include "CoreStatusLed.h"
#include "CoreVideo.h"

#include "encoder/esp_video_enc_default.h"
#include "esp_capture.h"
#include "esp_capture_sink.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "impl/esp_capture_video_v4l2_src.h"
#include "impl/mp4_muxer.h"
#include "esp_muxer.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "core_recorder";

#define REC_MIN_MP4_BYTES       1024
#define SD_PREP_DONE_BIT        BIT0
#define H264_CODEC_SPEC_MAX     512
#define REC_STATUS_LOG_MS       5000
#define REC_DRAIN_TIMEOUT_MS    1000
#define MANUAL_REC_DONE_BIT     BIT0
#define MANUAL_REC_TASK_STACK   12288
#define MANUAL_REC_TASK_PRIO    5

typedef struct {
    char  *ptr;
    size_t cap;
} file_io_buf_t;

static file_io_buf_t file_io_buf_alloc(void) {
    file_io_buf_t b = {};
    b.ptr = (char *)heap_caps_malloc(CORE_SD_FILE_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (b.ptr) {
        b.cap = CORE_SD_FILE_BUF_BYTES;
    }
    return b;
}

static void file_io_buf_attach(FILE *fp, file_io_buf_t *b) {
    if (fp && b && b->ptr) {
        setvbuf(fp, b->ptr, _IOFBF, b->cap);
    }
}

static void file_io_buf_free(file_io_buf_t *b) {
    if (b && b->ptr) {
        heap_caps_free(b->ptr);
        b->ptr = nullptr;
        b->cap = 0;
    }
}

// encrypt_mp4_file resta per il recupero dei vecchi .mp4.tmp orfani (registrazioni
// pre-VFS interrotte). Le nuove registrazioni sono cifrate on-the-fly dal VFS /enc.
static bool encrypt_mp4_file(const char *plain_path, const char *enc_path, const uint8_t key[16]);

typedef bool (*recorder_should_stop_fn_t)(void);

typedef struct {
    char path[140];
} recorder_muxer_ctx_t;

typedef struct {
    esp_capture_handle_t capture;
    esp_capture_sink_handle_t sink;
    esp_capture_video_src_if_t *vsrc;
    bool running;
    int64_t pts_base_us;   // istante (esp_timer) del primo frame; PTS = tempo reale trascorso
} recorder_capture_t;

// PTS in ms basato sul tempo reale trascorso dal primo frame: la durata dell'MP4
// coincide con la durata reale della registrazione, a prescindere dagli fps effettivi.
static uint32_t recorder_next_pts_ms(recorder_capture_t *rc) {
    int64_t now = esp_timer_get_time();
    if (rc->pts_base_us < 0) {
        rc->pts_base_us = now;
    }
    return (uint32_t)((now - rc->pts_base_us) / 1000);
}

typedef struct {
    EventGroupHandle_t ev;
    uint32_t           core_id;
    char               tmp_path[140];
    char               final_path[128];
    char               enc_path[160];   // "/enc" + final_path: mux cifra on-the-fly
    bool               paths_ok;
    bool               sd_mounted;
} sd_prep_ctx_t;

typedef struct {
    esp_muxer_handle_t handle;
    int                stream_index;
    bool               active;
} recorder_muxer_t;

#define CORE_CRYPTO_FILE_HEADER   20   // MRBI (4) + IV (16)

static bool recording_file_is_valid(const char *path);

static bool plain_mp4_has_ftyp(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    uint8_t buf[512];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n < 8) {
        return false;
    }
    for (size_t i = 0; i + 8 <= n; ++i) {
        if (buf[i + 4] == 'f' && buf[i + 5] == 't' && buf[i + 6] == 'y' && buf[i + 7] == 'p') {
            return true;
        }
    }
    return false;
}

static bool encrypt_mp4_file(const char *plain_path, const char *enc_path, const uint8_t key[16]) {
    int64_t t0 = esp_timer_get_time();

    struct stat st;
    if (stat(plain_path, &st) != 0 || st.st_size < (off_t)REC_MIN_MP4_BYTES) {
        ESP_LOGE(TAG, "MP4 temporaneo non valido: %s", plain_path);
        return false;
    }
    if (!plain_mp4_has_ftyp(plain_path)) {
        ESP_LOGE(TAG, "MP4 tmp senza header ftyp: %s", plain_path);
        return false;
    }

    FILE *fin = fopen(plain_path, "rb");
    if (!fin) {
        ESP_LOGE(TAG, "Apertura %s fallita (errno=%d)", plain_path, errno);
        return false;
    }

    // Cifratura atomica: scrive su <enc_path>.part e rinomina in .mp4 solo a fine OK.
    // Così il file .mp4 finale non è mai visibile incompleto/0 byte.
    char part_path[160];
    snprintf(part_path, sizeof(part_path), "%s.part", enc_path);
    remove(part_path);

    FILE *fout = fopen(part_path, "wb");
    if (!fout) {
        ESP_LOGE(TAG, "Creazione %s fallita (errno=%d)", part_path, errno);
        fclose(fin);
        return false;
    }

    file_io_buf_t fin_vbuf = file_io_buf_alloc();
    file_io_buf_t fout_vbuf = file_io_buf_alloc();
    file_io_buf_attach(fin, &fin_vbuf);
    file_io_buf_attach(fout, &fout_vbuf);

    size_t io_buf_size = CORE_REC_ENCRYPT_IO_BYTES;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        io_buf_size = 64 * 1024;
        buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        file_io_buf_free(&fin_vbuf);
        file_io_buf_free(&fout_vbuf);
        fclose(fin);
        fclose(fout);
        remove(part_path);
        return false;
    }

    core_crypto_ctx_t ctx = {};

    uint32_t magic = CORE_CRYPTO_MAGIC;
    if (fwrite(&magic, 1, sizeof(magic), fout) != sizeof(magic)) {
        heap_caps_free(buf);
        file_io_buf_free(&fin_vbuf);
        file_io_buf_free(&fout_vbuf);
        fclose(fin);
        fclose(fout);
        remove(part_path);
        return false;
    }

    uint8_t iv[16];
    esp_fill_random(iv, sizeof(iv));
    if (fwrite(iv, 1, sizeof(iv), fout) != sizeof(iv)) {
        heap_caps_free(buf);
        file_io_buf_free(&fin_vbuf);
        file_io_buf_free(&fout_vbuf);
        fclose(fin);
        fclose(fout);
        remove(part_path);
        return false;
    }

    if (!core_crypto_init_ctx(&ctx, key, iv)) {
        heap_caps_free(buf);
        file_io_buf_free(&fin_vbuf);
        file_io_buf_free(&fout_vbuf);
        fclose(fin);
        fclose(fout);
        remove(part_path);
        return false;
    }

    uint64_t offset = 0;
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, io_buf_size, fin)) > 0) {
        core_crypto_crypt_buffer(&ctx, buf, n, offset);
        if (fwrite(buf, 1, n, fout) != n) {
            ok = false;
            break;
        }
        offset += n;
    }

    core_crypto_deinit_ctx(&ctx);
    fflush(fout);
    heap_caps_free(buf);
    file_io_buf_free(&fin_vbuf);
    file_io_buf_free(&fout_vbuf);
    fclose(fin);
    fclose(fout);

    if (!ok) {
        remove(part_path);
        return false;
    }

    // Rinomina atomica: il .mp4 finale compare solo ora, completo e valido.
    remove(enc_path);
    if (rename(part_path, enc_path) != 0) {
        ESP_LOGE(TAG, "Rename %s -> %s fallito (errno=%d)", part_path, enc_path, errno);
        remove(part_path);
        return false;
    }

    remove(plain_path);
    ESP_LOGI(TAG, "Cifratura MP4 %ld bytes in %lld ms (buf=%u)",
             (long)st.st_size, (long long)((esp_timer_get_time() - t0) / 1000), (unsigned)io_buf_size);
    return true;
}

int core_recorder_recover_tmp_files(const core_settings_t *settings) {
    if (!settings) {
        return 0;
    }
    if (!core_sd_is_mounted() && !core_sd_init()) {
        ESP_LOGW(TAG, "Recupero .tmp: SD non montata");
        return 0;
    }

    DIR *root = opendir(CORE_SD_MOUNT_POINT);
    if (!root) {
        ESP_LOGW(TAG, "Recupero .tmp: opendir %s fallito (errno=%d)", CORE_SD_MOUNT_POINT, errno);
        return 0;
    }

    int recovered = 0;
    int removed = 0;
    struct dirent *day;
    while ((day = readdir(root)) != nullptr) {
        if (day->d_name[0] == '.' || day->d_name[0] == 0) {
            continue;
        }
        char daypath[96];
        snprintf(daypath, sizeof(daypath), "%s/%.64s", CORE_SD_MOUNT_POINT, day->d_name);
        struct stat dst;
        if (stat(daypath, &dst) != 0 || !S_ISDIR(dst.st_mode)) {
            continue;
        }

        // Raccolgo prima i nomi .mp4.tmp, poi li elaboro: evita di modificare la
        // directory mentre la sto leggendo (comportamento fragile su FATFS).
        DIR *sub = opendir(daypath);
        if (!sub) {
            continue;
        }
        char (*names)[80] = (char (*)[80])calloc(32, 80);
        int n_names = 0;
        if (names) {
            struct dirent *f;
            while (n_names < 32 && (f = readdir(sub)) != nullptr) {
                size_t len = strlen(f->d_name);
                if (len >= 8 && len < 80 && strcmp(f->d_name + len - 8, ".mp4.tmp") == 0) {
                    snprintf(names[n_names], 80, "%s", f->d_name);
                    n_names++;
                }
            }
        }
        closedir(sub);
        if (!names) {
            continue;
        }

        for (int i = 0; i < n_names; ++i) {
            char tmp_path[180];
            snprintf(tmp_path, sizeof(tmp_path), "%.96s/%.78s", daypath, names[i]);
            char final_path[180];
            snprintf(final_path, sizeof(final_path), "%.*s",
                     (int)(strlen(tmp_path) - 4), tmp_path);  // toglie ".tmp"

            if (settings->aes_key_valid && recording_file_is_valid(tmp_path) &&
                plain_mp4_has_ftyp(tmp_path)) {
                if (encrypt_mp4_file(tmp_path, final_path, settings->aes_key)) {
                    ESP_LOGI(TAG, "Recuperato orfano: %s", final_path);
                    recovered++;
                } else {
                    ESP_LOGW(TAG, "Recupero fallito, elimino: %s", tmp_path);
                    remove(tmp_path);
                    removed++;
                }
            } else {
                ESP_LOGW(TAG, "TMP corrotto/incompleto, elimino: %s", tmp_path);
                remove(tmp_path);
                removed++;
            }
        }
        free(names);
    }
    closedir(root);

    if (recovered || removed) {
        ESP_LOGI(TAG, "Recupero .tmp: %d recuperati, %d eliminati", recovered, removed);
    } else {
        ESP_LOGI(TAG, "Recupero .tmp: nessun file orfano");
    }
    return recovered + removed;
}

static int storage_path_handler(esp_muxer_slice_info_t *info, void *ctx) {
    recorder_muxer_ctx_t *mux_ctx = (recorder_muxer_ctx_t *)ctx;
    snprintf(info->file_path, info->len, "%s", mux_ctx->path);
    return 0;
}

// ---------------------------------------------------------------------------
// File-writer cifrante del muxer: cifra AES-128-CTR on-the-fly mentre esp_muxer
// scrive l'MP4, con pieno controllo di open/write/seek (nessuno stdio, nessun
// VFS, nessun fstat sfasato). Header [MRBI][IV] a offset 0; l'MP4 logico parte
// al byte CORE_CRYPTO_FILE_HEADER. Compatibile in lettura con stream_decrypted_mp4.
// ---------------------------------------------------------------------------
static uint8_t s_mux_key[16];
static bool    s_mux_key_set = false;

typedef struct {
    int               fd;
    uint64_t          logical_pos;   // 0 = primo byte MP4
    core_crypto_ctx_t crypto;
    uint8_t          *scratch;       // buffer di cifratura (non muto il buffer del muxer)
    size_t            scratch_cap;
} enc_writer_t;

static void *enc_writer_open(char *path) {
    if (!s_mux_key_set) {
        ESP_LOGE(TAG, "Chiave muxer non impostata: apertura %s negata", path);
        return nullptr;
    }
    enc_writer_t *w = (enc_writer_t *)calloc(1, sizeof(*w));
    if (!w) {
        return nullptr;
    }
    w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (w->fd < 0) {
        ESP_LOGE(TAG, "open(%s) fallita (errno=%d)", path, errno);
        free(w);
        return nullptr;
    }
    w->scratch_cap = 16 * 1024;
    w->scratch = (uint8_t *)heap_caps_malloc(w->scratch_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!w->scratch) {
        w->scratch = (uint8_t *)malloc(w->scratch_cap);
    }
    uint8_t iv[16];
    esp_fill_random(iv, sizeof(iv));
    uint32_t magic = CORE_CRYPTO_MAGIC;
    if (!w->scratch ||
        write(w->fd, &magic, sizeof(magic)) != (ssize_t)sizeof(magic) ||
        write(w->fd, iv, sizeof(iv)) != (ssize_t)sizeof(iv) ||
        !core_crypto_init_ctx(&w->crypto, s_mux_key, iv)) {
        ESP_LOGE(TAG, "Init writer cifrante fallito: %s", path);
        if (w->scratch) free(w->scratch);
        close(w->fd);
        free(w);
        return nullptr;
    }
    w->logical_pos = 0;
    return w;
}

static int enc_writer_write(void *writer, void *buffer, int len) {
    enc_writer_t *w = (enc_writer_t *)writer;
    if (!w || len <= 0) {
        return len == 0 ? 0 : -1;
    }
    if (lseek(w->fd, (off_t)(w->logical_pos + CORE_CRYPTO_FILE_HEADER), SEEK_SET) < 0) {
        return -1;
    }
    const uint8_t *src = (const uint8_t *)buffer;
    int done = 0;
    while (done < len) {
        size_t n = (size_t)(len - done);
        if (n > w->scratch_cap) n = w->scratch_cap;
        memcpy(w->scratch, src + done, n);
        core_crypto_crypt_at(&w->crypto, w->scratch, n, w->logical_pos);
        ssize_t wr = write(w->fd, w->scratch, n);
        if (wr <= 0) {
            return done > 0 ? done : -1;
        }
        w->logical_pos += (uint64_t)wr;
        done += (int)wr;
        if ((size_t)wr < n) break;
    }
    return done;
}

static int enc_writer_seek(void *writer, uint64_t pos) {
    enc_writer_t *w = (enc_writer_t *)writer;
    if (!w) return -1;
    w->logical_pos = pos;
    return 0;
}

static int enc_writer_close(void *writer) {
    enc_writer_t *w = (enc_writer_t *)writer;
    if (!w) return -1;
    int r = close(w->fd);
    core_crypto_deinit_ctx(&w->crypto);
    if (w->scratch) free(w->scratch);
    free(w);
    return r;
}

static void core_recorder_set_mux_key(const uint8_t key[16]) {
    memcpy(s_mux_key, key, 16);
    s_mux_key_set = true;
}

static bool recording_file_is_valid(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < (off_t)REC_MIN_MP4_BYTES) {
        return false;
    }
    uint32_t magic = 0;
    FILE *fp = fopen(path, "rb");
    if (fp) {
        (void)fread(&magic, 1, sizeof(magic), fp);
        fclose(fp);
    }
    bool enc_ok = (magic == CORE_CRYPTO_MAGIC);
    ESP_LOGI(TAG, "MP4 %s: %s (%ld bytes, header=%s)", enc_ok ? "OK cifrato" : "SENZA header cifrato",
             path, (long)st.st_size, enc_ok ? "MRBI" : "assente");
    return true;
}

static void recorder_capture_cleanup(recorder_capture_t *rc) {
    if (!rc) {
        return;
    }
    if (rc->capture) {
        esp_capture_close(rc->capture);
        rc->capture = nullptr;
    }
    if (rc->vsrc) {
        free(rc->vsrc);
        rc->vsrc = nullptr;
    }
    esp_video_enc_unregister_default();
    rc->running = false;
}

static bool recorder_capture_start_psram(recorder_capture_t *rc) {
    memset(rc, 0, sizeof(*rc));
    rc->pts_base_us = -1;

    if (!core_video_init()) {
        ESP_LOGE(TAG, "core_video_init fallita");
        return false;
    }

    esp_video_enc_register_default();

    esp_capture_video_v4l2_src_cfg_t v4l2_cfg = {
        .buf_count = 4,
    };
    strncpy(v4l2_cfg.dev_name, CORE_VIDEO_DEV_NAME, sizeof(v4l2_cfg.dev_name) - 1);

    esp_capture_video_src_if_t *vsrc = esp_capture_new_video_v4l2_src(&v4l2_cfg);
    if (!vsrc) {
        ESP_LOGE(TAG, "esp_capture_new_video_v4l2_src fallita");
        esp_video_enc_unregister_default();
        return false;
    }

    esp_capture_cfg_t cap_cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_NONE,
        .audio_src = nullptr,
        .video_src = vsrc,
        .share_overlay = false,
    };

    esp_capture_handle_t capture = nullptr;
    esp_capture_err_t err = esp_capture_open(&cap_cfg, &capture);
    if (err != ESP_CAPTURE_ERR_OK || capture == nullptr) {
        ESP_LOGE(TAG, "esp_capture_open fallita: %d", (int)err);
        free(vsrc);
        esp_video_enc_unregister_default();
        return false;
    }

    esp_capture_sink_cfg_t sink_cfg = {
        .video_info = {
            .format_id = ESP_CAPTURE_FMT_ID_H264,
            .width = CORE_VIDEO_WIDTH,
            .height = CORE_VIDEO_HEIGHT,
            .fps = CORE_VIDEO_FPS,
        },
    };

    esp_capture_sink_handle_t sink = nullptr;
    err = esp_capture_sink_setup(capture, 0, &sink_cfg, &sink);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_sink_setup fallita: %d (%dx%d@%d)",
                 (int)err, CORE_VIDEO_WIDTH, CORE_VIDEO_HEIGHT, CORE_VIDEO_FPS);
        esp_capture_close(capture);
        free(vsrc);
        esp_video_enc_unregister_default();
        return false;
    }

    esp_capture_sink_enable(sink, ESP_CAPTURE_RUN_MODE_ALWAYS);

    err = esp_capture_start(capture);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_start fallita: %d", (int)err);
        esp_capture_close(capture);
        free(vsrc);
        esp_video_enc_unregister_default();
        return false;
    }

    rc->capture = capture;
    rc->sink = sink;
    rc->vsrc = vsrc;
    rc->running = true;

    ESP_LOGI(TAG, "Capture H264 %dx%d@%d avviato (buffer PSRAM)", CORE_VIDEO_WIDTH, CORE_VIDEO_HEIGHT, CORE_VIDEO_FPS);
    return true;
}

static void recorder_capture_stop(recorder_capture_t *rc) {
    if (!rc || !rc->running) {
        return;
    }
    esp_capture_stop(rc->capture);
    recorder_capture_cleanup(rc);
}

static bool recorder_muxer_feed_frame(recorder_muxer_t *muxer, const esp_capture_stream_frame_t *frame,
                                      uint32_t pts_ms) {
    esp_muxer_video_packet_t pkt = {
        .data = frame->data,
        .len = frame->size,
        .pts = pts_ms,
        .dts = pts_ms,
        .key_frame = core_h264_is_idr_nal(frame->data, (uint32_t)frame->size),
    };
    return esp_muxer_add_video_packet(muxer->handle, muxer->stream_index, &pkt) == ESP_MUXER_ERR_OK;
}

static bool recorder_muxer_start_from_buffer(recorder_muxer_t *muxer, const char *tmp_path,
                                             core_h264_buffer_t *buf) {
    if (!muxer || !tmp_path || !buf || buf->frame_count == 0) {
        return false;
    }

    uint8_t codec_spec[H264_CODEC_SPEC_MAX];
    int spec_len = 0;
    if (!core_h264_buffer_extract_h264_config(buf, codec_spec, sizeof(codec_spec), &spec_len)) {
        ESP_LOGW(TAG, "SPS/PPS non trovati nel buffer — attendo altri frame");
        return false;
    }

    mp4_muxer_register();

    static recorder_muxer_ctx_t mux_ctx;
    snprintf(mux_ctx.path, sizeof(mux_ctx.path), "%s", tmp_path);

    mp4_muxer_config_t mp4_cfg = {
        .base_config = {
            .muxer_type = ESP_MUXER_TYPE_MP4,
            .slice_duration = 3600000,
            .url_pattern = nullptr,
            .url_pattern_ex = storage_path_handler,
            .ctx = &mux_ctx,
            .ram_cache_size = CORE_MUXER_RAM_CACHE_BYTES,
            .no_key_frame_verify = true,
        },
        .display_in_order = true,
        .moov_before_mdat = true,
    };

    esp_muxer_handle_t handle = esp_muxer_open((esp_muxer_config_t *)&mp4_cfg, sizeof(mp4_cfg));
    if (!handle) {
        ESP_LOGE(TAG, "esp_muxer_open fallita");
        return false;
    }

    esp_muxer_file_writer_t writer = {
        .on_open  = enc_writer_open,
        .on_write = enc_writer_write,
        .on_seek  = enc_writer_seek,
        .on_close = enc_writer_close,
    };
    if (esp_muxer_set_file_writer(handle, &writer) != ESP_MUXER_ERR_OK) {
        esp_muxer_close(handle);
        ESP_LOGE(TAG, "esp_muxer_set_file_writer fallita");
        return false;
    }

    esp_muxer_video_stream_info_t vinfo = {
        .codec = ESP_MUXER_VDEC_H264,
        .width = CORE_VIDEO_WIDTH,
        .height = CORE_VIDEO_HEIGHT,
        .fps = CORE_VIDEO_FPS,
        .min_packet_duration = 1000 / CORE_VIDEO_FPS,
        .codec_spec_info = codec_spec,
        .spec_info_len = spec_len,
    };

    int stream_index = -1;
    if (esp_muxer_add_video_stream(handle, &vinfo, &stream_index) != ESP_MUXER_ERR_OK) {
        esp_muxer_close(handle);
        ESP_LOGE(TAG, "esp_muxer_add_video_stream fallita");
        return false;
    }

    for (core_h264_frame_t *f = buf->head; f; f = f->next) {
        esp_muxer_video_packet_t pkt = {
            .data = f->data,
            .len = (int)f->size,
            .pts = f->pts_ms,
            .dts = f->pts_ms,
            .key_frame = core_h264_is_idr_nal(f->data, f->size),
        };
        if (esp_muxer_add_video_packet(handle, stream_index, &pkt) != ESP_MUXER_ERR_OK) {
            esp_muxer_close(handle);
            ESP_LOGE(TAG, "Replay buffer→SD fallita");
            return false;
        }
    }

    muxer->handle = handle;
    muxer->stream_index = stream_index;
    muxer->active = true;
    ESP_LOGI(TAG, "Mux MP4 su SD: %s (%lu frame replay, %lu bytes)", tmp_path,
             (unsigned long)buf->frame_count, (unsigned long)buf->total_bytes);
    core_h264_buffer_clear(buf);
    return true;
}

static void recorder_muxer_close(recorder_muxer_t *muxer) {
    if (muxer && muxer->active && muxer->handle) {
        esp_muxer_close(muxer->handle);
        muxer->handle = nullptr;
        muxer->active = false;
    }
}

static bool recorder_process_frame(recorder_capture_t *capture, recorder_muxer_t *muxer,
                                   core_h264_buffer_t *psram_buf, uint32_t *dropped_frames) {
    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
    };
    esp_capture_err_t err = esp_capture_sink_acquire_frame(capture->sink, &frame, true);
    if (err != ESP_CAPTURE_ERR_OK || !frame.data || frame.size <= 0) {
        return false;
    }

    uint32_t pts_ms = recorder_next_pts_ms(capture);
    bool ok = true;
    if (!muxer->active) {
        if (!core_h264_buffer_push(psram_buf, pts_ms, frame.data, (uint32_t)frame.size)) {
            if (dropped_frames) {
                (*dropped_frames)++;
            }
            ok = false;
        }
    } else if (!recorder_muxer_feed_frame(muxer, &frame, pts_ms)) {
        ESP_LOGW(TAG, "Scrittura frame su SD fallita");
        ok = false;
    }

    esp_capture_sink_release_frame(capture->sink, &frame);
    return ok;
}

static void recorder_drain_capture(recorder_capture_t *capture, recorder_muxer_t *muxer,
                                   core_h264_buffer_t *psram_buf, uint32_t timeout_ms) {
    if (!capture || !capture->running) {
        return;
    }
    int64_t until = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (esp_timer_get_time() < until) {
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
        };
        esp_capture_err_t err = esp_capture_sink_acquire_frame(capture->sink, &frame, false);
        if (err != ESP_CAPTURE_ERR_OK || !frame.data || frame.size <= 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        uint32_t pts_ms = recorder_next_pts_ms(capture);
        if (!muxer->active) {
            core_h264_buffer_push(psram_buf, pts_ms, frame.data, (uint32_t)frame.size);
        } else if (!recorder_muxer_feed_frame(muxer, &frame, pts_ms)) {
            ESP_LOGW(TAG, "Scrittura frame su SD fallita (drain)");
        }
        esp_capture_sink_release_frame(capture->sink, &frame);
    }
}

static bool recorder_try_sd_handoff(recorder_muxer_t *muxer, const sd_prep_ctx_t *sd_ctx,
                                    core_h264_buffer_t *psram_buf, bool capture_ok,
                                    bool sd_prep_done, bool *muxer_was_active) {
    if (!muxer || !sd_ctx || !psram_buf || muxer->active || !sd_prep_done ||
        !sd_ctx->paths_ok || !capture_ok || psram_buf->frame_count == 0) {
        return false;
    }
    if (recorder_muxer_start_from_buffer(muxer, sd_ctx->final_path, psram_buf)) {
        if (muxer_was_active) {
            *muxer_was_active = true;
        }
        return true;
    }
    return false;
}

static void sd_prep_task(void *arg) {
    sd_prep_ctx_t *ctx = (sd_prep_ctx_t *)arg;
    int64_t t0 = esp_timer_get_time();

    ctx->sd_mounted = core_sd_init();
    if (ctx->sd_mounted) {
        if (core_sd_ensure_space(CORE_SD_MIN_FREE_BYTES)) {
            char day_dir[64];
            if (core_sd_make_day_dir(day_dir, sizeof(day_dir))) {
                if (core_sd_make_recording_path(day_dir, ctx->core_id, ctx->final_path, sizeof(ctx->final_path))) {
                    snprintf(ctx->tmp_path, sizeof(ctx->tmp_path), "%s.tmp", ctx->final_path);
                    ctx->paths_ok = true;
                    ESP_LOGI(TAG, "Path registrazione: %s (MP4 cifrato on-the-fly dal writer del muxer)",
                             ctx->final_path);
                } else {
                    ESP_LOGE(TAG, "Generazione path file fallita");
                }
            } else {
                ESP_LOGE(TAG, "Creazione directory giorno fallita");
            }
        } else {
            ESP_LOGE(TAG, "Spazio SD insufficiente (min %llu MB)",
                     (unsigned long long)(CORE_SD_MIN_FREE_BYTES / (1024ULL * 1024ULL)));
        }
    } else {
        ESP_LOGE(TAG, "Mount SD fallito in sd_prep");
    }

    ESP_LOGI(TAG, "SD prep completata in %lld ms (mount=%d paths=%d)",
             (long long)((esp_timer_get_time() - t0) / 1000), ctx->sd_mounted, ctx->paths_ok);
    xEventGroupSetBits(ctx->ev, SD_PREP_DONE_BIT);
    vTaskDelete(nullptr);
}

static volatile bool s_manual_stop = false;
static volatile bool s_manual_active = false;
static volatile bool s_manual_last_saved = false;
static volatile uint32_t s_manual_frame_count = 0;
static TaskHandle_t s_manual_task = nullptr;
static EventGroupHandle_t s_manual_ev = nullptr;
static char s_manual_last_path[128] = {0};
static char s_manual_error[96] = {0};

typedef struct {
    core_settings_t settings;
} manual_rec_arg_t;

static bool manual_should_stop(void) {
    return s_manual_stop;
}

static bool rec_stop_should_stop(void) {
    return core_gpio_is_rec_stop_triggered();
}

static bool recorder_finalize_save(const core_settings_t *settings, const sd_prep_ctx_t *sd_ctx,
                                   bool capture_ok, bool muxer_was_active, uint32_t dropped_frames,
                                   bool async_encrypt, char *out_path, size_t out_path_len) {
    bool saved = false;

    if (!sd_ctx->paths_ok) {
        ESP_LOGW(TAG, "Nessun file salvato (capture=%d sd=%d paths=%d mux=%d drop=%lu)",
                 capture_ok, sd_ctx->sd_mounted, sd_ctx->paths_ok, muxer_was_active,
                 (unsigned long)dropped_frames);
        return false;
    }

    if (!core_sd_is_mounted() && !core_sd_init()) {
        ESP_LOGE(TAG, "SD non montata in finalize");
        return false;
    }

    (void)async_encrypt;  // cifratura ora on-the-fly nel VFS: nessun secondo passaggio

    // Il muxer ha già scritto il file CIFRATO direttamente su final_path (via /enc).
    // Qui basta validarlo: dimensione >= header crypto + minimo MP4.
    struct stat st;
    if (stat(sd_ctx->final_path, &st) != 0) {
        ESP_LOGW(TAG, "File registrazione assente: %s (mux=%d)", sd_ctx->final_path, muxer_was_active);
    } else if (st.st_size < (off_t)(CORE_CRYPTO_FILE_HEADER + REC_MIN_MP4_BYTES)) {
        ESP_LOGW(TAG, "File registrazione troppo piccolo/corrotto: %s (%ld byte) — elimino",
                 sd_ctx->final_path, (long)st.st_size);
        remove(sd_ctx->final_path);
    } else {
        saved = true;
        ESP_LOGI(TAG, "File salvato cifrato (real-time): %s (%ld bytes)",
                 sd_ctx->final_path, (long)st.st_size);
        if (out_path && out_path_len > 0) {
            snprintf(out_path, out_path_len, "%s", sd_ctx->final_path);
        }
    }

    if (!saved) {
        ESP_LOGW(TAG, "Nessun file salvato (capture=%d sd=%d paths=%d mux=%d drop=%lu)",
                 capture_ok, sd_ctx->sd_mounted, sd_ctx->paths_ok, muxer_was_active,
                 (unsigned long)dropped_frames);
    }
    return saved;
}

static bool recorder_run_core(const core_settings_t *settings, recorder_should_stop_fn_t should_stop,
                              bool async_encrypt, const char *stop_label,
                              char *out_path, size_t out_path_len) {
    if (!settings || !should_stop) {
        return false;
    }

    int64_t boot_t0 = esp_timer_get_time();

    sd_prep_ctx_t sd_ctx = {
        .ev = xEventGroupCreate(),
        .core_id = settings->core_id,
    };
    if (!sd_ctx.ev) {
        ESP_LOGE(TAG, "EventGroup SD fallito");
        return false;
    }

    core_gpio_rec_stop_session_begin();

    // Chiave per il writer cifrante del muxer: MP4 scritto già cifrato su SD (real-time).
    if (settings->aes_key_valid) {
        core_recorder_set_mux_key(settings->aes_key);
    } else {
        ESP_LOGE(TAG, "Chiave AES assente — impossibile cifrare la registrazione");
        vEventGroupDelete(sd_ctx.ev);
        return false;
    }

    recorder_capture_t capture = {};
    core_h264_buffer_t psram_buf;
    core_h264_buffer_init(&psram_buf, CORE_H264_PSRAM_MAX_BYTES);

    bool capture_ok = recorder_capture_start_psram(&capture);
    if (!capture_ok) {
        ESP_LOGE(TAG, "Capture non avviato");
    } else {
        core_status_led_set_mode(CORE_LED_RECORDING);
        ESP_LOGI(TAG, "Registrazione PSRAM attiva in %lld ms (%lld ms da accensione)",
                 (long long)((esp_timer_get_time() - boot_t0) / 1000),
                 (long long)(esp_timer_get_time() / 1000));
    }

    xTaskCreate(sd_prep_task, "sd_prep", CORE_SD_PREP_TASK_STACK, &sd_ctx, CORE_SD_PREP_TASK_PRIO, nullptr);

    recorder_muxer_t muxer = {};
    bool sd_prep_done = false;
    bool muxer_was_active = false;
    uint32_t dropped_frames = 0;
    int64_t last_status_log = esp_timer_get_time();

    ESP_LOGI(TAG, "Registrazione attiva — stop: %s", stop_label ? stop_label : "?");
    bool first_frame_logged = false;
    while (!should_stop()) {
        core_gpio_log_pin_edges();
        if (capture_ok) {
            recorder_process_frame(&capture, &muxer, &psram_buf, &dropped_frames);
        }
        if (!first_frame_logged && psram_buf.frame_count > 0) {
            first_frame_logged = true;
            ESP_LOGI(TAG, "Primo frame catturato a %lld ms da accensione",
                     (long long)(esp_timer_get_time() / 1000));
        }

        if (!sd_prep_done && (xEventGroupGetBits(sd_ctx.ev) & SD_PREP_DONE_BIT)) {
            sd_prep_done = true;
        }

        if (recorder_try_sd_handoff(&muxer, &sd_ctx, &psram_buf, capture_ok, sd_prep_done,
                                    &muxer_was_active)) {
            ESP_LOGI(TAG, "Passaggio PSRAM→SD in %lld ms",
                     (long long)((esp_timer_get_time() - boot_t0) / 1000));
        }

        int64_t now = esp_timer_get_time();
        if (now - last_status_log >= ((int64_t)REC_STATUS_LOG_MS * 1000)) {
            last_status_log = now;
            s_manual_frame_count = psram_buf.frame_count;
            ESP_LOGI(TAG, "Stato rec: buf=%lu frame/%lu B, sd=%d paths=%d mux=%d drop=%lu",
                     (unsigned long)psram_buf.frame_count, (unsigned long)psram_buf.total_bytes,
                     sd_ctx.sd_mounted, sd_ctx.paths_ok, muxer.active, (unsigned long)dropped_frames);
        }

        if (!capture_ok) {
            vTaskDelay(pdMS_TO_TICKS(50));
        } else if (!muxer.active && psram_buf.frame_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    ESP_LOGI(TAG, "Stop registrazione (%s) — %lld ms", stop_label ? stop_label : "?", 
             (long long)((esp_timer_get_time() - boot_t0) / 1000));

    core_gpio_rec_stop_session_end();

    if (!sd_prep_done) {
        xEventGroupWaitBits(sd_ctx.ev, SD_PREP_DONE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
        sd_prep_done = true;
    }

    if (capture_ok) {
        recorder_drain_capture(&capture, &muxer, &psram_buf, REC_DRAIN_TIMEOUT_MS);
    }

    if (recorder_try_sd_handoff(&muxer, &sd_ctx, &psram_buf, capture_ok, sd_prep_done,
                                &muxer_was_active)) {
        ESP_LOGI(TAG, "Passaggio PSRAM→SD (ultima chance) in %lld ms",
                 (long long)((esp_timer_get_time() - boot_t0) / 1000));
        if (capture_ok) {
            recorder_drain_capture(&capture, &muxer, &psram_buf, REC_DRAIN_TIMEOUT_MS);
        }
    }

    if (capture_ok) {
        recorder_capture_stop(&capture);
    }
    recorder_muxer_close(&muxer);
    core_h264_buffer_clear(&psram_buf);
    vEventGroupDelete(sd_ctx.ev);

    core_status_led_set_mode(CORE_LED_OFF);

    return recorder_finalize_save(settings, &sd_ctx, capture_ok, muxer_was_active, dropped_frames,
                                  async_encrypt, out_path, out_path_len);
}

static void manual_rec_task(void *arg) {
    manual_rec_arg_t *ctx = (manual_rec_arg_t *)arg;
    char saved_path[128] = {0};
    bool saved = recorder_run_core(&ctx->settings, manual_should_stop, true, "web stop",
                                   saved_path, sizeof(saved_path));
    s_manual_last_saved = saved;
    if (saved) {
        snprintf(s_manual_last_path, sizeof(s_manual_last_path), "%s", saved_path);
        s_manual_error[0] = 0;
    } else {
        snprintf(s_manual_error, sizeof(s_manual_error), "Registrazione non salvata");
    }
    free(ctx);
    s_manual_task = nullptr;
    s_manual_active = false;
    if (s_manual_ev) {
        xEventGroupSetBits(s_manual_ev, MANUAL_REC_DONE_BIT);
    }
    vTaskDelete(nullptr);
}

bool core_recorder_manual_start(const core_settings_t *settings) {
    if (!settings) {
        return false;
    }
    if (s_manual_active || s_manual_task) {
        ESP_LOGW(TAG, "Registrazione manuale già attiva");
        return false;
    }
    if (!s_manual_ev) {
        s_manual_ev = xEventGroupCreate();
        if (!s_manual_ev) {
            return false;
        }
    }

    manual_rec_arg_t *arg = (manual_rec_arg_t *)malloc(sizeof(manual_rec_arg_t));
    if (!arg) {
        return false;
    }
    memcpy(&arg->settings, settings, sizeof(*settings));

    s_manual_stop = false;
    s_manual_last_saved = false;
    s_manual_frame_count = 0;
    s_manual_last_path[0] = 0;
    s_manual_error[0] = 0;
    xEventGroupClearBits(s_manual_ev, MANUAL_REC_DONE_BIT);

    if (xTaskCreate(manual_rec_task, "web_rec", MANUAL_REC_TASK_STACK, arg,
                    MANUAL_REC_TASK_PRIO, &s_manual_task) != pdPASS) {
        free(arg);
        return false;
    }
    s_manual_active = true;
    ESP_LOGI(TAG, "Registrazione manuale avviata da Web");
    return true;
}

bool core_recorder_manual_stop(void) {
    if (!s_manual_active) {
        return false;
    }
    s_manual_stop = true;
    return true;
}

bool core_recorder_manual_wait_done(uint32_t timeout_ms) {
    if (!s_manual_ev) {
        return !s_manual_active;
    }
    if (s_manual_active) {
        xEventGroupWaitBits(s_manual_ev, MANUAL_REC_DONE_BIT, pdTRUE, pdTRUE,
                            pdMS_TO_TICKS(timeout_ms));
    }
    return !s_manual_active;
}

bool core_recorder_manual_is_active(void) {
    return s_manual_active;
}

void core_recorder_manual_get_status(core_recorder_manual_status_t *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->active = s_manual_active;
    out->last_saved = s_manual_last_saved;
    out->frame_count = s_manual_frame_count;
    snprintf(out->last_path, sizeof(out->last_path), "%s", s_manual_last_path);
    snprintf(out->error, sizeof(out->error), "%s", s_manual_error);
}

bool core_recorder_run_session(const core_settings_t *settings) {
    if (!settings) {
        core_gpio_hold_tpl_done(CORE_D2_POST_DELAY_MS);
        return false;
    }

    core_gpio_log_inputs();
    bool saved = recorder_run_core(settings, rec_stop_should_stop, false, "stop GPIO",
                                     nullptr, 0);
    core_gpio_hold_tpl_done(settings->d2_post_delay_ms);
    return saved;
}

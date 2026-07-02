#include "CoreRecorder.h"
#include "CoreConfig.h"
#include "CoreCrypto.h"
#include "CoreEncFs.h"
#include "CoreGPIO.h"
#include "CoreH264Buffer.h"
#include "CoreSD.h"
#include "CoreStatusLed.h"
#include "CoreVideo.h"

#include "encoder/esp_video_enc_default.h"
#include "esp_capture.h"
#include "esp_capture_advance.h"
#include "esp_capture_sink.h"
#include "esp_gmf_video_enc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
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
#define REC_FINAL_FLUSH_STOP_MS 2000   // max flush residuo (solo se stop non utente)
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
    uint32_t warmup_left;
    uint32_t kept_frames;
    uint32_t pts_epoch_ms;
    uint16_t video_width;
    uint16_t video_height;
    uint8_t video_fps;
    uint32_t frame_dur_ms;
    uint32_t video_bitrate;
    uint32_t video_gop;
} recorder_capture_t;

static bool recorder_capture_warmup(recorder_capture_t *rc) {
    if (rc->warmup_left > 0) {
        rc->warmup_left--;
        return true;
    }
    return false;
}

static uint32_t recorder_capture_next_pts(const recorder_capture_t *rc) {
    return rc->pts_epoch_ms + rc->kept_frames * rc->frame_dur_ms;
}

static bool recorder_frame_accept(recorder_capture_t *rc, uint32_t *out_pts_ms) {
    if (recorder_capture_warmup(rc)) {
        return false;
    }
    if (rc->kept_frames == 0) {
        ESP_LOGI(TAG, "Warmup completato — registrazione utile da PTS 0");
    }
    *out_pts_ms = recorder_capture_next_pts(rc);
    rc->kept_frames++;
    return true;
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
    uint64_t          real_fd_pos;   // posizione fisica corrente su SD (evita lseek ridondanti)
    core_crypto_ctx_t crypto;
    uint8_t          *scratch;
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
    w->scratch_cap = 128 * 1024;
    w->scratch = (uint8_t *)heap_caps_malloc(w->scratch_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    w->real_fd_pos = CORE_CRYPTO_FILE_HEADER;
    return w;
}

static int enc_writer_write(void *writer, void *buffer, int len) {
    enc_writer_t *w = (enc_writer_t *)writer;
    if (!w || len <= 0) {
        return len == 0 ? 0 : -1;
    }
    const uint8_t *src = (const uint8_t *)buffer;
    int done = 0;
    while (done < len) {
        uint64_t phys = w->logical_pos + CORE_CRYPTO_FILE_HEADER;
        if (w->real_fd_pos != phys) {
            if (lseek(w->fd, (off_t)phys, SEEK_SET) < 0) {
                return done > 0 ? done : -1;
            }
            w->real_fd_pos = phys;
        }
        size_t n = (size_t)(len - done);
        if (n > w->scratch_cap) n = w->scratch_cap;
        memcpy(w->scratch, src + done, n);
        core_crypto_crypt_at(&w->crypto, w->scratch, n, w->logical_pos);
        ssize_t wr = write(w->fd, w->scratch, n);
        if (wr <= 0) {
            static int64_t s_last_enc_wr_us = 0;
            int64_t now = esp_timer_get_time();
            if (now - s_last_enc_wr_us >= 1000000) {
                s_last_enc_wr_us = now;
                ESP_LOGW(TAG, "enc_writer_write fallita su SD (errno=%d, done=%d)", errno, done);
            }
            return done > 0 ? done : -1;
        }
        w->logical_pos += (uint64_t)wr;
        w->real_fd_pos += (uint64_t)wr;
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

static esp_capture_err_t rec_capture_pipeline_event(esp_capture_event_t event, void *ctx) {
    if (event != ESP_CAPTURE_EVENT_VIDEO_PIPELINE_BUILT || !ctx) {
        return ESP_CAPTURE_ERR_OK;
    }
    recorder_capture_t *rc = (recorder_capture_t *)ctx;
    if (!rc->sink) {
        return ESP_CAPTURE_ERR_OK;
    }
    esp_gmf_element_handle_t venc = nullptr;
    esp_capture_err_t ret = esp_capture_sink_get_element_by_tag(
        rc->sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, "vid_enc", &venc);
    if (ret != ESP_CAPTURE_ERR_OK || !venc) {
        ESP_LOGW(TAG, "vid_enc non trovato — GOP/QP default");
        return ESP_CAPTURE_ERR_OK;
    }
    esp_gmf_video_enc_set_bitrate(venc, rc->video_bitrate);
    esp_gmf_video_enc_set_gop(venc, rc->video_gop);
    esp_gmf_video_enc_set_qp(venc, CORE_VIDEO_QP_MIN, CORE_VIDEO_QP_MAX);
    ESP_LOGI(TAG, "Encoder H264: %u bps, GOP=%u, QP [%u-%u]",
             (unsigned)rc->video_bitrate, (unsigned)rc->video_gop,
             (unsigned)CORE_VIDEO_QP_MIN, (unsigned)CORE_VIDEO_QP_MAX);
    return ESP_CAPTURE_ERR_OK;
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

static bool recorder_capture_start_psram(recorder_capture_t *rc, uint32_t warmup_frames,
                                         const core_settings_t *settings) {
    memset(rc, 0, sizeof(*rc));
    rc->warmup_left = warmup_frames;
    core_settings_t vcfg = settings ? *settings : core_settings_t{};
    if (!settings) {
        core_settings_video_set_defaults(&vcfg);
    } else {
        core_settings_video_normalize(&vcfg);
    }
    rc->video_width = vcfg.video_width;
    rc->video_height = vcfg.video_height;
    rc->video_fps = vcfg.video_fps;
    rc->frame_dur_ms = core_settings_video_frame_ms(&vcfg);
    rc->video_bitrate = core_settings_video_bitrate(&vcfg);
    rc->video_gop = core_settings_video_gop(&vcfg);

    if (!core_video_init()) {
        ESP_LOGW(TAG, "core_video_init fallita — retry dopo deinit");
        core_video_deinit();
        vTaskDelay(pdMS_TO_TICKS(300));
        if (!core_video_init()) {
            ESP_LOGE(TAG, "core_video_init fallita");
            return false;
        }
    }

    esp_video_enc_register_default();

    esp_capture_video_v4l2_src_cfg_t v4l2_cfg = {
        .buf_count = CORE_V4L2_BUF_COUNT,
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
            .width = rc->video_width,
            .height = rc->video_height,
            .fps = rc->video_fps,
        },
    };

    esp_capture_sink_handle_t sink = nullptr;
    err = esp_capture_sink_setup(capture, 0, &sink_cfg, &sink);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_sink_setup fallita: %d (%dx%d@%d)",
                 (int)err, rc->video_width, rc->video_height, rc->video_fps);
        esp_capture_close(capture);
        free(vsrc);
        esp_video_enc_unregister_default();
        return false;
    }

    esp_capture_sink_set_bitrate(sink, ESP_CAPTURE_STREAM_TYPE_VIDEO, rc->video_bitrate);

    rc->capture = capture;
    rc->sink = sink;
    rc->vsrc = vsrc;

    err = esp_capture_set_event_cb(capture, rec_capture_pipeline_event, rc);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGW(TAG, "esp_capture_set_event_cb fallita: %d", (int)err);
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

    rc->running = true;

    ESP_LOGI(TAG, "Capture H264 %dx%d@%d avviato (buffer PSRAM, GOP=%u, %u bps)",
             rc->video_width, rc->video_height, rc->video_fps,
             (unsigned)rc->video_gop, (unsigned)rc->video_bitrate);
    return true;
}

static void recorder_capture_stop(recorder_capture_t *rc) {
    if (!rc || !rc->running) {
        return;
    }
    esp_capture_stop(rc->capture);
    recorder_capture_cleanup(rc);
}

// Attende il primo frame H264 dalla pipeline. Serve a due cose:
// 1. l'encoder HW e il PPA allocano i buffer di lavoro solo al primo frame
//    (~3-6 MB in PSRAM a seconda della risoluzione): vanno allocati PRIMA
//    di dimensionare il ring, che si prende il 95% della PSRAM residua;
// 2. se l'encoder muore (OOM o errore pipeline), lo scopriamo subito invece
//    di registrare 0 frame per tutta la sessione.
static bool recorder_capture_prime(recorder_capture_t *rc, uint32_t timeout_ms) {
    int64_t until = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (esp_timer_get_time() < until) {
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
        };
        esp_capture_err_t err = esp_capture_sink_acquire_frame(rc->sink, &frame, true);
        if (err == ESP_CAPTURE_ERR_OK && frame.data && frame.size > 0) {
            esp_capture_sink_release_frame(rc->sink, &frame);
            ESP_LOGI(TAG, "Pipeline H264 pronta (primo frame %lu byte) — buffer encoder allocati",
                     (unsigned long)frame.size);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGE(TAG, "Pipeline H264 non produce frame entro %lu ms (encoder OOM o pipeline morta)",
             (unsigned long)timeout_ms);
    return false;
}

static bool recorder_muxer_start_from_buffer(recorder_muxer_t *muxer, const char *tmp_path,
                                             core_h264_buffer_t *buf, uint32_t *out_replayed,
                                             uint16_t width, uint16_t height, uint8_t fps) {
    if (out_replayed) {
        *out_replayed = 0;
    }
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
        .width = width,
        .height = height,
        .fps = fps,
        .min_packet_duration = fps > 0 ? (uint32_t)(1000 / fps) : 83U,
        .codec_spec_info = codec_spec,
        .spec_info_len = spec_len,
    };

    int stream_index = -1;
    if (esp_muxer_add_video_stream(handle, &vinfo, &stream_index) != ESP_MUXER_ERR_OK) {
        esp_muxer_close(handle);
        ESP_LOGE(TAG, "esp_muxer_add_video_stream fallita");
        return false;
    }

    muxer->handle = handle;
    muxer->stream_index = stream_index;
    muxer->active = true;
    ESP_LOGI(TAG, "Mux MP4 su SD: %s (buffer PSRAM %lu KB, ~%u s)",
             tmp_path, (unsigned long)(core_h264_buffer_bytes_used(buf) / 1024),
             (unsigned)core_h264_buffer_estimated_sec(buf));
    return true;
}

static void recorder_discard_psram_until_idr(core_h264_buffer_t *buf) {
    if (!buf) {
        return;
    }
    while (buf->head &&
           !core_h264_is_idr_nal(core_h264_frame_data(buf, buf->head), buf->head->size)) {
        core_h264_buffer_pop_head(buf);
    }
}

static uint32_t recorder_drain_psram_to_muxer(recorder_muxer_t *muxer, core_h264_buffer_t *buf,
                                              recorder_should_stop_fn_t should_stop) {
    if (!muxer || !muxer->active || !buf) {
        return 0;
    }
    uint32_t drained = 0;
    while (buf->head) {
        if (should_stop && should_stop()) {
            break;
        }
        core_h264_frame_t *f = buf->head;
        esp_muxer_video_packet_t pkt = {
            .data = (uint8_t *)core_h264_frame_data(buf, f),
            .len = (int)f->size,
            .pts = f->pts_ms,
            .dts = f->pts_ms,
            .key_frame = core_h264_is_idr_nal(core_h264_frame_data(buf, f), f->size),
        };
        if (esp_muxer_add_video_packet(muxer->handle, muxer->stream_index, &pkt) != ESP_MUXER_ERR_OK) {
            break;
        }
        core_h264_buffer_pop_head(buf);
        drained++;
    }
    return drained;
}

static void recorder_muxer_close(recorder_muxer_t *muxer) {
    if (muxer && muxer->active && muxer->handle) {
        esp_muxer_close(muxer->handle);
        muxer->handle = nullptr;
        muxer->active = false;
    }
}

static bool recorder_ensure_muxer_open(recorder_muxer_t *muxer, const char *path,
                                       core_h264_buffer_t *psram_buf,
                                       const recorder_capture_t *capture);

static bool recorder_muxer_write_packet(recorder_muxer_t *muxer, const uint8_t *data, uint32_t size,
                                        uint32_t pts_ms, uint32_t *muxer_frames) {
    if (!muxer || !muxer->active || !data || size == 0) {
        return false;
    }
    esp_muxer_video_packet_t pkt = {
        .data = (uint8_t *)data,
        .len = (int)size,
        .pts = pts_ms,
        .dts = pts_ms,
        .key_frame = core_h264_is_idr_nal(data, size),
    };
    esp_muxer_err_t err = esp_muxer_add_video_packet(muxer->handle, muxer->stream_index, &pkt);
    if (err != ESP_MUXER_ERR_OK) {
        static int64_t s_last_mux_err_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - s_last_mux_err_us >= 1000000) {
            s_last_mux_err_us = now;
            ESP_LOGW(TAG, "esp_muxer_add_video_packet fallita: err=%d pts=%lu size=%lu",
                     (int)err, (unsigned long)pts_ms, (unsigned long)size);
        }
        return false;
    }
    if (muxer_frames) {
        (*muxer_frames)++;
    }
    return true;
}

static bool recorder_stream_frame_to_muxer(recorder_muxer_t *muxer, const sd_prep_ctx_t *sd_ctx,
                                             bool sd_ready, core_h264_buffer_t *staging,
                                             const recorder_capture_t *capture,
                                             bool *muxer_was_active, uint32_t *muxer_frames,
                                             uint32_t *dropped_frames, const uint8_t *data,
                                             uint32_t size, uint32_t pts_ms) {
    if (!sd_ready || !sd_ctx->paths_ok) {
        if (!core_h264_buffer_push(staging, pts_ms, data, size)) {
            if (dropped_frames) {
                (*dropped_frames)++;
            }
            ESP_LOGW(TAG, "Staging pieno (SD non pronta) — frame perso");
            return false;
        }
        return true;
    }

    if (!muxer->active) {
        if (!core_h264_buffer_push(staging, pts_ms, data, size)) {
            if (dropped_frames) {
                (*dropped_frames)++;
            }
            return false;
        }
        if (!core_h264_buffer_has_idr(staging)) {
            return true;
        }
        if (!recorder_ensure_muxer_open(muxer, sd_ctx->final_path, staging, capture)) {
            ESP_LOGW(TAG, "Muxer non pronto (SPS/PPS/IDR)");
            return false;
        }
        if (muxer_was_active) {
            *muxer_was_active = true;
        }
        uint32_t n = recorder_drain_psram_to_muxer(muxer, staging, nullptr);
        if (muxer_frames) {
            *muxer_frames += n;
        }
        return true;
    }

    if (!recorder_muxer_write_packet(muxer, data, size, pts_ms, muxer_frames)) {
        if (!core_h264_buffer_push(staging, pts_ms, data, size)) {
            if (dropped_frames) {
                (*dropped_frames)++;
            }
            ESP_LOGW(TAG, "Scrittura SD fallita e staging pieno — frame perso");
        }
        return false;
    }

    if (staging->frame_count > 0) {
        uint32_t n = recorder_drain_psram_to_muxer(muxer, staging, nullptr);
        if (muxer_frames) {
            *muxer_frames += n;
        }
    }
    return true;
}

static bool recorder_process_frame_stream(recorder_capture_t *capture, recorder_muxer_t *muxer,
                                          const sd_prep_ctx_t *sd_ctx, bool sd_ready,
                                          core_h264_buffer_t *staging, bool *muxer_was_active,
                                          uint32_t *muxer_frames, uint32_t *dropped_frames,
                                          bool no_wait) {
    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
    };
    esp_capture_err_t err = esp_capture_sink_acquire_frame(capture->sink, &frame, no_wait);
    if (err != ESP_CAPTURE_ERR_OK || !frame.data || frame.size <= 0) {
        return false;
    }

    uint32_t pts_ms = 0;
    if (!recorder_frame_accept(capture, &pts_ms)) {
        esp_capture_sink_release_frame(capture->sink, &frame);
        return true;
    }

    (void)recorder_stream_frame_to_muxer(muxer, sd_ctx, sd_ready, staging, capture,
                                         muxer_was_active, muxer_frames, dropped_frames,
                                         frame.data, (uint32_t)frame.size, pts_ms);
    esp_capture_sink_release_frame(capture->sink, &frame);
    return true;
}

static void recorder_drain_capture_to_muxer(recorder_capture_t *capture, recorder_muxer_t *muxer,
                                            const sd_prep_ctx_t *sd_ctx, bool sd_ready,
                                            core_h264_buffer_t *staging, bool *muxer_was_active,
                                            uint32_t *muxer_frames, uint32_t *dropped_frames) {
    (void)capture;
    (void)muxer;
    (void)sd_ctx;
    (void)sd_ready;
    (void)staging;
    (void)muxer_was_active;
    (void)muxer_frames;
    (void)dropped_frames;
}

typedef struct {
    recorder_capture_t       *capture;
    recorder_muxer_t         *muxer;
    sd_prep_ctx_t            *sd_ctx;
    core_h264_buffer_t       *ring;
    SemaphoreHandle_t         ring_mtx;
    recorder_should_stop_fn_t should_stop;
    volatile bool             capture_done;
    volatile bool             sd_done;
    volatile bool             sd_prep_done;
    volatile bool             user_stop;   // stop D2/web: no drain encoder, flush ring
    bool                     *muxer_was_active;
    volatile uint32_t         muxer_frames;
    volatile uint32_t         dropped_frames;
    volatile uint32_t         producer_pts_max;
    int64_t                   last_ring_warn_us;
} rec_dual_job_ctx_t;

static uint32_t rec_ring_fill_pct(rec_dual_job_ctx_t *ctx) {
    uint32_t pct = 0;
    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    pct = core_h264_buffer_fill_percent(ctx->ring);
    xSemaphoreGive(ctx->ring_mtx);
    return pct;
}

static bool rec_capture_push_ring(rec_dual_job_ctx_t *ctx, const uint8_t *data, uint32_t size,
                                  uint32_t pts_ms) {
    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    bool ok = core_h264_buffer_push(ctx->ring, pts_ms, data, size);
    if (ok) {
        ctx->producer_pts_max = pts_ms;
    } else {
        ctx->dropped_frames++;
        int64_t now = esp_timer_get_time();
        if (now - ctx->last_ring_warn_us >= 1000000) {
            ctx->last_ring_warn_us = now;
            ESP_LOGW(TAG, "Ring PSRAM pieno (%u%%) — frame perso",
                     (unsigned)core_h264_buffer_fill_percent(ctx->ring));
        }
    }
    xSemaphoreGive(ctx->ring_mtx);
    return ok;
}

static bool rec_capture_one_frame(rec_dual_job_ctx_t *ctx, bool no_wait) {
    recorder_capture_t *capture = ctx->capture;
    if (!no_wait && rec_ring_fill_pct(ctx) >= CORE_RING_PAUSE_PCT) {
        return false;
    }

    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
    };
    esp_capture_err_t err = esp_capture_sink_acquire_frame(capture->sink, &frame, no_wait);
    if (err != ESP_CAPTURE_ERR_OK || !frame.data || frame.size <= 0) {
        return false;
    }

    if (recorder_capture_warmup(capture)) {
        esp_capture_sink_release_frame(capture->sink, &frame);
        return true;
    }

    if (capture->kept_frames == 0) {
        ESP_LOGI(TAG, "Warmup completato — registrazione utile da PTS 0");
    }
    uint32_t pts_ms = recorder_capture_next_pts(capture);

    bool pushed = false;
    while (!ctx->should_stop() && !ctx->user_stop) {
        if (rec_capture_push_ring(ctx, frame.data, (uint32_t)frame.size, pts_ms)) {
            capture->kept_frames++;
            pushed = true;
            break;
        }
        if (no_wait) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(capture->frame_dur_ms / 4));
    }
    if (!pushed && no_wait) {
        ctx->dropped_frames++;
    }

    esp_capture_sink_release_frame(capture->sink, &frame);
    return true;
}

static void rec_capture_job_task(void *arg) {
    rec_dual_job_ctx_t *ctx = (rec_dual_job_ctx_t *)arg;
    ESP_LOGI(TAG, "JOB1 capture→PSRAM (%dfps, sequenza H264 intatta)", ctx->capture->video_fps);
    while (!ctx->should_stop()) {
        if (rec_ring_fill_pct(ctx) >= CORE_RING_PAUSE_PCT) {
            vTaskDelay(pdMS_TO_TICKS(ctx->capture->frame_dur_ms / 2));
            continue;
        }
        if (!rec_capture_one_frame(ctx, false)) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    // Stop D2/web: esci subito — il ring contiene già il video utile (lag SD ~2 s).
    ESP_LOGI(TAG, "JOB1 capture terminato (%lu frame in ring)",
             (unsigned long)ctx->capture->kept_frames);
    ctx->capture_done = true;
    vTaskDelete(nullptr);
}

static bool rec_sd_write_head_frame(rec_dual_job_ctx_t *ctx) {
    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    if (!ctx->ring->head) {
        xSemaphoreGive(ctx->ring_mtx);
        return false;
    }
    core_h264_frame_t *f = ctx->ring->head;
    uint32_t pts = f->pts_ms;
    uint32_t size = f->size;
    const uint8_t *data = core_h264_frame_data(ctx->ring, f);
    xSemaphoreGive(ctx->ring_mtx);

    if (!recorder_muxer_write_packet(ctx->muxer, data, size, pts, nullptr)) {
        return false;
    }

    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    if (ctx->ring->head && ctx->ring->head->pts_ms == pts) {
        core_h264_buffer_pop_head(ctx->ring);
        ctx->muxer_frames++;
    }
    xSemaphoreGive(ctx->ring_mtx);
    return true;
}

static bool rec_sd_may_write_head(rec_dual_job_ctx_t *ctx) {
    if (!ctx->sd_prep_done || !ctx->sd_ctx->paths_ok) {
        return false;
    }
    xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
    if (!ctx->ring->head) {
        xSemaphoreGive(ctx->ring_mtx);
        return false;
    }
    uint32_t head_pts = ctx->ring->head->pts_ms;
    uint32_t tail_pts = ctx->producer_pts_max;
    uint32_t ring_pct = core_h264_buffer_fill_percent(ctx->ring);
    xSemaphoreGive(ctx->ring_mtx);

    uint32_t lag_ms = CORE_REC_SD_LAG_MS;
    if (ctx->should_stop() || ctx->user_stop) {
        lag_ms = 0;
    } else if (ring_pct >= CORE_REC_SD_LAG_URGENT_PCT) {
        lag_ms = 0;
    } else if (ring_pct >= CORE_REC_SD_LAG_CATCHUP_PCT) {
        lag_ms = CORE_REC_SD_LAG_MS / 2;
    }

    if (!ctx->capture_done &&
        (tail_pts < head_pts || (tail_pts - head_pts) < lag_ms)) {
        return false;
    }

    if (!ctx->muxer->active) {
        xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
        bool has_idr = core_h264_buffer_has_idr(ctx->ring);
        xSemaphoreGive(ctx->ring_mtx);
        if (!has_idr) {
            return false;
        }
        if (!recorder_ensure_muxer_open(ctx->muxer, ctx->sd_ctx->final_path, ctx->ring,
                                        ctx->capture)) {
            return false;
        }
        if (ctx->muxer_was_active) {
            *ctx->muxer_was_active = true;
        }
        // Allinea al path stream→muxer: svuota subito il backlog già pronto nel ring.
        for (uint32_t i = 0; i < CORE_REC_SD_BURST_FRAMES; i++) {
            if (!rec_sd_write_head_frame(ctx)) {
                break;
            }
        }
    }
    return ctx->muxer->active;
}

static void rec_sd_job_task(void *arg) {
    rec_dual_job_ctx_t *ctx = (rec_dual_job_ctx_t *)arg;
    ESP_LOGI(TAG, "JOB2 PSRAM→SD (lag %u ms, burst %u)", (unsigned)CORE_REC_SD_LAG_MS,
             (unsigned)CORE_REC_SD_BURST_FRAMES);

    while (!ctx->sd_prep_done && !ctx->capture_done) {
        if (ctx->sd_ctx->ev && (xEventGroupGetBits(ctx->sd_ctx->ev) & SD_PREP_DONE_BIT)) {
            ctx->sd_prep_done = true;
            ESP_LOGI(TAG, "JOB2 SD: percorso registrazione pronto");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!ctx->sd_prep_done && ctx->sd_ctx->ev) {
        ctx->sd_prep_done = (xEventGroupGetBits(ctx->sd_ctx->ev) & SD_PREP_DONE_BIT) != 0;
    }

    bool muxer_logged = false;
    for (;;) {
        xSemaphoreTake(ctx->ring_mtx, portMAX_DELAY);
        bool empty = (ctx->ring->head == nullptr);
        bool done = ctx->capture_done;
        xSemaphoreGive(ctx->ring_mtx);
        if (done && empty) {
            break;
        }

        if (!rec_sd_may_write_head(ctx)) {
            vTaskDelay(pdMS_TO_TICKS((ctx->should_stop() || ctx->user_stop) ? 0 : 2));
            continue;
        }

        if (!muxer_logged) {
            muxer_logged = true;
            ESP_LOGI(TAG, "JOB2 SD: scrittura MP4 %s", ctx->sd_ctx->final_path);
        }

        uint32_t burst_limit = (ctx->should_stop() || ctx->user_stop)
            ? CORE_REC_SD_BURST_STOP : CORE_REC_SD_BURST_FRAMES;
        uint32_t burst = 0;
        for (uint32_t i = 0; i < burst_limit; i++) {
            if (!rec_sd_may_write_head(ctx)) {
                break;
            }
            if (!rec_sd_write_head_frame(ctx)) {
                break;
            }
            burst++;
        }
        if (burst == 0) {
            vTaskDelay(pdMS_TO_TICKS((ctx->should_stop() || ctx->user_stop) ? 0 : 2));
        }
    }

    ESP_LOGI(TAG, "JOB2 SD terminato (%lu frame scritti)", (unsigned long)ctx->muxer_frames);
    ctx->sd_done = true;
    vTaskDelete(nullptr);
}

static bool recorder_process_frame_psram(recorder_capture_t *capture, core_h264_buffer_t *psram_buf,
                                         uint32_t *dropped_frames, bool no_wait) {
    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
    };
    esp_capture_err_t err = esp_capture_sink_acquire_frame(capture->sink, &frame, no_wait);
    if (err != ESP_CAPTURE_ERR_OK || !frame.data || frame.size <= 0) {
        return false;
    }

    uint32_t pts_ms = 0;
    if (!recorder_frame_accept(capture, &pts_ms)) {
        esp_capture_sink_release_frame(capture->sink, &frame);
        return true;
    }

    if (!core_h264_buffer_push(psram_buf, pts_ms, frame.data, (uint32_t)frame.size)) {
        if (dropped_frames) {
            (*dropped_frames)++;
        }
    }

    esp_capture_sink_release_frame(capture->sink, &frame);
    return true;
}

static bool recorder_ensure_muxer_open(recorder_muxer_t *muxer, const char *path,
                                       core_h264_buffer_t *psram_buf,
                                       const recorder_capture_t *capture) {
    if (muxer->active) {
        return true;
    }
    if (!capture || !core_h264_buffer_has_idr(psram_buf)) {
        return false;
    }
    return recorder_muxer_start_from_buffer(muxer, path, psram_buf, nullptr,
                                            capture->video_width, capture->video_height,
                                            capture->video_fps);
}

static bool recorder_flush_psram_to_sd(recorder_muxer_t *muxer, recorder_capture_t *capture,
                                       const sd_prep_ctx_t *sd_ctx, core_h264_buffer_t *psram_buf,
                                       bool capture_ok, bool sd_prep_done, bool *muxer_was_active,
                                       uint32_t *dropped_frames, uint32_t *flush_cycles,
                                       recorder_should_stop_fn_t should_stop, bool *capture_running) {
    if (!muxer || !sd_ctx || !psram_buf || !sd_prep_done || !sd_ctx->paths_ok ||
        psram_buf->frame_count == 0) {
        return false;
    }
    if (!core_h264_buffer_should_flush(psram_buf)) {
        return false;
    }

    if (!recorder_ensure_muxer_open(muxer, sd_ctx->final_path, psram_buf, capture)) {
        ESP_LOGW(TAG, "Flush SD: muxer non pronto (IDR/SPS mancanti)");
        return false;
    }
    if (!muxer->active) {
        return false;
    }
    if (muxer_was_active) {
        *muxer_was_active = true;
    }

    if (*flush_cycles == 0) {
        recorder_discard_psram_until_idr(psram_buf);
    }

    int64_t t0 = esp_timer_get_time();
    uint32_t drained = 0;
    uint32_t stall = 0;

    ESP_LOGI(TAG, "Flush SD ciclo %lu: pool %u%% (%lu KB, %lu frame) → svuotamento...",
             (unsigned long)(*flush_cycles + 1),
             (unsigned)core_h264_buffer_fill_percent(psram_buf),
             (unsigned long)(core_h264_buffer_bytes_used(psram_buf) / 1024),
             (unsigned long)psram_buf->frame_count);

    while (psram_buf->frame_count > 0) {
        if (should_stop && should_stop()) {
            ESP_LOGI(TAG, "Flush SD interrotto da stop (%lu frame già scritti, %lu in pool)",
                     (unsigned long)drained, (unsigned long)psram_buf->frame_count);
            if (capture && capture->running) {
                recorder_capture_stop(capture);
                if (capture_running) {
                    *capture_running = false;
                }
            }
            break;
        }
        uint32_t n = recorder_drain_psram_to_muxer(muxer, psram_buf, should_stop);
        drained += n;
        if (n == 0) {
            if (++stall >= 400) {
                ESP_LOGE(TAG, "Flush SD bloccato dopo %lu frame — memoria muxer/SD esaurita",
                         (unsigned long)drained);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            stall = 0;
        }
    }

    if (psram_buf->frame_count > 0) {
        ESP_LOGW(TAG, "Flush incompleto: %lu frame rimasti in pool", (unsigned long)psram_buf->frame_count);
    }

    if (capture) {
        capture->pts_epoch_ms += CORE_REC_FLUSH_PTS_GAP_MS;
    }
    (*flush_cycles)++;

    ESP_LOGI(TAG, "Flush SD ciclo %lu completato: %lu frame in %lld ms (salto PTS +%u ms, pool 0%%)",
             (unsigned long)*flush_cycles, (unsigned long)drained,
             (long long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)CORE_REC_FLUSH_PTS_GAP_MS);
    return true;
}

static bool recorder_final_psram_flush(recorder_muxer_t *muxer, recorder_capture_t *capture,
                                       const sd_prep_ctx_t *sd_ctx, core_h264_buffer_t *psram_buf,
                                       bool capture_ok, bool sd_prep_done, bool *muxer_was_active,
                                       uint32_t *dropped_frames, bool stop_requested,
                                       recorder_should_stop_fn_t should_stop) {
    if (!psram_buf || psram_buf->frame_count == 0 || !sd_prep_done || !sd_ctx->paths_ok) {
        return false;
    }
    if (!recorder_ensure_muxer_open(muxer, sd_ctx->final_path, psram_buf, capture)) {
        return false;
    }
    if (muxer_was_active && muxer->active) {
        *muxer_was_active = true;
    }

    ESP_LOGI(TAG, "Flush finale PSRAM: %lu frame (~%u s)",
             (unsigned long)psram_buf->frame_count,
             (unsigned)core_h264_buffer_estimated_sec(psram_buf));

    uint32_t flush_limit_ms = stop_requested ? REC_FINAL_FLUSH_STOP_MS
                                             : (REC_DRAIN_TIMEOUT_MS * 3000U);
    int64_t until = esp_timer_get_time() + ((int64_t)flush_limit_ms * 1000);
    while (psram_buf->frame_count > 0 && esp_timer_get_time() < until) {
        if (stop_requested) {
            break;
        }
        if (recorder_drain_psram_to_muxer(muxer, psram_buf, nullptr) == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (stop_requested && psram_buf->frame_count > 0) {
        ESP_LOGW(TAG, "Flush finale abbreviato da stop: %lu frame non scritti",
                 (unsigned long)psram_buf->frame_count);
    }
    return psram_buf->frame_count == 0;
}

static bool recorder_process_frame(recorder_capture_t *capture, recorder_muxer_t *muxer,
                                   core_h264_buffer_t *psram_buf, uint32_t *dropped_frames,
                                   bool no_wait) {
    (void)muxer;
    return recorder_process_frame_psram(capture, psram_buf, dropped_frames, no_wait);
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
        uint32_t pts_ms = 0;
        if (!recorder_frame_accept(capture, &pts_ms)) {
            esp_capture_sink_release_frame(capture->sink, &frame);
            continue;
        }
        if (!core_h264_buffer_push(psram_buf, pts_ms, frame.data, (uint32_t)frame.size)) {
            ESP_LOGW(TAG, "Pool pieno durante drain capture");
        }
        esp_capture_sink_release_frame(capture->sink, &frame);
    }
}

static void recorder_drain_pending_capture(recorder_capture_t *capture, core_h264_buffer_t *psram_buf,
                                           uint32_t *dropped_frames) {
    if (!capture || !capture->running || !psram_buf) {
        return;
    }
    uint32_t recovered = 0;
    for (;;) {
        if (!recorder_process_frame_psram(capture, psram_buf, dropped_frames, true)) {
            break;
        }
        recovered++;
    }
    if (recovered > 0) {
        ESP_LOGI(TAG, "Recuperati %lu frame in coda capture", (unsigned long)recovered);
    }
}

static void sd_prep_task(void *arg) {
    sd_prep_ctx_t *ctx = (sd_prep_ctx_t *)arg;
    int64_t t0 = esp_timer_get_time();

    core_time_init();

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
                    core_status_led_notify_sd_ready();
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
                                   bool capture_ok, bool muxer_was_active, uint32_t muxer_frames,
                                   uint32_t dropped_frames, bool async_encrypt,
                                   char *out_path, size_t out_path_len) {
    bool saved = false;

    if (!sd_ctx->paths_ok) {
        ESP_LOGW(TAG, "Nessun file salvato (capture=%d sd=%d paths=%d mux=%d frames=%lu drop=%lu)",
                 capture_ok, sd_ctx->sd_mounted, sd_ctx->paths_ok, muxer_was_active,
                 (unsigned long)muxer_frames, (unsigned long)dropped_frames);
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
        ESP_LOGW(TAG, "File registrazione assente: %s (mux=%d frames=%lu)",
                 sd_ctx->final_path, muxer_was_active, (unsigned long)muxer_frames);
    } else if (st.st_size < (off_t)(CORE_CRYPTO_FILE_HEADER + REC_MIN_MP4_BYTES)) {
        ESP_LOGW(TAG, "File registrazione troppo piccolo/corrotto: %s (%ld byte, mux_frames=%lu) — elimino",
                 sd_ctx->final_path, (long)st.st_size, (unsigned long)muxer_frames);
        remove(sd_ctx->final_path);
    } else {
        saved = true;
        ESP_LOGI(TAG, "File salvato cifrato (dual-job PSRAM→SD): %s (%ld bytes, %lu frame mux)",
                 sd_ctx->final_path, (long)st.st_size, (unsigned long)muxer_frames);
        if (out_path && out_path_len > 0) {
            snprintf(out_path, out_path_len, "%s", sd_ctx->final_path);
        }
    }

    if (!saved) {
        ESP_LOGW(TAG, "Nessun file salvato (capture=%d sd=%d paths=%d mux=%d frames=%lu drop=%lu)",
                 capture_ok, sd_ctx->sd_mounted, sd_ctx->paths_ok, muxer_was_active,
                 (unsigned long)muxer_frames, (unsigned long)dropped_frames);
    }
    return saved;
}

static bool recorder_run_core(const core_settings_t *settings, recorder_should_stop_fn_t should_stop,
                              bool async_encrypt, const char *stop_label,
                              char *out_path, size_t out_path_len, uint32_t warmup_frames) {
    if (!settings || !should_stop) {
        return false;
    }

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

    if (!core_encfs_register()) {
        ESP_LOGE(TAG, "VFS /enc non disponibile");
        vEventGroupDelete(sd_ctx.ev);
        return false;
    }

    recorder_capture_t capture = {};
    core_h264_buffer_t ring_buf = {};
    SemaphoreHandle_t ring_mtx = xSemaphoreCreateMutex();

    bool sd_prep_done = false;

    // PIR: camera subito, SD monta in parallelo dopo (non blocca capture).
    bool capture_ok = recorder_capture_start_psram(&capture, warmup_frames, settings);
    if (capture_ok) {
        core_status_led_set_mode(CORE_LED_RECORDING);
        ESP_LOGI(TAG, "Capture avviata in %lld ms dal boot (warmup=%lu frame, SD in parallelo)",
                 (long long)core_boot_elapsed_ms(), (unsigned long)warmup_frames);
    }

    xTaskCreate(sd_prep_task, "sd_prep", CORE_SD_PREP_TASK_STACK, &sd_ctx, CORE_SD_PREP_TASK_PRIO, nullptr);

    // L'encoder HW alloca i buffer di lavoro al primo frame: deve farlo ORA,
    // prima che il ring si prenda il 95% della PSRAM (altrimenti OOM e 0 frame).
    if (capture_ok && !recorder_capture_prime(&capture, CORE_REC_PRIME_TIMEOUT_MS)) {
        recorder_capture_stop(&capture);
        capture_ok = false;
    }

    if (!capture_ok || !ring_mtx) {
        ESP_LOGE(TAG, "Capture/mutex non avviati — impossibile registrare");
        core_status_led_end_recording(core_gpio_is_mode_config());
        xEventGroupWaitBits(sd_ctx.ev, SD_PREP_DONE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
        if (ring_mtx) {
            vSemaphoreDelete(ring_mtx);
        }
        core_gpio_rec_stop_session_end();
        vEventGroupDelete(sd_ctx.ev);
        return false;
    }

    if (!core_h264_buffer_init(&ring_buf)) {
        ESP_LOGE(TAG, "Ring PSRAM non disponibile");
        recorder_capture_stop(&capture);
        core_status_led_end_recording(core_gpio_is_mode_config());
        vSemaphoreDelete(ring_mtx);
        core_gpio_rec_stop_session_end();
        vEventGroupDelete(sd_ctx.ev);
        return false;
    }

    ESP_LOGI(TAG, "Registrazione dual-job: %dx%d@%dfps → ring → SD (lag %u ms, catch-up automatico)",
             capture.video_width, capture.video_height, capture.video_fps,
             (unsigned)CORE_REC_SD_LAG_MS);

    recorder_muxer_t muxer = {};
    bool muxer_was_active = false;

    rec_dual_job_ctx_t job = {
        .capture = &capture,
        .muxer = &muxer,
        .sd_ctx = &sd_ctx,
        .ring = &ring_buf,
        .ring_mtx = ring_mtx,
        .should_stop = should_stop,
        .muxer_was_active = &muxer_was_active,
    };

    TaskHandle_t cap_task = nullptr;
    TaskHandle_t sd_task = nullptr;
    if (xTaskCreate(rec_capture_job_task, "rec_cap", CORE_REC_CAPTURE_TASK_STACK, &job,
                    CORE_REC_CAPTURE_TASK_PRIO, &cap_task) != pdPASS ||
        xTaskCreate(rec_sd_job_task, "rec_sd", CORE_REC_SD_TASK_STACK, &job,
                    CORE_REC_SD_TASK_PRIO, &sd_task) != pdPASS) {
        ESP_LOGE(TAG, "Avvio task dual-job fallito");
        recorder_capture_stop(&capture);
        core_h264_buffer_deinit(&ring_buf);
        vSemaphoreDelete(ring_mtx);
        core_gpio_rec_stop_session_end();
        vEventGroupDelete(sd_ctx.ev);
        return false;
    }

    int64_t last_status_log = esp_timer_get_time();
    int64_t rec_start_us = esp_timer_get_time();

    while (!should_stop()) {
        core_gpio_log_pin_edges();

        if (!sd_prep_done && (xEventGroupGetBits(sd_ctx.ev) & SD_PREP_DONE_BIT)) {
            sd_prep_done = true;
            job.sd_prep_done = true;
        }

        int64_t now = esp_timer_get_time();
        if (now - last_status_log >= ((int64_t)REC_STATUS_LOG_MS * 1000)) {
            last_status_log = now;
            s_manual_frame_count = capture.kept_frames;
            uint32_t rec_ms = (uint32_t)((now - rec_start_us) / 1000);
            uint32_t ring_pct = 0;
            xSemaphoreTake(ring_mtx, portMAX_DELAY);
            ring_pct = core_h264_buffer_fill_percent(&ring_buf);
            xSemaphoreGive(ring_mtx);
            ESP_LOGI(TAG, "Stato rec: kept=%lu mux=%lu ring=%u%% fps~%.1f ~%u s drop=%lu",
                     (unsigned long)capture.kept_frames,
                     (unsigned long)job.muxer_frames,
                     (unsigned)ring_pct,
                     rec_ms > 0 ? (capture.kept_frames * 1000.0f) / (float)rec_ms : 0.0f,
                     (unsigned)(capture.kept_frames * capture.frame_dur_ms / 1000U),
                     (unsigned long)job.dropped_frames);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Stop registrazione (%s) — fermo capture e flush SD", stop_label ? stop_label : "?");

    const bool stopped_by_user = should_stop();
    int64_t stop_at_us = stopped_by_user ? esp_timer_get_time() : 0;

    core_status_led_notify_rec_stop();
    job.user_stop = true;

    const int cap_wait_iters = CORE_REC_STOP_CAPTURE_WAIT_MS / 10;
    for (int i = 0; i < cap_wait_iters && !job.capture_done; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!job.capture_done) {
        ESP_LOGW(TAG, "JOB1 capture: timeout uscita — stop pipeline forzato");
    }

    recorder_capture_stop(&capture);
    if (stopped_by_user) {
        ESP_LOGI(TAG, "Pipeline capture fermata (%lld ms da stop)",
                 (long long)((esp_timer_get_time() - stop_at_us) / 1000));
    }

    const int sd_wait_iters = CORE_REC_STOP_SD_WAIT_MS / 10;
    int64_t last_flush_log = esp_timer_get_time();
    for (int i = 0; i < sd_wait_iters && !job.sd_done; ++i) {
        int64_t now = esp_timer_get_time();
        if (stopped_by_user && (now - last_flush_log) >= 1000000) {
            last_flush_log = now;
            uint32_t ring_pct = 0;
            xSemaphoreTake(ring_mtx, portMAX_DELAY);
            ring_pct = core_h264_buffer_fill_percent(&ring_buf);
            xSemaphoreGive(ring_mtx);
            ESP_LOGI(TAG, "Flush SD: ring=%u%% mux=%lu (%lld ms da stop)",
                     (unsigned)ring_pct, (unsigned long)job.muxer_frames,
                     (long long)((now - stop_at_us) / 1000));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!job.sd_done) {
        ESP_LOGW(TAG, "JOB2 SD: timeout drain — chiusura forzata");
    }

    if (!job.sd_prep_done) {
        xEventGroupWaitBits(sd_ctx.ev, SD_PREP_DONE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
        job.sd_prep_done = true;
    }

    recorder_muxer_close(&muxer);
    if (stopped_by_user) {
        ESP_LOGI(TAG, "Muxer chiuso (%lld ms da stop)",
                 (long long)((esp_timer_get_time() - stop_at_us) / 1000));
    }
    if (job.muxer_frames == 0 && muxer_was_active) {
        ESP_LOGW(TAG, "Muxer attivo ma 0 frame scritti su SD — file probabilmente assente");
    }
    core_h264_buffer_deinit(&ring_buf);
    vSemaphoreDelete(ring_mtx);
    vEventGroupDelete(sd_ctx.ev);

    core_gpio_rec_stop_session_end();

    uint32_t timeline_sec = capture.kept_frames * capture.frame_dur_ms / 1000U;
    ESP_LOGI(TAG, "Registrazione chiusa: %lu catturati (~%u s), %lu su SD, drop=%lu",
             (unsigned long)capture.kept_frames, (unsigned)timeline_sec,
             (unsigned long)job.muxer_frames, (unsigned long)job.dropped_frames);

    if (stopped_by_user) {
        ESP_LOGI(TAG, "Teardown stop utente in %lld ms",
                 (long long)((esp_timer_get_time() - stop_at_us) / 1000));
    }

    bool saved = recorder_finalize_save(settings, &sd_ctx, true, muxer_was_active, job.muxer_frames,
                                        job.dropped_frames, async_encrypt, out_path, out_path_len);
    core_status_led_end_recording(core_gpio_is_mode_config());
    return saved;
}

static void manual_rec_task(void *arg) {
    manual_rec_arg_t *ctx = (manual_rec_arg_t *)arg;
    char saved_path[128] = {0};
    bool saved = recorder_run_core(&ctx->settings, manual_should_stop, true, "web stop",
                                   saved_path, sizeof(saved_path), CORE_REC_WARMUP_DROP_FRAMES);
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

    bool saved = recorder_run_core(settings, rec_stop_should_stop, false, "stop GPIO",
                                     nullptr, 0, CORE_REC_WARMUP_DROP_FRAMES_PIR);
    core_gpio_hold_tpl_done(settings->d2_post_delay_ms);
    return saved;
}

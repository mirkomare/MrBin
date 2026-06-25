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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "core_recorder";

#define REC_MIN_MP4_BYTES       1024
#define SD_PREP_DONE_BIT        BIT0
#define H264_CODEC_SPEC_MAX     512
#define REC_STATUS_LOG_MS       5000
#define REC_D2_RELEASE_TIMEOUT_MS 30000
#define REC_DRAIN_TIMEOUT_MS    1000
#define MANUAL_REC_DONE_BIT     BIT0
#define MANUAL_REC_TASK_STACK   12288
#define MANUAL_REC_TASK_PRIO    5
#define ENCRYPT_BG_TASK_STACK   16384
#define ENCRYPT_BG_TASK_PRIO    4

typedef struct {
    char     tmp_path[140];
    char     final_path[128];
    uint8_t  key[16];
} encrypt_job_t;

static volatile bool s_encrypt_bg_busy = false;

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

static bool encrypt_mp4_file(const char *plain_path, const char *enc_path, const uint8_t key[16]);

static void encrypt_bg_task(void *arg) {
    encrypt_job_t *job = (encrypt_job_t *)arg;
    int64_t t0 = esp_timer_get_time();
    bool ok = encrypt_mp4_file(job->tmp_path, job->final_path, job->key);
    ESP_LOGI(TAG, "Cifratura background %s in %lld ms: %s",
             ok ? "OK" : "FALLITA",
             (long long)((esp_timer_get_time() - t0) / 1000),
             job->final_path);
    s_encrypt_bg_busy = false;
    free(job);
    vTaskDelete(nullptr);
}

typedef bool (*recorder_should_stop_fn_t)(void);

typedef struct {
    char path[140];
} recorder_muxer_ctx_t;

typedef struct {
    esp_capture_handle_t capture;
    esp_capture_sink_handle_t sink;
    esp_capture_video_src_if_t *vsrc;
    bool running;
} recorder_capture_t;

typedef struct {
    EventGroupHandle_t ev;
    uint32_t           core_id;
    char               tmp_path[140];
    char               final_path[128];
    bool               paths_ok;
    bool               sd_mounted;
} sd_prep_ctx_t;

typedef struct {
    esp_muxer_handle_t handle;
    int                stream_index;
    bool               active;
} recorder_muxer_t;

#define CORE_CRYPTO_FILE_HEADER   20   // MRBI (4) + IV (16)

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

    FILE *fout = fopen(enc_path, "wb");
    if (!fout) {
        ESP_LOGE(TAG, "Creazione %s fallita (errno=%d)", enc_path, errno);
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
        remove(enc_path);
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
        remove(enc_path);
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
        remove(enc_path);
        return false;
    }

    if (!core_crypto_init_ctx(&ctx, key, iv)) {
        heap_caps_free(buf);
        file_io_buf_free(&fin_vbuf);
        file_io_buf_free(&fout_vbuf);
        fclose(fin);
        fclose(fout);
        remove(enc_path);
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
        remove(enc_path);
        return false;
    }

    remove(plain_path);
    ESP_LOGI(TAG, "Cifratura MP4 %ld bytes in %lld ms (buf=%u)",
             (long)st.st_size, (long long)((esp_timer_get_time() - t0) / 1000), (unsigned)io_buf_size);
    return true;
}

static int storage_path_handler(esp_muxer_slice_info_t *info, void *ctx) {
    recorder_muxer_ctx_t *mux_ctx = (recorder_muxer_ctx_t *)ctx;
    snprintf(info->file_path, info->len, "%s", mux_ctx->path);
    return 0;
}

static bool recording_file_is_valid(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < (off_t)REC_MIN_MP4_BYTES) {
        return false;
    }
    ESP_LOGI(TAG, "MP4 OK: %s (%ld bytes)", path, (long)st.st_size);
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

static bool recorder_muxer_feed_frame(recorder_muxer_t *muxer, const esp_capture_stream_frame_t *frame) {
    esp_muxer_video_packet_t pkt = {
        .data = frame->data,
        .len = frame->size,
        .pts = frame->pts,
        .dts = frame->pts,
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

    bool ok = true;
    if (!muxer->active) {
        if (!core_h264_buffer_push(psram_buf, frame.pts, frame.data, (uint32_t)frame.size)) {
            if (dropped_frames) {
                (*dropped_frames)++;
            }
            ok = false;
        }
    } else if (!recorder_muxer_feed_frame(muxer, &frame)) {
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
        if (!muxer->active) {
            core_h264_buffer_push(psram_buf, frame.pts, frame.data, (uint32_t)frame.size);
        } else if (!recorder_muxer_feed_frame(muxer, &frame)) {
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
    if (recorder_muxer_start_from_buffer(muxer, sd_ctx->tmp_path, psram_buf)) {
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
                    ESP_LOGI(TAG, "Path registrazione: %s", ctx->final_path);
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

static bool d2_should_stop(void) {
    return core_gpio_is_d2_end();
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

    bool tmp_valid = recording_file_is_valid(sd_ctx->tmp_path);
    if (!tmp_valid) {
        struct stat tmp_st;
        if (stat(sd_ctx->tmp_path, &tmp_st) == 0) {
            ESP_LOGW(TAG, "MP4 tmp non valido: %s (%ld byte)", sd_ctx->tmp_path, (long)tmp_st.st_size);
            remove(sd_ctx->tmp_path);
        } else {
            ESP_LOGW(TAG, "MP4 tmp assente: %s (mux=%d)", sd_ctx->tmp_path, muxer_was_active);
        }
        remove(sd_ctx->final_path);
    } else if (!settings->aes_key_valid) {
        ESP_LOGE(TAG, "Chiave AES assente — impossibile cifrare %s", sd_ctx->tmp_path);
        remove(sd_ctx->tmp_path);
    } else if (async_encrypt) {
        encrypt_job_t *job = (encrypt_job_t *)calloc(1, sizeof(*job));
        if (!job) {
            ESP_LOGE(TAG, "Job cifratura: memoria esaurita");
            saved = encrypt_mp4_file(sd_ctx->tmp_path, sd_ctx->final_path, settings->aes_key);
        } else {
            snprintf(job->tmp_path, sizeof(job->tmp_path), "%s", sd_ctx->tmp_path);
            snprintf(job->final_path, sizeof(job->final_path), "%s", sd_ctx->final_path);
            memcpy(job->key, settings->aes_key, sizeof(job->key));
            if (s_encrypt_bg_busy ||
                xTaskCreate(encrypt_bg_task, "enc_mp4", ENCRYPT_BG_TASK_STACK, job,
                            ENCRYPT_BG_TASK_PRIO, nullptr) != pdPASS) {
                ESP_LOGW(TAG, "Cifratura background non avviata — sync");
                free(job);
                saved = encrypt_mp4_file(sd_ctx->tmp_path, sd_ctx->final_path, settings->aes_key);
            } else {
                s_encrypt_bg_busy = true;
                saved = true;
                ESP_LOGI(TAG, "Mux OK — cifratura in background: %s", sd_ctx->final_path);
            }
        }
        if (!saved) {
            ESP_LOGE(TAG, "Cifratura fallita: %s -> %s", sd_ctx->tmp_path, sd_ctx->final_path);
            remove(sd_ctx->tmp_path);
            remove(sd_ctx->final_path);
        }
    } else {
        saved = encrypt_mp4_file(sd_ctx->tmp_path, sd_ctx->final_path, settings->aes_key);
        if (!saved) {
            ESP_LOGE(TAG, "Cifratura fallita: %s -> %s", sd_ctx->tmp_path, sd_ctx->final_path);
            remove(sd_ctx->tmp_path);
            remove(sd_ctx->final_path);
        }
    }

    if (saved) {
        if (!async_encrypt) {
            struct stat st;
            if (stat(sd_ctx->final_path, &st) == 0) {
                ESP_LOGI(TAG, "File salvato: %s (%ld bytes)", sd_ctx->final_path, (long)st.st_size);
            }
        }
        if (out_path && out_path_len > 0) {
            snprintf(out_path, out_path_len, "%s", sd_ctx->final_path);
        }
    } else {
        ESP_LOGW(TAG, "Nessun file salvato (capture=%d sd=%d paths=%d mux=%d enc=%d drop=%lu)",
                 capture_ok, sd_ctx->sd_mounted, sd_ctx->paths_ok, muxer_was_active, saved,
                 (unsigned long)dropped_frames);
    }
    return saved;
}

static bool recorder_run_core(const core_settings_t *settings, recorder_should_stop_fn_t should_stop,
                              bool wait_d2_release, bool async_encrypt, const char *stop_label,
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

    xTaskCreate(sd_prep_task, "sd_prep", CORE_SD_PREP_TASK_STACK, &sd_ctx, CORE_SD_PREP_TASK_PRIO, nullptr);

    recorder_capture_t capture = {};
    core_h264_buffer_t psram_buf;
    core_h264_buffer_init(&psram_buf, CORE_H264_PSRAM_MAX_BYTES);

    bool capture_ok = recorder_capture_start_psram(&capture);
    if (!capture_ok) {
        ESP_LOGE(TAG, "Capture non avviato");
    } else {
        core_status_led_set_mode(CORE_LED_RECORDING);
        ESP_LOGI(TAG, "Registrazione PSRAM attiva in %lld ms",
                 (long long)((esp_timer_get_time() - boot_t0) / 1000));
    }

    recorder_muxer_t muxer = {};
    bool sd_prep_done = false;
    bool muxer_was_active = false;
    uint32_t dropped_frames = 0;
    int64_t last_status_log = esp_timer_get_time();

    if (wait_d2_release && core_gpio_is_d2_end()) {
        ESP_LOGW(TAG, "D2 già attivo (GPIO%d LOW) — attendo rilascio prima di registrare", CORE_GPIO_D2_END);
        int64_t wait_until = esp_timer_get_time() + ((int64_t)REC_D2_RELEASE_TIMEOUT_MS * 1000);
        while (core_gpio_is_d2_end() && esp_timer_get_time() < wait_until) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (core_gpio_is_d2_end()) {
            ESP_LOGE(TAG, "D2 resta attivo — sessione senza frame");
        } else {
            ESP_LOGI(TAG, "D2 rilasciato — avvio loop registrazione");
        }
    }

    ESP_LOGI(TAG, "Registrazione attiva — stop: %s", stop_label ? stop_label : "?");
    while (!should_stop()) {
        if (capture_ok) {
            recorder_process_frame(&capture, &muxer, &psram_buf, &dropped_frames);
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
    bool saved = recorder_run_core(&ctx->settings, manual_should_stop, false, true, "web stop",
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
    bool saved = recorder_run_core(settings, d2_should_stop, true, false, "D2 GPIO",
                                     nullptr, 0);
    core_gpio_hold_tpl_done(settings->d2_post_delay_ms);
    return saved;
}

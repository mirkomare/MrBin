#include "CoreRecorder.h"
#include "CoreConfig.h"
#include "CoreCrypto.h"
#include "CoreGPIO.h"
#include "CoreSD.h"
#include "CoreVideo.h"

#include "encoder/esp_video_enc_default.h"
#include "esp_capture.h"
#include "esp_capture_sink.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "impl/esp_capture_video_v4l2_src.h"
#include "impl/mp4_muxer.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "core_recorder";

#define REC_MIN_MP4_BYTES 1024

typedef struct {
    char path[140];
} recorder_muxer_ctx_t;

typedef struct {
    esp_capture_handle_t capture;
    esp_capture_sink_handle_t sink;
    esp_capture_video_src_if_t *vsrc;
    bool running;
} recorder_capture_t;

static bool encrypt_mp4_file(const char *plain_path, const char *enc_path, const uint8_t key[16]) {
    struct stat st;
    if (stat(plain_path, &st) != 0 || st.st_size < (off_t)REC_MIN_MP4_BYTES) {
        ESP_LOGE(TAG, "MP4 temporaneo non valido: %s", plain_path);
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

    uint32_t magic = CORE_CRYPTO_MAGIC;
    if (fwrite(&magic, 1, sizeof(magic), fout) != sizeof(magic)) {
        ESP_LOGE(TAG, "Scrittura header cifrato fallita");
        fclose(fin);
        fclose(fout);
        remove(enc_path);
        return false;
    }

    uint8_t iv[16];
    esp_fill_random(iv, sizeof(iv));
    if (fwrite(iv, 1, sizeof(iv), fout) != sizeof(iv)) {
        ESP_LOGE(TAG, "Scrittura IV fallita");
        fclose(fin);
        fclose(fout);
        remove(enc_path);
        return false;
    }

    core_crypto_ctx_t ctx;
    core_crypto_init_ctx(&ctx, key, iv);

    uint8_t buf[4096];
    uint64_t offset = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        core_crypto_crypt_buffer(&ctx, buf, n, offset);
        if (fwrite(buf, 1, n, fout) != n) {
            ESP_LOGE(TAG, "Scrittura payload cifrato fallita (offset=%llu)", (unsigned long long)offset);
            fclose(fin);
            fclose(fout);
            remove(enc_path);
            return false;
        }
        offset += n;
    }
    if (ferror(fin)) {
        ESP_LOGE(TAG, "Errore lettura MP4 temporaneo");
        fclose(fin);
        fclose(fout);
        remove(enc_path);
        return false;
    }

    fclose(fin);
    fclose(fout);
    remove(plain_path);
    return true;
}

static int storage_path_handler(esp_muxer_slice_info_t *info, void *ctx) {
    recorder_muxer_ctx_t *mux_ctx = (recorder_muxer_ctx_t *)ctx;
    snprintf(info->file_path, info->len, "%s", mux_ctx->path);
    return 0;
}

static bool recording_file_is_valid(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "File registrazione assente: %s (errno=%d)", path, errno);
        return false;
    }
    if (st.st_size < (off_t)REC_MIN_MP4_BYTES) {
        ESP_LOGE(TAG, "File registrazione troppo piccolo: %s (%ld bytes)", path, (long)st.st_size);
        return false;
    }
    ESP_LOGI(TAG, "MP4 temporaneo OK: %s (%ld bytes)", path, (long)st.st_size);
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

static bool recorder_capture_start(recorder_capture_t *rc, const char *tmp_path) {
    memset(rc, 0, sizeof(*rc));

    if (!core_video_init()) {
        ESP_LOGE(TAG, "core_video_init fallita");
        return false;
    }

    esp_video_enc_register_default();
    mp4_muxer_register();

    esp_capture_video_v4l2_src_cfg_t v4l2_cfg = {
        .buf_count = 4,
    };
    strncpy(v4l2_cfg.dev_name, CORE_VIDEO_DEV_NAME, sizeof(v4l2_cfg.dev_name) - 1);

    esp_capture_video_src_if_t *vsrc = esp_capture_new_video_v4l2_src(&v4l2_cfg);
    if (!vsrc) {
        ESP_LOGE(TAG, "Sorgente V4L2 non disponibile");
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
        ESP_LOGE(TAG, "esp_capture_open fallita (%d)", err);
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
        ESP_LOGE(TAG, "esp_capture_sink_setup fallita (%d)", err);
        esp_capture_close(capture);
        free(vsrc);
        esp_video_enc_unregister_default();
        return false;
    }

    recorder_muxer_ctx_t mux_ctx = {};
    snprintf(mux_ctx.path, sizeof(mux_ctx.path), "%s", tmp_path);

    mp4_muxer_config_t mp4_cfg = {
        .base_config = {
            .muxer_type = ESP_MUXER_TYPE_MP4,
            .slice_duration = 3600000,
            .url_pattern = nullptr,
            .url_pattern_ex = storage_path_handler,
            .data_cb = nullptr,
            .ctx = &mux_ctx,
        },
    };

    esp_capture_muxer_cfg_t muxer_cfg = {
        .base_config = &mp4_cfg.base_config,
        .cfg_size = sizeof(mp4_cfg),
        .muxer_mask = ESP_CAPTURE_MUXER_MASK_VIDEO,
    };

    err = esp_capture_sink_add_muxer(sink, &muxer_cfg);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_sink_add_muxer fallita (%d)", err);
        esp_capture_close(capture);
        free(vsrc);
        esp_video_enc_unregister_default();
        return false;
    }

    esp_capture_sink_enable_muxer(sink, true);
    esp_capture_sink_enable(sink, ESP_CAPTURE_RUN_MODE_ALWAYS);

    err = esp_capture_start(capture);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_start fallita (%d)", err);
        esp_capture_close(capture);
        free(vsrc);
        esp_video_enc_unregister_default();
        return false;
    }

    rc->capture = capture;
    rc->sink = sink;
    rc->vsrc = vsrc;
    rc->running = true;

    ESP_LOGI(TAG, "Registrazione H264 %dx%d@%d -> %s",
             CORE_VIDEO_WIDTH, CORE_VIDEO_HEIGHT, CORE_VIDEO_FPS, tmp_path);
    return true;
}

static void recorder_capture_stop(recorder_capture_t *rc) {
    if (!rc || !rc->running) {
        return;
    }

    esp_capture_err_t err = esp_capture_stop(rc->capture);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGW(TAG, "esp_capture_stop: %d", err);
    }
    recorder_capture_cleanup(rc);
}

bool core_recorder_run_session(const core_settings_t *settings) {
    if (!settings) {
        ESP_LOGE(TAG, "Settings null");
        core_gpio_hold_tpl_done(CORE_D2_POST_DELAY_MS);
        return false;
    }

    core_gpio_log_inputs();

    if (!settings->aes_key_valid) {
        ESP_LOGE(TAG, "Chiave AES non valida — file non cifrato");
    }

    if (!core_sd_is_mounted() && !core_sd_init()) {
        ESP_LOGE(TAG, "SD non montata");
    }

    char day_dir[64] = {0};
    char final_path[128] = {0};
    char tmp_path[140] = {0};
    bool paths_ok = false;

    if (core_sd_is_mounted()) {
        if (!core_sd_ensure_space(CORE_SD_MIN_FREE_BYTES)) {
            ESP_LOGE(TAG, "Spazio SD insufficiente (min %llu bytes liberi)",
                     (unsigned long long)CORE_SD_MIN_FREE_BYTES);
        }
        paths_ok = core_sd_make_day_dir(day_dir, sizeof(day_dir)) &&
                   core_sd_make_recording_path(day_dir, settings->core_id, final_path, sizeof(final_path));
        if (paths_ok) {
            snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
        } else {
            ESP_LOGE(TAG, "Creazione path registrazione fallita");
        }
    }

    recorder_capture_t capture = {};
    bool recording = paths_ok && recorder_capture_start(&capture, tmp_path);
    if (!recording) {
        ESP_LOGW(TAG, "Registrazione non avviata — attendo comunque D2");
    }

    ESP_LOGI(TAG, "Attendo fine movimento: D2 su GPIO%d -> LOW", CORE_GPIO_D2_END);
    while (!core_gpio_is_d2_end()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "D2 ricevuto (GPIO%d LOW)", CORE_GPIO_D2_END);
    core_gpio_log_inputs();

    bool file_ok = false;
    if (recording) {
        recorder_capture_stop(&capture);
        file_ok = recording_file_is_valid(tmp_path);
        if (!file_ok) {
            remove(tmp_path);
        }
    }

    bool saved = false;
    if (file_ok && settings->aes_key_valid) {
        saved = encrypt_mp4_file(tmp_path, final_path, settings->aes_key);
        if (!saved) {
            remove(tmp_path);
        }
    } else if (file_ok) {
        remove(tmp_path);
    }

    if (saved) {
        struct stat st;
        if (stat(final_path, &st) == 0) {
            ESP_LOGI(TAG, "File salvato: %s (%ld bytes)", final_path, (long)st.st_size);
        }
    } else {
        ESP_LOGW(TAG, "Nessun file salvato (reg=%d mp4=%d cifrato=%d)",
                 recording, file_ok, saved);
    }

    core_gpio_hold_tpl_done(settings->d2_post_delay_ms);
    return true;
}

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
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "core_recorder";

typedef struct {
    char path[140];
} recorder_muxer_ctx_t;

static bool encrypt_mp4_file(const char *plain_path, const char *enc_path, const uint8_t key[16]) {
    FILE *fin = fopen(plain_path, "rb");
    if (!fin) {
        return false;
    }

    FILE *fout = fopen(enc_path, "wb");
    if (!fout) {
        fclose(fin);
        return false;
    }

    uint32_t magic = CORE_CRYPTO_MAGIC;
    fwrite(&magic, 1, sizeof(magic), fout);

    uint8_t iv[16];
    esp_fill_random(iv, sizeof(iv));
    fwrite(iv, 1, sizeof(iv), fout);

    core_crypto_ctx_t ctx;
    core_crypto_init_ctx(&ctx, key, iv);

    uint8_t buf[4096];
    uint64_t offset = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        core_crypto_crypt_buffer(&ctx, buf, n, offset);
        if (fwrite(buf, 1, n, fout) != n) {
            fclose(fin);
            fclose(fout);
            remove(enc_path);
            return false;
        }
        offset += n;
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

static bool record_h264_mp4(const char *tmp_path) {
    if (!core_video_init()) {
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

    ESP_LOGI(TAG, "Registrazione H264 %dx%d@%d -> %s",
             CORE_VIDEO_WIDTH, CORE_VIDEO_HEIGHT, CORE_VIDEO_FPS, tmp_path);

    while (!core_gpio_is_d2_end()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_capture_stop(capture);
    esp_capture_close(capture);
    free(vsrc);
    esp_video_enc_unregister_default();

    return true;
}

bool core_recorder_run_session(const core_settings_t *settings) {
    if (!settings || !settings->aes_key_valid) {
        return false;
    }

    if (!core_sd_ensure_space(CORE_SD_MIN_FREE_BYTES)) {
        ESP_LOGE(TAG, "Spazio SD insufficiente");
        return false;
    }

    char day_dir[64];
    if (!core_sd_make_day_dir(day_dir, sizeof(day_dir))) {
        ESP_LOGE(TAG, "Creazione directory giorno fallita");
        return false;
    }

    char final_path[128];
    if (!core_sd_make_recording_path(day_dir, settings->core_id, final_path, sizeof(final_path))) {
        return false;
    }

    char tmp_path[140];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    ESP_LOGI(TAG, "Avvio registrazione (D1 attivo, attendo D2 su GPIO%d)", CORE_GPIO_D2_END);

    bool rec_ok = record_h264_mp4(tmp_path);
    if (!rec_ok) {
        ESP_LOGE(TAG, "Registrazione fallita");
        remove(tmp_path);
        return false;
    }

    if (!encrypt_mp4_file(tmp_path, final_path, settings->aes_key)) {
        ESP_LOGE(TAG, "Cifratura file fallita");
        return false;
    }

    struct stat st;
    if (stat(final_path, &st) == 0) {
        ESP_LOGI(TAG, "File salvato: %s (%ld bytes)", final_path, (long)st.st_size);
    }

    ESP_LOGI(TAG, "D2 ricevuto, attendo %lu ms prima del DONE",
             (unsigned long)settings->d2_post_delay_ms);
    vTaskDelay(pdMS_TO_TICKS(settings->d2_post_delay_ms));

    core_gpio_signal_tpl_done();
    ESP_LOGI(TAG, "DONE inviato (GPIO%d HIGH) — spegnimento TPL", CORE_GPIO_TPL_DONE);

    while (true) {
        core_gpio_signal_tpl_done();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}

#include "CoreRecorder.h"
#include "CoreConfig.h"
#include "CoreCrypto.h"
#include "CoreGPIO.h"
#include "CoreSD.h"

#include "esp_capture.h"
#include "esp_capture_defaults.h"
#include "esp_capture_sink.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "core_recorder";

static bool encrypt_mp4_file(const char *plain_path, const char *enc_path, const uint8_t key[16]) {
    FILE *fin = fopen(plain_path, "rb");
    if (!fin) return false;

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

static bool record_h264_mp4(const char *tmp_path) {
    esp_capture_register_default_video_encoder();
    esp_capture_register_default_muxer();

    esp_capture_video_src_if_t *vsrc = esp_capture_new_video_v4l2_src();
    if (!vsrc) {
        ESP_LOGE(TAG, "video source init fallita");
        return false;
    }

    esp_capture_cfg_t cap_cfg = {
        .video_src = vsrc,
        .audio_src = nullptr,
    };
    esp_capture_handle_t capture = nullptr;
    if (esp_capture_open(&cap_cfg, &capture) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_open fallita");
        return false;
    }

    esp_capture_sink_cfg_t sink0 = {
        .video_info = {
            .format_id = ESP_CAPTURE_FMT_ID_H264,
            .width = CORE_VIDEO_WIDTH,
            .height = CORE_VIDEO_HEIGHT,
            .fps = CORE_VIDEO_FPS,
        },
    };
    esp_capture_sink_handle_t sink = nullptr;
    if (esp_capture_sink_setup(capture, 0, &sink0, &sink) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "sink setup fallita");
        esp_capture_close(capture);
        return false;
    }

    esp_capture_muxer_cfg_t mux_cfg = {
        .muxer_type = ESP_CAPTURE_MUXER_MP4,
        .url = tmp_path,
    };
    if (esp_capture_sink_add_muxer(sink, &mux_cfg) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "muxer add fallita");
        esp_capture_close(capture);
        return false;
    }

    esp_capture_sink_enable(sink, ESP_CAPTURE_RUN_MODE_ALWAYS);
    esp_capture_start(capture);

    ESP_LOGI(TAG, "Registrazione H264 %dx%d@%d → %s",
             CORE_VIDEO_WIDTH, CORE_VIDEO_HEIGHT, CORE_VIDEO_FPS, tmp_path);

    while (!core_gpio_is_d2_end()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_capture_stop(capture);
    esp_capture_sink_disable(sink);
    esp_capture_close(capture);
    return true;
}

bool core_recorder_run_session(const core_settings_t *settings) {
    if (!settings || !settings->aes_key_valid) return false;

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

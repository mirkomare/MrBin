#include "CoreConfig.h"
#include "CoreGPIO.h"
#include "CoreRecorder.h"
#include "CoreSD.h"
#include "CoreSettings.h"
#include "CoreWeb.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "mrbin_core";
static core_settings_t g_settings;

static void disable_radio_for_recording(void) {
    // WiFi/BLE non inizializzati in questa modalità — nessuna radio attiva
    ESP_LOGI(TAG, "Radio disabilitata per sessione registrazione");
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "MrBin CORE avvio (ESP32-P4 Waveshare)");

    if (!core_settings_init()) {
        ESP_LOGE(TAG, "NVS init fallita");
        return;
    }
    core_settings_load(&g_settings);
    core_settings_ensure_id(&g_settings);
    core_settings_ensure_aes_key(&g_settings);

    if (!core_gpio_init()) {
        ESP_LOGE(TAG, "GPIO init fallita");
        return;
    }

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    if (core_gpio_is_d1_wake()) {
        ESP_LOGI(TAG, "D1 attivo (GPIO%d LOW) — modalità registrazione PIR", CORE_GPIO_D1_WAKE);
        disable_radio_for_recording();
        if (!core_sd_init()) {
            ESP_LOGW(TAG, "SD non disponibile — registrazione non possibile");
        } else {
            ESP_LOGI(TAG, "SD OK, spazio libero: %llu bytes", (unsigned long long)core_sd_free_bytes());
        }
        core_recorder_run_session(&g_settings);
        // non ritorna: loop DONE
    }

    ESP_LOGI(TAG, "Boot senza D1 — modalità configurazione (Web GUI :%d)", CORE_WEB_PORT);
    core_web_start(&g_settings);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

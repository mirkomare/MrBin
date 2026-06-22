#include "CoreConfig.h"
#include "CoreGPIO.h"
#include "CoreRecorder.h"
#include "CoreSettings.h"
#include "CoreStatusLed.h"
#include "CoreWeb.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "mrbin_core";
static core_settings_t g_settings;

static void disable_radio_for_recording(void) {
    ESP_LOGI(TAG, "Radio disabilitata per sessione registrazione");
}

static void run_config_mode(void) {
    ESP_LOGI(TAG, "GPIO%d HIGH — modalità configurazione (WiFi/Web)", CORE_GPIO_MODE_CFG);

    bool web_ok = false;
    if (g_settings.wifi_ssid[0] != 0) {
        web_ok = core_web_start(&g_settings, CORE_WEB_WIFI_STA, nullptr);
    } else {
        web_ok = core_web_start(&g_settings, CORE_WEB_WIFI_AP, nullptr);
    }

    if (web_ok) {
        ESP_LOGI(TAG, "Web GUI attiva — attendo GPIO%d LOW per spegnimento", CORE_GPIO_MODE_CFG);
    } else {
        ESP_LOGW(TAG, "Web GUI non avviata — attendo GPIO%d LOW", CORE_GPIO_MODE_CFG);
    }

    while (core_gpio_is_mode_config()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "GPIO%d LOW — fine configurazione, invio DONE", CORE_GPIO_MODE_CFG);
    core_web_stop();
    core_gpio_hold_tpl_done(0);
}

static void run_pir_mode(void) {
    if (!core_gpio_is_d1_wake()) {
        core_gpio_boot_snapshot_t snap;
        core_gpio_get_boot_snapshot(&snap);
        ESP_LOGW(TAG, "PIR senza D1 — errore boot D1=%d D2=%d MODE=%d, AP+LED errore",
                 snap.d1_level, snap.d2_level, snap.mode_level);
        if (core_web_start(&g_settings, CORE_WEB_WIFI_AP_BOOT_ERROR, &snap)) {
            ESP_LOGI(TAG, "AP errore boot attivo — Web :%d", CORE_WEB_PORT);
        } else {
            ESP_LOGE(TAG, "AP errore boot fallito — solo LED errore");
            core_status_led_set_mode(CORE_LED_ERROR);
        }
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(TAG, "D1 attivo (GPIO%d LOW) — registrazione PIR", CORE_GPIO_D1_WAKE);
    disable_radio_for_recording();
    core_recorder_run_session(&g_settings);
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

    core_gpio_save_boot_snapshot();
    core_status_led_init();

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    core_gpio_log_inputs();

    if (core_gpio_is_mode_config()) {
        run_config_mode();
    } else {
        run_pir_mode();
    }
}

#include "CoreConfig.h"
#include "CoreGPIO.h"
#include "CoreRecorder.h"
#include "CoreSettings.h"
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
    if (core_web_start(&g_settings)) {
        ESP_LOGI(TAG, "Web GUI attiva — attendo GPIO%d LOW per spegnimento", CORE_GPIO_MODE_CFG);
    } else {
        ESP_LOGW(TAG, "Web GUI non avviata (WiFi non configurato?) — attendo GPIO%d LOW",
                 CORE_GPIO_MODE_CFG);
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
        ESP_LOGW(TAG, "PIR senza D1 attivo — errore boot, lampeggio LED");
        core_gpio_blink_error_forever();
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

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    core_gpio_log_inputs();

    if (core_gpio_is_mode_config()) {
        run_config_mode();
    } else {
        run_pir_mode();
    }
}

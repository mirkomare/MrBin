#include "CoreConfig.h"
#include "CoreEncFs.h"
#include "CoreGPIO.h"
#include "CoreRecorder.h"
#include "CoreSettings.h"
#include "CoreStatusLed.h"
#include "CoreWeb.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "mrbin_core";
static core_settings_t g_settings;

static void log_reset_reason(void) {
    esp_reset_reason_t r = esp_reset_reason();
    const char *s = "?";
    switch (r) {
        case ESP_RST_POWERON:   s = "POWERON (alimentazione applicata / TPL on)"; break;
        case ESP_RST_EXT:       s = "EXT (reset esterno pin)"; break;
        case ESP_RST_SW:        s = "SW (reset software)"; break;
        case ESP_RST_PANIC:     s = "PANIC (crash firmware!)"; break;
        case ESP_RST_INT_WDT:   s = "INT_WDT (watchdog interrupt!)"; break;
        case ESP_RST_TASK_WDT:  s = "TASK_WDT (watchdog task!)"; break;
        case ESP_RST_WDT:       s = "WDT (altro watchdog!)"; break;
        case ESP_RST_BROWNOUT:  s = "BROWNOUT (tensione caduta sotto soglia!)"; break;
        case ESP_RST_DEEPSLEEP: s = "DEEPSLEEP"; break;
        case ESP_RST_SDIO:      s = "SDIO"; break;
        default:                s = "UNKNOWN"; break;
    }
    ESP_LOGW(TAG, "Reset reason ultimo avvio: %s (code=%d)", s, (int)r);
}

static void disable_radio_for_recording(void) {
    ESP_LOGI(TAG, "Radio disabilitata per sessione registrazione");
}

static void recover_tmp_task(void *arg) {
    core_settings_t *settings = (core_settings_t *)arg;
    core_recorder_recover_tmp_files(settings);
    vTaskDelete(nullptr);
}

static void run_config_mode(void) {
    ESP_LOGI(TAG, "GPIO%d HIGH — modalità configurazione (WiFi/Web)", CORE_GPIO_MODE_CFG);

    core_gpio_mode_exit_session_begin();

    bool web_ok = false;
    if (g_settings.wifi_ssid[0] != 0) {
        web_ok = core_web_start(&g_settings, CORE_WEB_WIFI_STA, nullptr);
    } else {
        web_ok = core_web_start(&g_settings, CORE_WEB_WIFI_AP, nullptr);
    }

    if (web_ok) {
        ESP_LOGI(TAG, "Web GUI attiva — GPIO%d LOW stabile → spegnimento (DONE TPL)", CORE_GPIO_MODE_CFG);
    } else {
        ESP_LOGW(TAG, "Web GUI non avviata — GPIO%d LOW → spegnimento", CORE_GPIO_MODE_CFG);
    }

    xTaskCreate(recover_tmp_task, "recov_tmp", 8192, &g_settings, 2, nullptr);

    int64_t t0 = esp_timer_get_time();
    int64_t last_beat = t0;
    while (!core_gpio_is_mode_exit_triggered()) {
        vTaskDelay(pdMS_TO_TICKS(CORE_MODE_POLL_MS));

        int64_t now = esp_timer_get_time();
        if ((now - last_beat) >= 5000000) {
            last_beat = now;
            uint32_t up_s = (uint32_t)((now - t0) / 1000000);
            ESP_LOGI(TAG, "Config attiva: uptime %lu s (MODE=%d)",
                     (unsigned long)up_s, gpio_get_level(CORE_GPIO_MODE_CFG));
        }
    }

    ESP_LOGI(TAG, "Uscita config — stop Web/Live e loop DONE TPL");
    core_web_stop();
    core_gpio_mode_exit_session_end();
    core_gpio_hold_tpl_done(0);
}

static void run_pir_mode(void) {
    core_gpio_boot_snapshot_t snap;
    core_gpio_get_boot_snapshot(&snap);
    ESP_LOGI(TAG, "Operatività normale — registrazione (boot D1=%d D2=%d MODE=%d, start GPIO%d stop GPIO%d)",
             snap.d1_level, snap.d2_level, snap.mode_level,
             (int)g_settings.rec_gpio_start, (int)g_settings.rec_gpio_stop);
    disable_radio_for_recording();
    core_recorder_run_session(&g_settings);
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "MrBin CORE avvio (ESP32-P4 Waveshare)");
    log_reset_reason();

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
    core_gpio_set_rec_pins(g_settings.rec_gpio_start, g_settings.rec_gpio_stop);
    core_encfs_register();

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    core_gpio_log_inputs();

    if (core_gpio_is_mode_config()) {
        run_config_mode();
    } else {
        run_pir_mode();
    }
}

#include "CoreGPIO.h"
#include "CoreConfig.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "core_gpio";
static core_gpio_boot_snapshot_t s_boot_snapshot = {};
static gpio_num_t s_rec_start_gpio = CORE_GPIO_D1_WAKE;
static gpio_num_t s_rec_stop_gpio = CORE_GPIO_D2_END;
static int64_t s_rec_stop_low_since = -1;
static int s_prev_d1_level = -1;
static int s_prev_d2_level = -1;

bool core_gpio_init(void) {
    gpio_config_t pir_in_cfg = {
        .pin_bit_mask = (1ULL << CORE_GPIO_D1_WAKE) | (1ULL << CORE_GPIO_D2_END),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&pir_in_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio PIR input config failed: %s", esp_err_to_name(err));
        return false;
    }

    gpio_config_t mode_cfg = {
        .pin_bit_mask = (1ULL << CORE_GPIO_MODE_CFG),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&mode_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio MODE input config failed: %s", esp_err_to_name(err));
        return false;
    }

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << CORE_GPIO_TPL_DONE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&out_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio output config failed: %s", esp_err_to_name(err));
        return false;
    }
    gpio_set_level(CORE_GPIO_TPL_DONE, 0);
    return true;
}

void core_gpio_save_boot_snapshot(void) {
    s_boot_snapshot.d1_level = gpio_get_level(CORE_GPIO_D1_WAKE);
    s_boot_snapshot.d2_level = gpio_get_level(CORE_GPIO_D2_END);
    s_boot_snapshot.mode_level = gpio_get_level(CORE_GPIO_MODE_CFG);
}

void core_gpio_get_boot_snapshot(core_gpio_boot_snapshot_t *out) {
    if (out) {
        *out = s_boot_snapshot;
    }
}

bool core_gpio_is_d1_wake(void) {
    return gpio_get_level(CORE_GPIO_D1_WAKE) == 0;
}

bool core_gpio_is_d2_end(void) {
    if (core_gpio_is_mode_config()) {
        return false;
    }
    return gpio_get_level(CORE_GPIO_D2_END) == 0;
}

void core_gpio_set_rec_pins(uint8_t start_gpio, uint8_t stop_gpio) {
    s_rec_start_gpio = (gpio_num_t)start_gpio;
    s_rec_stop_gpio = (gpio_num_t)stop_gpio;
    ESP_LOGI(TAG, "Pin registrazione: start=GPIO%d stop=GPIO%d",
             (int)s_rec_start_gpio, (int)s_rec_stop_gpio);
}

bool core_gpio_is_rec_start_active(void) {
    return gpio_get_level(s_rec_start_gpio) == 0;
}

void core_gpio_log_pin_edges(void) {
    int d1 = gpio_get_level(CORE_GPIO_D1_WAKE);
    int d2 = gpio_get_level(CORE_GPIO_D2_END);
    if (d1 != s_prev_d1_level) {
        ESP_LOGI(TAG, "D1 (GPIO%d) -> %s%s", (int)CORE_GPIO_D1_WAKE, d1 ? "HIGH" : "LOW",
                 (s_rec_stop_gpio == CORE_GPIO_D1_WAKE) ? " [pin STOP]" : "");
        s_prev_d1_level = d1;
    }
    if (d2 != s_prev_d2_level) {
        ESP_LOGI(TAG, "D2 (GPIO%d) -> %s%s", (int)CORE_GPIO_D2_END, d2 ? "HIGH" : "LOW",
                 (s_rec_stop_gpio == CORE_GPIO_D2_END) ? " [pin STOP]" : "");
        s_prev_d2_level = d2;
    }
}

bool core_gpio_is_rec_stop_active(void) {
    if (core_gpio_is_mode_config()) {
        return false;
    }
    return gpio_get_level(s_rec_stop_gpio) == 0;
}

bool core_gpio_boot_was_pir_wake(void) {
    // Wake TPL5110 (manual, EN LOW): snapshot D1 fisico al reset — il PIR può essere già HIGH
    // quando l'ESP finisce il boot, quindi non usare lettura runtime per validare il wake.
    if (s_boot_snapshot.d1_level == 0) {
        return true;
    }
    if (s_rec_start_gpio == CORE_GPIO_D2_END && s_boot_snapshot.d2_level == 0) {
        return true;
    }
    return false;
}

static esp_timer_handle_t s_stop_timer = nullptr;
static volatile bool      s_rec_stop_latched = false;

// Campiona il pin di stop ogni CORE_REC_STOP_POLL_MS in un timer dedicato: il
// rilevamento è preciso e indipendente dalla velocità del loop di cattura.
static void rec_stop_poll_cb(void *arg) {
    (void)arg;
    if (s_rec_stop_latched) {
        return;
    }
    if (!core_gpio_is_rec_stop_active()) {
        s_rec_stop_low_since = -1;   // HIGH (o config): azzero il debounce
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_rec_stop_low_since < 0) {
        s_rec_stop_low_since = now;
        return;
    }
    if ((now - s_rec_stop_low_since) >= ((int64_t)CORE_REC_STOP_DEBOUNCE_MS * 1000)) {
        s_rec_stop_latched = true;
        ESP_LOGI(TAG, "Pin STOP (GPIO%d) LOW stabile %d ms -> STOP",
                 (int)s_rec_stop_gpio, CORE_REC_STOP_DEBOUNCE_MS);
    }
}

void core_gpio_rec_stop_session_begin(void) {
    s_rec_stop_low_since = -1;
    s_rec_stop_latched = false;
    s_prev_d1_level = -1;
    s_prev_d2_level = -1;
    if (!s_stop_timer) {
        esp_timer_create_args_t args = {
            .callback = rec_stop_poll_cb,
            .name = "recstop",
        };
        if (esp_timer_create(&args, &s_stop_timer) != ESP_OK) {
            s_stop_timer = nullptr;
        }
    }
    if (s_stop_timer) {
        esp_timer_stop(s_stop_timer);  // no-op se non attivo
        esp_timer_start_periodic(s_stop_timer, (uint64_t)CORE_REC_STOP_POLL_MS * 1000);
    }
}

void core_gpio_rec_stop_session_end(void) {
    if (s_stop_timer) {
        esp_timer_stop(s_stop_timer);
    }
    s_rec_stop_low_since = -1;
    s_rec_stop_latched = false;
}

bool core_gpio_is_rec_stop_triggered(void) {
    // Il debounce (50 ms LOW stabile) è valutato dal timer dedicato: qui leggo il latch.
    if (s_stop_timer) {
        return s_rec_stop_latched;
    }
    // Fallback (timer non disponibile): valutazione inline come prima.
    if (!core_gpio_is_rec_stop_active()) {
        s_rec_stop_low_since = -1;
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (s_rec_stop_low_since < 0) {
        s_rec_stop_low_since = now;
        return false;
    }
    return (now - s_rec_stop_low_since) >= ((int64_t)CORE_REC_STOP_DEBOUNCE_MS * 1000);
}

bool core_gpio_is_mode_config(void) {
    return gpio_get_level(CORE_GPIO_MODE_CFG) == 1;
}

void core_gpio_signal_tpl_done(void) {
    gpio_set_level(CORE_GPIO_TPL_DONE, 1);
}

void core_gpio_log_inputs(void) {
    ESP_LOGI(TAG, "GPIO MODE(%d)=%d D1(%d)=%d D2(%d)=%d DONE(%d)=%d rec_start=%d rec_stop=%d (0=LOW)",
             CORE_GPIO_MODE_CFG, gpio_get_level(CORE_GPIO_MODE_CFG),
             CORE_GPIO_D1_WAKE, gpio_get_level(CORE_GPIO_D1_WAKE),
             CORE_GPIO_D2_END, gpio_get_level(CORE_GPIO_D2_END),
             CORE_GPIO_TPL_DONE, gpio_get_level(CORE_GPIO_TPL_DONE),
             (int)s_rec_start_gpio, (int)s_rec_stop_gpio);
}

void core_gpio_hold_tpl_done(uint32_t delay_ms) {
    ESP_LOGI(TAG, "Attendo %lu ms prima del DONE", (unsigned long)delay_ms);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    ESP_LOGI(TAG, "Loop spegnimento TPL: DONE (GPIO%d) pulsato HIGH %d ms / LOW %d ms fino a stacco alimentazione",
             CORE_GPIO_TPL_DONE, CORE_TPL_DONE_PULSE_MS, CORE_TPL_DONE_GAP_MS);
    // Loop di spegnimento: DONE HIGH 1 s, riposo 100 ms, all'infinito.
    // Si esce solo quando il TPL toglie alimentazione (power off fisico).
    while (true) {
        gpio_set_level(CORE_GPIO_TPL_DONE, 1);
        vTaskDelay(pdMS_TO_TICKS(CORE_TPL_DONE_PULSE_MS));
        gpio_set_level(CORE_GPIO_TPL_DONE, 0);
        vTaskDelay(pdMS_TO_TICKS(CORE_TPL_DONE_GAP_MS));
    }
}

#include "CoreGPIO.h"
#include "CoreConfig.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "core_gpio";

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

bool core_gpio_is_d1_wake(void) {
    return gpio_get_level(CORE_GPIO_D1_WAKE) == 0;
}

bool core_gpio_is_d2_end(void) {
    if (core_gpio_is_mode_config()) {
        return false;
    }
    return gpio_get_level(CORE_GPIO_D2_END) == 0;
}

bool core_gpio_is_mode_config(void) {
    return gpio_get_level(CORE_GPIO_MODE_CFG) == 1;
}

void core_gpio_signal_tpl_done(void) {
    gpio_set_level(CORE_GPIO_TPL_DONE, 1);
}

void core_gpio_log_inputs(void) {
    ESP_LOGI(TAG, "GPIO MODE(%d)=%d D1(%d)=%d D2(%d)=%d DONE(%d)=%d (0=LOW)",
             CORE_GPIO_MODE_CFG, gpio_get_level(CORE_GPIO_MODE_CFG),
             CORE_GPIO_D1_WAKE, gpio_get_level(CORE_GPIO_D1_WAKE),
             CORE_GPIO_D2_END, gpio_get_level(CORE_GPIO_D2_END),
             CORE_GPIO_TPL_DONE, gpio_get_level(CORE_GPIO_TPL_DONE));
}

void core_gpio_hold_tpl_done(uint32_t delay_ms) {
    ESP_LOGI(TAG, "Attendo %lu ms prima del DONE", (unsigned long)delay_ms);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    core_gpio_signal_tpl_done();
    ESP_LOGI(TAG, "DONE inviato (GPIO%d HIGH) — spegnimento TPL", CORE_GPIO_TPL_DONE);
    while (true) {
        core_gpio_signal_tpl_done();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void core_gpio_blink_error_forever(void) {
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << CORE_GPIO_STATUS_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&led_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED errore GPIO%d config fallita: %s", CORE_GPIO_STATUS_LED, esp_err_to_name(err));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    const int off_level = CORE_STATUS_LED_ON_LEVEL ? 0 : 1;
    gpio_set_level(CORE_GPIO_STATUS_LED, off_level);
    ESP_LOGW(TAG, "Errore boot PIR — LED GPIO%d lampeggio %d ms", CORE_GPIO_STATUS_LED, CORE_STATUS_LED_BLINK_MS);

    while (true) {
        gpio_set_level(CORE_GPIO_STATUS_LED, CORE_STATUS_LED_ON_LEVEL);
        vTaskDelay(pdMS_TO_TICKS(CORE_STATUS_LED_BLINK_MS));
        gpio_set_level(CORE_GPIO_STATUS_LED, off_level);
        vTaskDelay(pdMS_TO_TICKS(CORE_STATUS_LED_BLINK_MS));
    }
}

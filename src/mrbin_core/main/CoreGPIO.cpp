#include "CoreGPIO.h"
#include "CoreConfig.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "core_gpio";

bool core_gpio_init(void) {
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << CORE_GPIO_D1_WAKE) | (1ULL << CORE_GPIO_D2_END),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&in_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio input config failed: %s", esp_err_to_name(err));
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
    return gpio_get_level(CORE_GPIO_D2_END) == 0;
}

void core_gpio_signal_tpl_done(void) {
    gpio_set_level(CORE_GPIO_TPL_DONE, 1);
}

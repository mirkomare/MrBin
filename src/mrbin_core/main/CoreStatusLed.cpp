#include "CoreStatusLed.h"
#include "CoreConfig.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "core_status_led";

static TaskHandle_t s_led_task = nullptr;
static SemaphoreHandle_t s_mode_mtx = nullptr;
static core_status_led_mode_t s_mode = CORE_LED_OFF;
static const int s_off_level = CORE_STATUS_LED_ON_LEVEL ? 0 : 1;

static void led_write(int level) {
    gpio_set_level(CORE_GPIO_STATUS_LED, level);
}

static core_status_led_mode_t led_get_mode(void) {
    core_status_led_mode_t mode = CORE_LED_OFF;
    if (s_mode_mtx && xSemaphoreTake(s_mode_mtx, portMAX_DELAY) == pdTRUE) {
        mode = s_mode;
        xSemaphoreGive(s_mode_mtx);
    }
    return mode;
}

static void led_set_mode_locked(core_status_led_mode_t mode) {
    if (s_mode_mtx && xSemaphoreTake(s_mode_mtx, portMAX_DELAY) == pdTRUE) {
        s_mode = mode;
        xSemaphoreGive(s_mode_mtx);
    }
}

static void led_pulse_ms(int on_ms) {
    led_write(CORE_STATUS_LED_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    led_write(s_off_level);
}

static void led_task(void *) {
    while (true) {
        core_status_led_mode_t mode = led_get_mode();

        switch (mode) {
        case CORE_LED_ERROR:
            led_pulse_ms(CORE_STATUS_LED_ERROR_BLINK_MS);
            vTaskDelay(pdMS_TO_TICKS(CORE_STATUS_LED_ERROR_BLINK_MS));
            break;

        case CORE_LED_AP:
            led_pulse_ms(150);
            vTaskDelay(pdMS_TO_TICKS(CORE_STATUS_LED_AP_BLINK_MS));
            break;

        case CORE_LED_STA_CONNECTED: {
            for (int i = 0; i < CORE_STATUS_LED_STA_BLINK_COUNT; ++i) {
                led_pulse_ms(CORE_STATUS_LED_STA_PULSE_MS);
                if (i + 1 < CORE_STATUS_LED_STA_BLINK_COUNT) {
                    vTaskDelay(pdMS_TO_TICKS(CORE_STATUS_LED_STA_BLINK_MS));
                }
            }
            led_set_mode_locked(CORE_LED_OFF);
            led_write(s_off_level);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }

        case CORE_LED_OFF:
        default:
            led_write(s_off_level);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        }
    }
}

bool core_status_led_init(void) {
    if (s_led_task) {
        return true;
    }

    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << CORE_GPIO_STATUS_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&led_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d config fallita: %s", CORE_GPIO_STATUS_LED, esp_err_to_name(err));
        return false;
    }
    led_write(s_off_level);

    s_mode_mtx = xSemaphoreCreateMutex();
    if (!s_mode_mtx) {
        ESP_LOGE(TAG, "mutex LED fallito");
        return false;
    }

    if (xTaskCreate(led_task, "status_led", 2048, nullptr, 2, &s_led_task) != pdPASS) {
        ESP_LOGE(TAG, "task LED fallita");
        vSemaphoreDelete(s_mode_mtx);
        s_mode_mtx = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "LED GPIO%d pronto", CORE_GPIO_STATUS_LED);
    return true;
}

void core_status_led_set_mode(core_status_led_mode_t mode) {
    if (!s_mode_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mode_mtx, portMAX_DELAY) == pdTRUE) {
        s_mode = mode;
        xSemaphoreGive(s_mode_mtx);
    }
}

void core_status_led_notify_sta_connected(void) {
    core_status_led_set_mode(CORE_LED_STA_CONNECTED);
}

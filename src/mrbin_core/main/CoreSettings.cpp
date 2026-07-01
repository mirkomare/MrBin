#include "CoreSettings.h"
#include "CoreConfig.h"

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "core_settings";

static nvs_handle_t s_nvs = 0;

void core_settings_rec_pins_set_defaults(core_settings_t *s) {
    if (!s) {
        return;
    }
    s->rec_gpio_start = (uint8_t)CORE_GPIO_D1_WAKE;
    s->rec_gpio_stop = (uint8_t)CORE_GPIO_D2_END;
}

bool core_settings_rec_pins_valid(uint8_t start_gpio, uint8_t stop_gpio) {
    if (start_gpio == stop_gpio) {
        return false;
    }
    if (start_gpio != (uint8_t)CORE_GPIO_D1_WAKE && start_gpio != (uint8_t)CORE_GPIO_D2_END) {
        return false;
    }
    if (stop_gpio != (uint8_t)CORE_GPIO_D1_WAKE && stop_gpio != (uint8_t)CORE_GPIO_D2_END) {
        return false;
    }
    return true;
}

bool core_settings_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_open(CORE_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool core_settings_load(core_settings_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->d2_post_delay_ms = CORE_D2_POST_DELAY_MS;
    core_settings_rec_pins_set_defaults(out);

    uint32_t id = 0;
    if (nvs_get_u32(s_nvs, "core_id", &id) == ESP_OK && id >= CORE_ID_MIN && id <= CORE_ID_MAX) {
        out->core_id = id;
    }

    size_t key_len = sizeof(out->aes_key);
    if (nvs_get_blob(s_nvs, "aes_key", out->aes_key, &key_len) == ESP_OK && key_len == sizeof(out->aes_key)) {
        out->aes_key_valid = true;
    }

    size_t ssid_len = sizeof(out->wifi_ssid);
    nvs_get_str(s_nvs, "wifi_ssid", out->wifi_ssid, &ssid_len);

    size_t pass_len = sizeof(out->wifi_pass);
    nvs_get_str(s_nvs, "wifi_pass", out->wifi_pass, &pass_len);

    uint32_t delay_ms = 0;
    if (nvs_get_u32(s_nvs, "d2_delay", &delay_ms) == ESP_OK &&
        delay_ms >= CORE_D2_DELAY_MIN_MS && delay_ms <= CORE_D2_DELAY_MAX_MS) {
        out->d2_post_delay_ms = delay_ms;
    }

    uint8_t start_gpio = 0;
    uint8_t stop_gpio = 0;
    if (nvs_get_u8(s_nvs, "rec_gpio_start", &start_gpio) == ESP_OK &&
        nvs_get_u8(s_nvs, "rec_gpio_stop", &stop_gpio) == ESP_OK &&
        core_settings_rec_pins_valid(start_gpio, stop_gpio)) {
        out->rec_gpio_start = start_gpio;
        out->rec_gpio_stop = stop_gpio;
    } else if (nvs_get_u8(s_nvs, "rec_gpio_start", &start_gpio) == ESP_OK ||
               nvs_get_u8(s_nvs, "rec_gpio_stop", &stop_gpio) == ESP_OK) {
        ESP_LOGW(TAG, "Pin registrazione NVS non validi (start=%u stop=%u) — ripristino default D1/D2",
                 (unsigned)start_gpio, (unsigned)stop_gpio);
    }
    return true;
}

bool core_settings_save(const core_settings_t *in) {
    if (!in) return false;
    esp_err_t err = nvs_set_u32(s_nvs, "core_id", in->core_id);
    if (err != ESP_OK) return false;
    if (in->aes_key_valid) {
        err = nvs_set_blob(s_nvs, "aes_key", in->aes_key, sizeof(in->aes_key));
        if (err != ESP_OK) return false;
    }
    err = nvs_set_str(s_nvs, "wifi_ssid", in->wifi_ssid);
    if (err != ESP_OK) return false;
    err = nvs_set_str(s_nvs, "wifi_pass", in->wifi_pass);
    if (err != ESP_OK) return false;
    err = nvs_set_u32(s_nvs, "d2_delay", in->d2_post_delay_ms);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "rec_gpio_start", in->rec_gpio_start);
    if (err != ESP_OK) return false;
    err = nvs_set_u8(s_nvs, "rec_gpio_stop", in->rec_gpio_stop);
    if (err != ESP_OK) return false;
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit fallito: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Impostazioni salvate (id=%05lu delay=%lu ms stop=GPIO%u)",
             (unsigned long)in->core_id, (unsigned long)in->d2_post_delay_ms,
             (unsigned)in->rec_gpio_stop);
    return true;
}

bool core_settings_ensure_id(core_settings_t *s) {
    if (!s) return false;
    if (s->core_id >= CORE_ID_MIN && s->core_id <= CORE_ID_MAX) {
        ESP_LOGI(TAG, "CORE ID esistente: %05lu", (unsigned long)s->core_id);
        return true;
    }
    uint32_t range = (CORE_ID_MAX - CORE_ID_MIN + 1);
    s->core_id = CORE_ID_MIN + (esp_random() % range);
    ESP_LOGI(TAG, "Nuovo CORE ID creato: %05lu", (unsigned long)s->core_id);
    return core_settings_save(s);
}

bool core_settings_ensure_aes_key(core_settings_t *s) {
    if (!s) return false;
    if (s->aes_key_valid) {
        ESP_LOGI(TAG, "Chiave AES-128 esistente, riutilizzo");
        return true;
    }
    esp_fill_random(s->aes_key, sizeof(s->aes_key));
    s->aes_key_valid = true;
    ESP_LOGI(TAG, "Nuova chiave AES-128 creata");
    return core_settings_save(s);
}

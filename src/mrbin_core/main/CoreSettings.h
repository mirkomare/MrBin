#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t core_id;           // 5 cifre
    uint8_t  aes_key[16];       // AES-128, creata al primo avvio
    bool     aes_key_valid;
    char     wifi_ssid[32];
    char     wifi_pass[64];
    uint32_t d2_post_delay_ms;  // default 5000
    uint8_t  rec_gpio_start;    // GPIO attivazione registrazione (default D1 = 28)
    uint8_t  rec_gpio_stop;     // GPIO stop registrazione (default D2 = 21)
} core_settings_t;

bool core_settings_rec_pins_valid(uint8_t start_gpio, uint8_t stop_gpio);
void core_settings_rec_pins_set_defaults(core_settings_t *s);

bool core_settings_init(void);
bool core_settings_load(core_settings_t *out);
bool core_settings_save(const core_settings_t *in);

bool core_settings_ensure_id(core_settings_t *s);
bool core_settings_ensure_aes_key(core_settings_t *s);

#ifdef __cplusplus
}
#endif

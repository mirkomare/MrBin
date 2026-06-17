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
    char     wifi_ssid[33];
    char     wifi_pass[65];
    uint32_t d2_post_delay_ms;  // default 5000
} core_settings_t;

bool core_settings_init(void);
bool core_settings_load(core_settings_t *out);
bool core_settings_save(const core_settings_t *in);

bool core_settings_ensure_id(core_settings_t *s);
bool core_settings_ensure_aes_key(core_settings_t *s);

#ifdef __cplusplus
}
#endif

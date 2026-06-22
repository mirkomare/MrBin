#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int d1_level;
    int d2_level;
    int mode_level;
} core_gpio_boot_snapshot_t;

bool core_gpio_init(void);
void core_gpio_save_boot_snapshot(void);
void core_gpio_get_boot_snapshot(core_gpio_boot_snapshot_t *out);
bool core_gpio_is_d1_wake(void);      // GPIO28 LOW
bool core_gpio_is_d2_end(void);       // GPIO21 LOW (ignorato se GPIO29 HIGH)
bool core_gpio_is_mode_config(void);  // GPIO29 HIGH = WiFi/Web manuale
void core_gpio_signal_tpl_done(void); // GPIO23 HIGH permanente
void core_gpio_log_inputs(void);
void core_gpio_hold_tpl_done(uint32_t delay_ms); // delay + DONE permanente (non ritorna)

#ifdef __cplusplus
}
#endif

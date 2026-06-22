#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool core_gpio_init(void);
bool core_gpio_is_d1_wake(void);      // GPIO28 LOW
bool core_gpio_is_d2_end(void);       // GPIO21 LOW (ignorato se GPIO29 HIGH)
bool core_gpio_is_mode_config(void);  // GPIO29 HIGH = WiFi/Web manuale
void core_gpio_signal_tpl_done(void); // GPIO23 HIGH permanente
void core_gpio_log_inputs(void);
void core_gpio_hold_tpl_done(uint32_t delay_ms); // delay + DONE permanente (non ritorna)
void core_gpio_blink_error_forever(void);        // lampeggio rapido errore (non ritorna)

#ifdef __cplusplus
}
#endif

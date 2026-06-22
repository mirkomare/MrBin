#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CORE_LED_OFF = 0,
    CORE_LED_STA_CONNECTED,  // 3 lampeggi @ 500 ms poi OFF
    CORE_LED_AP,             // 1 lampeggio ogni 2 s (fino a cambio modalità)
    CORE_LED_ERROR,          // lampeggio rapido 100 ms continuo
} core_status_led_mode_t;

bool core_status_led_init(void);
void core_status_led_set_mode(core_status_led_mode_t mode);
void core_status_led_notify_sta_connected(void);

#ifdef __cplusplus
}
#endif

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
void core_gpio_set_rec_pins(uint8_t start_gpio, uint8_t stop_gpio);
bool core_gpio_boot_was_pir_wake(void);    // snapshot boot: wake PIR/TPL valido
void core_gpio_rec_stop_session_begin(void); // avvia task polling stop/D2 + reset debounce
void core_gpio_rec_stop_session_end(void);   // ferma task polling stop a fine sessione
bool core_gpio_is_rec_stop_triggered(void);  // true se pin stop LOW stabile >= 50 ms (latch)
void core_gpio_mode_exit_session_begin(void); // timer polling GPIO29 (MODE) in config
void core_gpio_mode_exit_session_end(void);
bool core_gpio_is_mode_exit_triggered(void);  // true se MODE LOW stabile (uscita config → DONE)
void core_gpio_log_pin_edges(void);          // log seriale su transizioni HIGH/LOW di D1 e D2
bool core_gpio_is_rec_start_active(void);  // pin attivazione configurato LOW (runtime)
bool core_gpio_is_rec_stop_active(void);   // pin stop configurato LOW (ignorato se GPIO29 HIGH)
void core_gpio_signal_tpl_done(void); // GPIO23 HIGH permanente
void core_gpio_log_inputs(void);
void core_gpio_hold_tpl_done(uint32_t delay_ms); // delay + DONE permanente (non ritorna)

#ifdef __cplusplus
}
#endif

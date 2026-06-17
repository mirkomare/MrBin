#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool core_gpio_init(void);
bool core_gpio_is_d1_wake(void);      // GPIO28 LOW
bool core_gpio_is_d2_end(void);       // GPIO21 LOW
void core_gpio_signal_tpl_done(void); // GPIO23 HIGH permanente

#ifdef __cplusplus
}
#endif

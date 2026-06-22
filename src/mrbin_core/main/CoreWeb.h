#pragma once

#include "CoreGPIO.h"
#include "CoreSettings.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CORE_WEB_WIFI_STA = 0,
    CORE_WEB_WIFI_AP,
    CORE_WEB_WIFI_AP_BOOT_ERROR,
} core_web_wifi_mode_t;

bool core_web_start(core_settings_t *settings, core_web_wifi_mode_t wifi_mode,
                    const core_gpio_boot_snapshot_t *boot_error);
void core_web_stop(void);

#ifdef __cplusplus
}
#endif

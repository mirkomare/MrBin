#pragma once

#include "CoreSettings.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool core_web_start(core_settings_t *settings);
void core_web_stop(void);

#ifdef __cplusplus
}
#endif

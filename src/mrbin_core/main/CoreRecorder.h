#pragma once

#include "CoreSettings.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Registra fino a D2: capture immediato in PSRAM, SD in parallelo, mux MP4 su SD
bool core_recorder_run_session(const core_settings_t *settings);

#ifdef __cplusplus
}
#endif

#pragma once

#include "CoreSettings.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Registra fino a ricezione D2, poi cifra il file MP4
bool core_recorder_run_session(const core_settings_t *settings);

#ifdef __cplusplus
}
#endif

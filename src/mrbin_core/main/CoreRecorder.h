#pragma once

#include "CoreSettings.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     active;
    bool     last_saved;
    uint32_t frame_count;
    char     last_path[128];
    char     error[96];
} core_recorder_manual_status_t;

// Registra fino a D2: capture immediato in PSRAM, SD in parallelo, mux MP4 su SD
bool core_recorder_run_session(const core_settings_t *settings);

// Recupera i .mp4.tmp orfani (registrazioni interrotte da spegnimento improvviso):
// cifra e finalizza quelli validi, elimina quelli corrotti. Ritorna il numero di
// file rimossi/recuperati. Da chiamare quando NON si sta registrando (config mode).
int core_recorder_recover_tmp_files(const core_settings_t *settings);

// Registrazione manuale da Web (stop esplicito, nessun DONE TPL)
bool core_recorder_manual_start(const core_settings_t *settings);
bool core_recorder_manual_stop(void);
bool core_recorder_manual_wait_done(uint32_t timeout_ms);
bool core_recorder_manual_is_active(void);
void core_recorder_manual_get_status(core_recorder_manual_status_t *out);

#ifdef __cplusplus
}
#endif

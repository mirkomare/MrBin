#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool core_sd_init(void);
bool core_sd_is_mounted(void);
bool core_sd_format(void);
uint64_t core_sd_free_bytes(void);
bool core_sd_ensure_space(uint64_t min_free);

// Crea path directory DDMMAAAA sotto /sdcard
bool core_sd_make_day_dir(char *out_path, size_t out_len);

// Nome file: ID_DDMM_hhmmss.mp4 (path completo in out_path)
bool core_sd_make_recording_path(const char *day_dir, uint32_t core_id,
                                 char *out_path, size_t out_len);

#ifdef __cplusplus
}
#endif

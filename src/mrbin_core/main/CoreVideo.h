#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool core_video_init(void);
bool core_video_is_ready(void);
void core_video_deinit(void);
bool core_video_prepare_for_stream(void);

#ifdef __cplusplus
}
#endif

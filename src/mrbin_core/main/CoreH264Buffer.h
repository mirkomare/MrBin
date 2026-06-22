#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct core_h264_frame {
    uint32_t             pts_ms;
    uint32_t             size;
    struct core_h264_frame *next;
    uint8_t              data[];
} core_h264_frame_t;

typedef struct {
    core_h264_frame_t *head;
    core_h264_frame_t *tail;
    uint32_t           frame_count;
    uint32_t           total_bytes;
    uint32_t           max_bytes;
} core_h264_buffer_t;

void core_h264_buffer_init(core_h264_buffer_t *buf, uint32_t max_bytes);
void core_h264_buffer_clear(core_h264_buffer_t *buf);
bool core_h264_buffer_push(core_h264_buffer_t *buf, uint32_t pts_ms, const uint8_t *data, uint32_t size);
bool core_h264_buffer_extract_h264_config(const core_h264_buffer_t *buf, uint8_t *out, int out_cap, int *out_len);
bool core_h264_is_idr_nal(const uint8_t *data, uint32_t size);

#ifdef __cplusplus
}
#endif

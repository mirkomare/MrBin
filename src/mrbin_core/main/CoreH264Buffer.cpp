#include "CoreH264Buffer.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "core_h264_buf";

static int find_nal_start(const uint8_t *data, uint32_t size, uint32_t offset, uint32_t *nal_off) {
    for (uint32_t i = offset; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            *nal_off = i;
            return 3;
        }
        if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            *nal_off = i;
            return 4;
        }
    }
    return 0;
}

bool core_h264_is_idr_nal(const uint8_t *data, uint32_t size) {
    if (!data || size < 5) {
        return false;
    }
    uint32_t off = 0;
    uint32_t nal_off = 0;
    int sc = find_nal_start(data, size, 0, &nal_off);
    while (sc > 0) {
        uint32_t hdr = nal_off + sc;
        if (hdr < size) {
            uint8_t nal_type = data[hdr] & 0x1f;
            if (nal_type == 5) {
                return true;
            }
        }
        off = nal_off + sc + 1;
        sc = find_nal_start(data, size, off, &nal_off);
    }
    return false;
}

void core_h264_buffer_init(core_h264_buffer_t *buf, uint32_t max_bytes) {
    memset(buf, 0, sizeof(*buf));
    buf->max_bytes = max_bytes;
}

void core_h264_buffer_clear(core_h264_buffer_t *buf) {
    if (!buf) {
        return;
    }
    core_h264_frame_t *cur = buf->head;
    while (cur) {
        core_h264_frame_t *next = cur->next;
        heap_caps_free(cur);
        cur = next;
    }
    buf->head = buf->tail = nullptr;
    buf->frame_count = 0;
    buf->total_bytes = 0;
}

bool core_h264_buffer_push(core_h264_buffer_t *buf, uint32_t pts_ms, const uint8_t *data, uint32_t size) {
    if (!buf || !data || size == 0) {
        return false;
    }
    if (buf->total_bytes + size > buf->max_bytes) {
        ESP_LOGW(TAG, "Buffer PSRAM pieno (%lu / %lu bytes)", (unsigned long)buf->total_bytes, (unsigned long)buf->max_bytes);
        return false;
    }

    core_h264_frame_t *frame = (core_h264_frame_t *)heap_caps_malloc(sizeof(core_h264_frame_t) + size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) {
        ESP_LOGE(TAG, "Alloc frame PSRAM fallita (%lu bytes)", (unsigned long)size);
        return false;
    }
    frame->pts_ms = pts_ms;
    frame->size = size;
    frame->next = nullptr;
    memcpy(frame->data, data, size);

    if (buf->tail) {
        buf->tail->next = frame;
    } else {
        buf->head = frame;
    }
    buf->tail = frame;
    buf->frame_count++;
    buf->total_bytes += size;
    return true;
}

bool core_h264_buffer_extract_h264_config(const core_h264_buffer_t *buf, uint8_t *out, int out_cap, int *out_len) {
    if (!buf || !out || !out_len || out_cap < 8) {
        return false;
    }
    *out_len = 0;

    for (core_h264_frame_t *f = buf->head; f; f = f->next) {
        uint32_t off = 0;
        uint32_t nal_off = 0;
        int sc = find_nal_start(f->data, f->size, 0, &nal_off);
        while (sc > 0) {
            uint32_t hdr = nal_off + sc;
            if (hdr >= f->size) {
                break;
            }
            uint8_t nal_type = f->data[hdr] & 0x1f;
            uint32_t next_off = f->size;
            uint32_t next_nal = 0;
            int next_sc = find_nal_start(f->data, f->size, hdr + 1, &next_nal);
            if (next_sc > 0) {
                next_off = next_nal;
            }
            uint32_t nal_size = next_off - nal_off;
            if ((nal_type == 7 || nal_type == 8) && (int)(*out_len + nal_size) <= out_cap) {
                memcpy(out + *out_len, f->data + nal_off, nal_size);
                *out_len += (int)nal_size;
            }
            off = hdr + 1;
            sc = find_nal_start(f->data, f->size, off, &nal_off);
        }
        if (*out_len > 0) {
            return true;
        }
    }
    return false;
}

#include "CoreLive.h"
#include "CoreConfig.h"
#include "CoreVideo.h"

#include "encoder/esp_video_enc_default.h"
#include "esp_capture.h"
#include "esp_capture_sink.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "impl/esp_capture_video_v4l2_src.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "core_live";
static const char *MJPEG_BOUNDARY = "mrbinframe";

#define LIVE_STREAM_WORKER_STACK  32768
#define LIVE_STREAM_WORKER_PRIO   5

static esp_capture_handle_t s_capture = nullptr;
static esp_capture_video_src_if_t *s_vsrc = nullptr;
static esp_capture_sink_handle_t s_sink = nullptr;
static bool s_running = false;
static volatile bool s_abort_requested = false;

static QueueHandle_t s_stream_queue = nullptr;
static SemaphoreHandle_t s_worker_ready = nullptr;
static TaskHandle_t s_worker_task = nullptr;
static volatile bool s_job_active = false;

typedef struct {
    httpd_req_t *req;
} live_stream_job_t;

bool core_live_start(void) {
    if (s_running) return true;
    s_abort_requested = false;

    if (!core_video_prepare_for_stream()) {
        ESP_LOGE(TAG, "Video init fallita");
        return false;
    }

    esp_video_enc_register_default();

    esp_capture_video_v4l2_src_cfg_t v4l2_cfg = {
        .buf_count = 4,
    };
    strncpy(v4l2_cfg.dev_name, CORE_VIDEO_DEV_NAME, sizeof(v4l2_cfg.dev_name) - 1);

    s_vsrc = esp_capture_new_video_v4l2_src(&v4l2_cfg);
    if (!s_vsrc) {
        ESP_LOGE(TAG, "Sorgente V4L2 non disponibile");
        esp_video_enc_unregister_default();
        return false;
    }

    esp_capture_cfg_t cap_cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_NONE,
        .audio_src = nullptr,
        .video_src = s_vsrc,
        .share_overlay = false,
    };

    esp_capture_err_t err = esp_capture_open(&cap_cfg, &s_capture);
    if (err != ESP_CAPTURE_ERR_OK || !s_capture) {
        ESP_LOGE(TAG, "esp_capture_open fallita (%d)", err);
        free(s_vsrc);
        s_vsrc = nullptr;
        esp_video_enc_unregister_default();
        return false;
    }

    esp_capture_sink_cfg_t sink_cfg = {
        .video_info = {
            .format_id = ESP_CAPTURE_FMT_ID_MJPEG,
            .width = CORE_LIVE_WIDTH,
            .height = CORE_LIVE_HEIGHT,
            .fps = CORE_LIVE_FPS,
        },
    };

    err = esp_capture_sink_setup(s_capture, 0, &sink_cfg, &s_sink);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_sink_setup fallita (%d)", err);
        esp_capture_close(s_capture);
        s_capture = nullptr;
        free(s_vsrc);
        s_vsrc = nullptr;
        esp_video_enc_unregister_default();
        return false;
    }

    esp_capture_sink_enable(s_sink, ESP_CAPTURE_RUN_MODE_ALWAYS);

    err = esp_capture_start(s_capture);
    if (err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "esp_capture_start fallita (%d)", err);
        esp_capture_close(s_capture);
        s_capture = nullptr;
        free(s_vsrc);
        s_vsrc = nullptr;
        s_sink = nullptr;
        esp_video_enc_unregister_default();
        return false;
    }

    s_running = true;
    ESP_LOGI(TAG, "Live MJPEG %dx%d@%d", CORE_LIVE_WIDTH, CORE_LIVE_HEIGHT, CORE_LIVE_FPS);
    return true;
}

void core_live_stop(void) {
    if (!s_running) return;

    esp_capture_stop(s_capture);
    esp_capture_close(s_capture);
    s_capture = nullptr;
    s_sink = nullptr;

    if (s_vsrc) {
        free(s_vsrc);
        s_vsrc = nullptr;
    }

    esp_video_enc_unregister_default();
    s_running = false;
    ESP_LOGI(TAG, "Live fermato");
}

void core_live_request_stop(void) {
    s_abort_requested = true;
}

bool core_live_is_running(void) {
    return s_running;
}

bool core_live_wait_idle(uint32_t timeout_ms) {
    int64_t t0 = esp_timer_get_time();
    while (s_running || s_job_active) {
        if ((uint32_t)((esp_timer_get_time() - t0) / 1000) >= timeout_ms) {
            ESP_LOGW(TAG, "Timeout attesa live idle (running=%d job=%d)",
                     (int)s_running, (int)s_job_active);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

static esp_err_t core_live_stream_impl(httpd_req_t *req) {
    if (!core_live_start()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera non disponibile");
        return ESP_FAIL;
    }

    char content_type[72];
    snprintf(content_type, sizeof(content_type),
             "multipart/x-mixed-replace; boundary=%s", MJPEG_BOUNDARY);
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Connection", "close");

    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO,
    };

    char part_hdr[96];
    uint32_t sent_frames = 0;
    while (true) {
        if (s_abort_requested) {
            ESP_LOGI(TAG, "Live interrotto per registrazione");
            s_abort_requested = false;
            break;
        }

        esp_capture_err_t err = esp_capture_sink_acquire_frame(s_sink, &frame, true);
        if (err != ESP_CAPTURE_ERR_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (frame.size <= 0 || !frame.data) {
            esp_capture_sink_release_frame(s_sink, &frame);
            continue;
        }

        if (sent_frames == 0) {
            ESP_LOGI(TAG, "Primo frame MJPEG: %d byte", frame.size);
        }

        int hlen = snprintf(part_hdr, sizeof(part_hdr),
                            "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n",
                            MJPEG_BOUNDARY, frame.size);

        if (httpd_resp_send_chunk(req, part_hdr, hlen) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)frame.data, frame.size) != ESP_OK ||
            httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
            ESP_LOGI(TAG, "Client disconnesso dopo %lu frame MJPEG", (unsigned long)sent_frames);
            esp_capture_sink_release_frame(s_sink, &frame);
            break;
        }

        esp_capture_sink_release_frame(s_sink, &frame);
        sent_frames++;
        if ((sent_frames % 30) == 0) {
            ESP_LOGI(TAG, "Live stream: %lu frame MJPEG inviati", (unsigned long)sent_frames);
        }
    }

    httpd_resp_send_chunk(req, nullptr, 0);
    core_live_stop();
    return ESP_OK;
}

static void live_stream_worker(void *) {
    ESP_LOGI(TAG, "Worker live stream avviato");
    while (true) {
        xSemaphoreGive(s_worker_ready);

        live_stream_job_t job;
        if (xQueueReceive(s_stream_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Live stream async — connessione client");
        s_job_active = true;
        core_live_stream_impl(job.req);
        s_job_active = false;

        if (httpd_req_async_handler_complete(job.req) != ESP_OK) {
            ESP_LOGW(TAG, "httpd_req_async_handler_complete fallita");
        }
    }
}

bool core_live_async_init(void) {
    if (s_worker_task) {
        return true;
    }

    s_worker_ready = xSemaphoreCreateCounting(1, 0);
    if (!s_worker_ready) {
        return false;
    }

    s_stream_queue = xQueueCreate(1, sizeof(live_stream_job_t));
    if (!s_stream_queue) {
        vSemaphoreDelete(s_worker_ready);
        s_worker_ready = nullptr;
        return false;
    }

    if (xTaskCreate(live_stream_worker, "live_stream", LIVE_STREAM_WORKER_STACK, nullptr,
                    LIVE_STREAM_WORKER_PRIO, &s_worker_task) != pdPASS) {
        vQueueDelete(s_stream_queue);
        vSemaphoreDelete(s_worker_ready);
        s_stream_queue = nullptr;
        s_worker_ready = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "Async live stream pronto");
    return true;
}

esp_err_t core_live_stream(httpd_req_t *req) {
    if (!core_live_async_init()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Live worker non disponibile");
        return ESP_FAIL;
    }

    httpd_req_t *async_req = nullptr;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Async live fallito");
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_worker_ready, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Live stream già attivo");
        httpd_req_async_handler_complete(async_req);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Live stream busy", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    live_stream_job_t job = { .req = async_req };
    if (xQueueSend(s_stream_queue, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreGive(s_worker_ready);
        httpd_req_async_handler_complete(async_req);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Coda live piena");
        return ESP_FAIL;
    }

    return ESP_OK;
}

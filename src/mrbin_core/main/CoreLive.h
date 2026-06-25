#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

bool core_live_start(void);
void core_live_stop(void);
void core_live_request_stop(void);
bool core_live_is_running(void);
bool core_live_async_init(void);
esp_err_t core_live_stream(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

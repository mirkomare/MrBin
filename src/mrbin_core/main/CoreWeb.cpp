#include "CoreWeb.h"
#include "CoreAuth.h"
#include "CoreConfig.h"
#include "CoreCrypto.h"
#include "CoreSD.h"

#include "dirent.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "core_web";
static httpd_handle_t s_server = nullptr;
static core_settings_t *s_settings = nullptr;

static bool is_authed(httpd_req_t *req) {
    char cookie[128];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }
    char token[80] = {0};
    const char *p = strstr(cookie, "MRBIN_SESS=");
    if (!p) return false;
    p += 11;
    size_t i = 0;
    while (*p && *p != ';' && i < sizeof(token) - 1) token[i++] = *p++;
    return core_auth_validate_session_token(token);
}

static esp_err_t send_redirect(httpd_req_t *req, const char *loc) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, nullptr, 0);
}

static const char *LOGIN_HTML = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>MrBin CORE</title>
<style>body{font-family:sans-serif;max-width:420px;margin:40px auto}input{display:block;width:100%;margin:8px 0;padding:8px}button{padding:10px 16px}</style>
</head><body><h2>MrBin CORE</h2>
<form method="POST" action="/login"><label>User</label><input name="user" required>
<label>Password</label><input name="pass" type="password" required>
<button type="submit">Login</button></form></body></html>
)HTML";

static const char *SETTINGS_HTML_HEAD = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Impostazioni CORE</title>
<style>body{font-family:sans-serif;max-width:640px;margin:24px auto}input,button{padding:8px;margin:4px 0;width:100%}
nav a{margin-right:12px}</style></head><body>
<nav><a href="/settings">Impostazioni</a><a href="/videos">Video</a><a href="/logout">Logout</a></nav>
<h2>Impostazioni CORE</h2>
)HTML";

static const char *VIDEOS_HTML_HEAD = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Video CORE</title>
<style>body{font-family:sans-serif;max-width:720px;margin:24px auto}nav a{margin-right:12px}
ul{line-height:1.8}</style></head><body>
<nav><a href="/settings">Impostazioni</a><a href="/videos">Video</a><a href="/logout">Logout</a></nav>
<h2>Registrazioni SD</h2><ul>
)HTML";

static esp_err_t handle_root(httpd_req_t *req) {
    if (is_authed(req)) return send_redirect(req, "/settings");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, LOGIN_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_login(httpd_req_t *req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = 0;

    char user[32] = {0}, pass[64] = {0};
    char *u = strstr(buf, "user=");
    char *p = strstr(buf, "pass=");
    if (u) sscanf(u, "user=%31[^&]", user);
    if (p) sscanf(p, "pass=%63s", pass);

    if (!core_auth_check_credentials(user, pass)) {
        return send_redirect(req, "/?err=1");
    }
    char token[65];
    core_auth_create_session_token(token, sizeof(token));
    char cookie[96];
    snprintf(cookie, sizeof(cookie), "MRBIN_SESS=%s; Path=/; HttpOnly", token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return send_redirect(req, "/settings");
}

static esp_err_t handle_logout(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Set-Cookie", "MRBIN_SESS=; Path=/; Max-Age=0");
    return send_redirect(req, "/");
}

static void key_to_hex(const uint8_t *key, char *out, size_t out_len) {
    for (int i = 0; i < 16 && (size_t)(i * 2 + 2) < out_len; ++i) {
        snprintf(out + i * 2, 3, "%02x", key[i]);
    }
}

static esp_err_t handle_settings_get(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");
    char keyhex[40] = {0};
    if (s_settings && s_settings->aes_key_valid) {
        key_to_hex(s_settings->aes_key, keyhex, sizeof(keyhex));
    }
    char body[2048];
    snprintf(body, sizeof(body),
        "%s"
        "<form method='POST' action='/settings'>"
        "<label>CORE ID (5 cifre)</label><input name='core_id' value='%05lu'>"
        "<label>Delay dopo D2 (ms)</label><input name='d2_delay' value='%lu'>"
        "<label>WiFi SSID</label><input name='wifi_ssid' value='%s'>"
        "<label>WiFi Password</label><input name='wifi_pass' type='password' value='%s'>"
        "<label>Chiave AES-128 (hex, sola lettura)</label><input readonly value='%s'>"
        "<button type='submit'>Salva</button></form>"
        "<form method='POST' action='/format_sd' onsubmit='return confirm(\"Formattare SD?\");'>"
        "<button type='submit' style='background:#c33;color:#fff'>Formatta SD</button></form>"
        "</body></html>",
        SETTINGS_HTML_HEAD,
        s_settings ? (unsigned long)s_settings->core_id : 0UL,
        s_settings ? (unsigned long)s_settings->d2_post_delay_ms : (unsigned long)CORE_D2_POST_DELAY_MS,
        s_settings ? s_settings->wifi_ssid : "",
        s_settings ? s_settings->wifi_pass : "",
        keyhex);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_settings_post(httpd_req_t *req) {
    if (!is_authed(req) || !s_settings) return send_redirect(req, "/");
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = 0;

    char id_s[16] = {0}, delay_s[16] = {0}, ssid[33] = {0}, pass[65] = {0};
    char *f;
    if ((f = strstr(buf, "core_id="))) sscanf(f, "core_id=%15[^&]", id_s);
    if ((f = strstr(buf, "d2_delay="))) sscanf(f, "d2_delay=%15[^&]", delay_s);
    if ((f = strstr(buf, "wifi_ssid="))) sscanf(f, "wifi_ssid=%32[^&]", ssid);
    if ((f = strstr(buf, "wifi_pass="))) sscanf(f, "wifi_pass=%64s", pass);

    uint32_t id = (uint32_t)atoi(id_s);
    if (id >= CORE_ID_MIN && id <= CORE_ID_MAX) s_settings->core_id = id;
    uint32_t delay = (uint32_t)atoi(delay_s);
    if (delay >= 1000 && delay <= 600000) s_settings->d2_post_delay_ms = delay;
    strncpy(s_settings->wifi_ssid, ssid, sizeof(s_settings->wifi_ssid) - 1);
    strncpy(s_settings->wifi_pass, pass, sizeof(s_settings->wifi_pass) - 1);
    core_settings_save(s_settings);
    return send_redirect(req, "/settings");
}

static esp_err_t handle_format_sd(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");
    core_sd_format();
    return send_redirect(req, "/settings");
}

static esp_err_t handle_videos_get(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");
    char body[8192];
    size_t off = 0;
    off += snprintf(body + off, sizeof(body) - off, "%s", VIDEOS_HTML_HEAD);

    DIR *root = opendir(CORE_SD_MOUNT_POINT);
    if (root) {
        struct dirent *day;
        while ((day = readdir(root)) != nullptr && off < sizeof(body) - 200) {
            if (day->d_type != DT_DIR) continue;
            if (strcmp(day->d_name, ".") == 0 || strcmp(day->d_name, "..") == 0) continue;
            char daypath[96];
            snprintf(daypath, sizeof(daypath), "%s/%s", CORE_SD_MOUNT_POINT, day->d_name);
            DIR *subdir = opendir(daypath);
            if (!subdir) continue;
            struct dirent *f;
            while ((f = readdir(subdir)) != nullptr && off < sizeof(body) - 200) {
                if (f->d_type != DT_REG) continue;
                off += snprintf(body + off, sizeof(body) - off,
                    "<li><a href='/play?f=%s/%s'>%s/%s</a></li>",
                    day->d_name, f->d_name, day->d_name, f->d_name);
            }
            closedir(subdir);
        }
        closedir(root);
    }
    off += snprintf(body + off, sizeof(body) - off, "</ul></body></html>");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_play(httpd_req_t *req) {
    if (!is_authed(req) || !s_settings || !s_settings->aes_key_valid) {
        return send_redirect(req, "/");
    }
    char query[160] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return ESP_FAIL;
    }
    char rel[128] = {0};
    if (httpd_query_key_value(query, "f", rel, sizeof(rel)) != ESP_OK) {
        return ESP_FAIL;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", CORE_SD_MOUNT_POINT, rel);

    FILE *fp = fopen(path, "rb");
    if (!fp) return ESP_FAIL;

    uint32_t magic = 0;
    if (fread(&magic, 1, sizeof(magic), fp) != sizeof(magic) || magic != CORE_CRYPTO_MAGIC) {
        fclose(fp);
        return ESP_FAIL;
    }
    uint8_t iv[16];
    if (fread(iv, 1, sizeof(iv), fp) != sizeof(iv)) {
        fclose(fp);
        return ESP_FAIL;
    }

    core_crypto_ctx_t ctx;
    core_crypto_init_ctx(&ctx, s_settings->aes_key, iv);

    httpd_resp_set_type(req, "video/mp4");
    httpd_resp_set_hdr(req, "Accept-Ranges", "none");

    uint8_t buf[4096];
    uint64_t offset = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        core_crypto_crypt_buffer(&ctx, buf, n, offset);
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) {
            fclose(fp);
            return ESP_FAIL;
        }
        offset += n;
    }
    fclose(fp);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

static bool wifi_start_sta(const core_settings_t *s) {
    if (!s || s->wifi_ssid[0] == 0) return false;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t wcfg = {};
    strncpy((char *)wcfg.sta.ssid, s->wifi_ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, s->wifi_pass, sizeof(wcfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "WiFi STA avviato: %s", s->wifi_ssid);
    return true;
}

bool core_web_start(core_settings_t *settings) {
    s_settings = settings;
    if (!core_sd_init()) {
        ESP_LOGW(TAG, "SD non montata — pagina video limitata");
    }
    wifi_start_sta(settings);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CORE_WEB_PORT;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start fallito");
        return false;
    }

    httpd_uri_t uris[] = {
        {"/", HTTP_GET, handle_root, nullptr},
        {"/login", HTTP_POST, handle_login, nullptr},
        {"/logout", HTTP_GET, handle_logout, nullptr},
        {"/settings", HTTP_GET, handle_settings_get, nullptr},
        {"/settings", HTTP_POST, handle_settings_post, nullptr},
        {"/format_sd", HTTP_POST, handle_format_sd, nullptr},
        {"/videos", HTTP_GET, handle_videos_get, nullptr},
        {"/play", HTTP_GET, handle_play, nullptr},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "Web GUI attiva su porta %d", CORE_WEB_PORT);
    return true;
}

void core_web_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
}

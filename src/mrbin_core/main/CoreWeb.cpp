#include "CoreWeb.h"
#include "CoreAuth.h"
#include "CoreConfig.h"
#include "CoreCrypto.h"
#include "CoreLive.h"
#include "CoreSD.h"
#include "CoreVideo.h"

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
<style>
body{font-family:sans-serif;max-width:640px;margin:24px auto}
input,button{padding:8px;margin:4px 0;width:100%}
nav a{margin-right:12px}
.sd-info{padding:10px 12px;margin:12px 0;border-radius:6px;background:#eef3ff;border:1px solid #c5d4f5}
.sd-info.warn{background:#fff8e6;border-color:#f0d080}
.alert{padding:12px 14px;margin:12px 0;border-radius:6px}
.alert.ok{background:#e8f7ea;border:1px solid #8fd19a;color:#1b5e20}
.alert.err{background:#fdecea;border:1px solid #f5a8a0;color:#8a1f11}
.overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.45);z-index:1000;align-items:center;justify-content:center}
.overlay.show{display:flex}
.overlay-box{background:#fff;padding:28px 32px;border-radius:10px;text-align:center;max-width:320px;box-shadow:0 8px 32px rgba(0,0,0,.2)}
.spinner{width:42px;height:42px;margin:0 auto 16px;border:4px solid #ddd;border-top-color:#c33;border-radius:50%;animation:spin .9s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
.fmt-btn{background:#c33;color:#fff}
.fmt-btn:disabled{opacity:.6;cursor:wait}
</style></head><body>
<nav><a href="/settings">Impostazioni</a><a href="/live">Live</a><a href="/videos">Video</a><a href="/logout">Logout</a></nav>
<h2>Impostazioni CORE</h2>
)HTML";

static const char *FORMAT_OVERLAY_HTML = R"HTML(
<div id="fmtOverlay" class="overlay" aria-live="polite">
<div class="overlay-box"><div class="spinner"></div><strong>Formattazione SD in corso…</strong>
<p style="margin:8px 0 0;font-size:14px;color:#555">Non chiudere la pagina.<br>Operazione di solito 10–60 s.</p></div></div>
<script>
function startFormat(e){
  if(!confirm('Formattare SD? Tutti i video verranno eliminati.')){e.preventDefault();return false;}
  document.getElementById('fmtOverlay').classList.add('show');
  var b=document.getElementById('fmtBtn'); if(b){b.disabled=true;}
  return true;
}
</script>
)HTML";

static const char *FORMAT_RESULT_HEAD = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Formattazione SD</title>
<style>body{font-family:sans-serif;max-width:640px;margin:24px auto}
.alert{padding:14px 16px;margin:16px 0;border-radius:6px}
.alert.ok{background:#e8f7ea;border:1px solid #8fd19a;color:#1b5e20}
.alert.err{background:#fdecea;border:1px solid #f5a8a0;color:#8a1f11}
a.btn{display:inline-block;margin-top:12px;padding:10px 16px;background:#2266cc;color:#fff;text-decoration:none;border-radius:6px}
</style></head><body>
<h2>Formattazione SD</h2>
)HTML";

static const char *VIDEOS_HTML_HEAD = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Video CORE</title>
<style>body{font-family:sans-serif;max-width:720px;margin:24px auto}nav a{margin-right:12px}
ul{line-height:1.8}</style></head><body>
<nav><a href="/settings">Impostazioni</a><a href="/live">Live</a><a href="/videos">Video</a><a href="/logout">Logout</a></nav>
<h2>Registrazioni SD</h2><ul>
)HTML";

static const char *LIVE_HTML_HEAD = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Live CORE</title>
<style>
body{font-family:sans-serif;max-width:960px;margin:24px auto}
nav a{margin-right:12px}
.live-wrap{background:#111;border-radius:8px;padding:8px;text-align:center;min-height:240px;display:flex;align-items:center;justify-content:center}
.live-wrap img{max-width:100%;height:auto;border-radius:4px;background:#000}
.live-err{color:#fff;background:#5a1a1a;border:1px solid #a33;padding:20px;border-radius:6px;text-align:left;line-height:1.5}
.live-meta{color:#666;font-size:14px;margin-top:8px}
</style></head><body>
<nav><a href="/settings">Impostazioni</a><a href="/live">Live</a><a href="/videos">Video</a><a href="/logout">Logout</a></nav>
<h2>Live camera</h2>
)HTML";

static const char *LIVE_PAGE_OK = R"HTML(
<div class='live-wrap'><img src='/live/stream' alt='Live camera'></div>
<p class='live-meta'>Live MJPEG hardware %dx%d @ %d fps (anteprima browser)</p>
<p class='live-meta'>Registrazione SD: H264 hardware %dx%d @ %d fps</p>
</body></html>
)HTML";

static esp_err_t handle_live_get(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");

    bool cam_ok = core_video_init();
    char body[2048];
    if (cam_ok) {
        char tail[256];
        snprintf(tail, sizeof(tail), LIVE_PAGE_OK,
                 CORE_LIVE_WIDTH, CORE_LIVE_HEIGHT, CORE_LIVE_FPS,
                 CORE_VIDEO_WIDTH, CORE_VIDEO_HEIGHT, CORE_VIDEO_FPS);
        snprintf(body, sizeof(body), "%s%s", LIVE_HTML_HEAD, tail);
    } else {
        snprintf(body, sizeof(body),
            "%s"
            "<div class='live-wrap'><div class='live-err'>"
            "<strong>Camera non disponibile</strong><br><br>"
            "Il sensore OV5647 non risponde su I2C (GPIO%d SDA, GPIO%d SCL).<br>"
            "Verificare:<ul>"
            "<li>Modulo camera collegato al connettore MIPI-CSI</li>"
            "<li>Cavo FPC ben inserito</li>"
            "<li>Camera compatibile OV5647</li>"
            "</ul></div></div>"
            "</body></html>",
            LIVE_HTML_HEAD, CORE_CSI_I2C_SDA, CORE_CSI_I2C_SCL);
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_live_stream(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");
    return core_live_stream(req);
}

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

static void format_bytes_human(uint64_t bytes, char *out, size_t out_len) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(out, out_len, "%.0f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_len, "%llu B", (unsigned long long)bytes);
    }
}

static esp_err_t handle_settings_get(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");
    char keyhex[40] = {0};
    if (s_settings && s_settings->aes_key_valid) {
        key_to_hex(s_settings->aes_key, keyhex, sizeof(keyhex));
    }

    char sd_info[160];
    if (core_sd_is_mounted()) {
        char free_h[32];
        format_bytes_human(core_sd_free_bytes(), free_h, sizeof(free_h));
        snprintf(sd_info, sizeof(sd_info),
            "<div class='sd-info'>SD montata — spazio libero: <strong>%s</strong></div>", free_h);
    } else {
        snprintf(sd_info, sizeof(sd_info),
            "<div class='sd-info warn'>SD non montata — inserire la scheda prima di formattare.</div>");
    }

    char flash[192] = {0};
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char msg[8] = {0};
        if (httpd_query_key_value(query, "sd", msg, sizeof(msg)) == ESP_OK) {
            if (strcmp(msg, "ok") == 0) {
                snprintf(flash, sizeof(flash),
                    "<div class='alert ok'>SD formattata con successo.</div>");
            } else if (strcmp(msg, "err") == 0) {
                snprintf(flash, sizeof(flash),
                    "<div class='alert err'>Formattazione SD fallita. Verificare la scheda e riprovare.</div>");
            }
        }
    }

    char body[4096];
    snprintf(body, sizeof(body),
        "%s"
        "%s"
        "%s"
        "<form method='POST' action='/settings'>"
        "<label>CORE ID (5 cifre)</label><input name='core_id' value='%05lu'>"
        "<label>Delay dopo D2 (ms)</label><input name='d2_delay' value='%lu'>"
        "<label>WiFi SSID</label><input name='wifi_ssid' value='%s'>"
        "<label>WiFi Password</label><input name='wifi_pass' type='password' value='%s'>"
        "<label>Chiave AES-128 (hex, sola lettura)</label><input readonly value='%s'>"
        "<button type='submit'>Salva</button></form>"
        "<form method='POST' action='/format_sd' onsubmit='return startFormat(event);'>"
        "<button id='fmtBtn' type='submit' class='fmt-btn'%s>Formatta SD</button></form>"
        "%s"
        "</body></html>",
        SETTINGS_HTML_HEAD,
        flash,
        sd_info,
        s_settings ? (unsigned long)s_settings->core_id : 0UL,
        s_settings ? (unsigned long)s_settings->d2_post_delay_ms : (unsigned long)CORE_D2_POST_DELAY_MS,
        s_settings ? s_settings->wifi_ssid : "",
        s_settings ? s_settings->wifi_pass : "",
        keyhex,
        core_sd_is_mounted() ? "" : " disabled",
        FORMAT_OVERLAY_HTML);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_settings_post(httpd_req_t *req) {
    if (!is_authed(req) || !s_settings) return send_redirect(req, "/");
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = 0;

    char id_s[16] = {0}, delay_s[16] = {0}, ssid[32] = {0}, pass[64] = {0};
    char *f;
    if ((f = strstr(buf, "core_id="))) sscanf(f, "core_id=%15[^&]", id_s);
    if ((f = strstr(buf, "d2_delay="))) sscanf(f, "d2_delay=%15[^&]", delay_s);
    if ((f = strstr(buf, "wifi_ssid="))) sscanf(f, "wifi_ssid=%31[^&]", ssid);
    if ((f = strstr(buf, "wifi_pass="))) sscanf(f, "wifi_pass=%63s", pass);

    uint32_t id = (uint32_t)atoi(id_s);
    if (id >= CORE_ID_MIN && id <= CORE_ID_MAX) s_settings->core_id = id;
    uint32_t delay = (uint32_t)atoi(delay_s);
    if (delay >= 1000 && delay <= 600000) s_settings->d2_post_delay_ms = delay;
    snprintf(s_settings->wifi_ssid, sizeof(s_settings->wifi_ssid), "%s", ssid);
    snprintf(s_settings->wifi_pass, sizeof(s_settings->wifi_pass), "%s", pass);
    core_settings_save(s_settings);
    return send_redirect(req, "/settings");
}

static esp_err_t handle_format_sd(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");

    ESP_LOGI(TAG, "Formattazione SD richiesta da web GUI");
    bool ok = core_sd_format();
    if (ok) {
        ESP_LOGI(TAG, "Formattazione SD completata");
    } else {
        ESP_LOGE(TAG, "Formattazione SD fallita");
    }

    char body[1024];
    if (ok) {
        char detail[80] = {0};
        if (core_sd_is_mounted()) {
            char free_h[32];
            format_bytes_human(core_sd_free_bytes(), free_h, sizeof(free_h));
            snprintf(detail, sizeof(detail), "<br>Spazio libero: <strong>%s</strong>", free_h);
        }
        snprintf(body, sizeof(body),
            "%s"
            "<div class='alert ok'><strong>Formattazione completata.</strong>%s</div>"
            "<p>La scheda SD è pronta per nuove registrazioni.</p>"
            "<a class='btn' href='/settings?sd=ok'>Torna alle impostazioni</a>"
            "<meta http-equiv='refresh' content='6;url=/settings?sd=ok'>"
            "</body></html>",
            FORMAT_RESULT_HEAD, detail);
    } else {
        snprintf(body, sizeof(body),
            "%s"
            "<div class='alert err'><strong>Formattazione fallita.</strong>"
            "<br>Verificare che la SD sia inserita e montata, poi riprovare.</div>"
            "<a class='btn' href='/settings?sd=err'>Torna alle impostazioni</a>"
            "</body></html>",
            FORMAT_RESULT_HEAD);
    }

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
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
            if (day->d_type != DT_DIR && day->d_type != DT_UNKNOWN) {
                continue;
            }
            if (strcmp(day->d_name, ".") == 0 || strcmp(day->d_name, "..") == 0) {
                continue;
            }
            char daypath[48];
            snprintf(daypath, sizeof(daypath), "%s/%.8s", CORE_SD_MOUNT_POINT, day->d_name);
            struct stat day_st;
            if (stat(daypath, &day_st) != 0 || !S_ISDIR(day_st.st_mode)) {
                continue;
            }
            DIR *subdir = opendir(daypath);
            if (!subdir) continue;
            struct dirent *f;
            while ((f = readdir(subdir)) != nullptr && off < sizeof(body) - 200) {
                if (f->d_type != DT_REG && f->d_type != DT_UNKNOWN) {
                    continue;
                }
                if (strstr(f->d_name, ".tmp")) {
                    continue;
                }
                char fpath[128];
                snprintf(fpath, sizeof(fpath), "%s/%.64s", daypath, f->d_name);
                struct stat fst;
                if (stat(fpath, &fst) != 0 || !S_ISREG(fst.st_mode)) {
                    continue;
                }
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

static bool s_wifi_driver_started = false;
static bool s_wifi_radio_started = false;

static esp_err_t init_once(esp_err_t err, const char *what) {
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) return ESP_OK;
    ESP_LOGE(TAG, "%s fallito: %s", what, esp_err_to_name(err));
    return err;
}

static bool ensure_network_stack(void) {
    return init_once(esp_netif_init(), "esp_netif_init") == ESP_OK &&
           init_once(esp_event_loop_create_default(), "esp_event_loop_create_default") == ESP_OK;
}

static bool wifi_init_driver(void) {
    if (s_wifi_driver_started) return true;
    if (!ensure_network_stack()) return false;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (init_once(esp_wifi_init(&cfg), "esp_wifi_init") != ESP_OK) return false;
    s_wifi_driver_started = true;
    return true;
}

static bool wifi_start_sta(const core_settings_t *s) {
    if (!s || s->wifi_ssid[0] == 0) return false;
    if (s_wifi_radio_started) return true;
    if (!wifi_init_driver()) return false;

    esp_netif_create_default_wifi_sta();

    if (init_once(esp_wifi_set_mode(WIFI_MODE_STA), "esp_wifi_set_mode") != ESP_OK) return false;
    wifi_config_t wcfg = {};
    snprintf((char *)wcfg.sta.ssid, sizeof(wcfg.sta.ssid), "%s", s->wifi_ssid);
    snprintf((char *)wcfg.sta.password, sizeof(wcfg.sta.password), "%s", s->wifi_pass);
    if (init_once(esp_wifi_set_config(WIFI_IF_STA, &wcfg), "esp_wifi_set_config") != ESP_OK) {
        return false;
    }
    if (init_once(esp_wifi_start(), "esp_wifi_start") != ESP_OK) return false;
    if (init_once(esp_wifi_connect(), "esp_wifi_connect") != ESP_OK) return false;
    s_wifi_radio_started = true;
    ESP_LOGI(TAG, "WiFi STA avviato: %s", s->wifi_ssid);
    return true;
}

static bool wifi_start_ap(const core_settings_t *s) {
    if (!s || s_wifi_radio_started) return s_wifi_radio_started;
    if (!wifi_init_driver()) return false;

    esp_netif_create_default_wifi_ap();

    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "MrBin-%05lu", (unsigned long)s->core_id);

    wifi_config_t wcfg = {};
    snprintf((char *)wcfg.ap.ssid, sizeof(wcfg.ap.ssid), "%s", ap_ssid);
    wcfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    wcfg.ap.channel = 1;
    wcfg.ap.max_connection = 4;
    snprintf((char *)wcfg.ap.password, sizeof(wcfg.ap.password), "12345678");
    wcfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if (init_once(esp_wifi_set_mode(WIFI_MODE_AP), "esp_wifi_set_mode") != ESP_OK) return false;
    if (init_once(esp_wifi_set_config(WIFI_IF_AP, &wcfg), "esp_wifi_set_config") != ESP_OK) {
        return false;
    }
    if (init_once(esp_wifi_start(), "esp_wifi_start") != ESP_OK) return false;
    s_wifi_radio_started = true;
    ESP_LOGI(TAG, "WiFi AP avviato: %s (pass 12345678)", ap_ssid);
    return true;
}

static bool wifi_start(const core_settings_t *s) {
    if (s && s->wifi_ssid[0] != 0) {
        return wifi_start_sta(s);
    }
    return wifi_start_ap(s);
}

bool core_web_start(core_settings_t *settings) {
    s_settings = settings;
    if (!ensure_network_stack()) {
        ESP_LOGE(TAG, "Stack rete non inizializzato");
        return false;
    }
    if (!wifi_start(settings)) {
        ESP_LOGW(TAG, "WiFi non avviato");
    }
    if (!core_sd_is_mounted() && !core_sd_init()) {
        ESP_LOGW(TAG, "SD non montata — pagina video limitata");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CORE_WEB_PORT;
    config.stack_size = 32768;
    config.max_uri_handlers = 13;
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
        {"/live", HTTP_GET, handle_live_get, nullptr},
        {"/live/stream", HTTP_GET, handle_live_stream, nullptr},
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

#include "CoreWeb.h"
#include "CoreAuth.h"
#include "CoreConfig.h"
#include "CoreCrypto.h"
#include "CoreLive.h"
#include "CoreRecorder.h"
#include "CoreSD.h"
#include "CoreStatusLed.h"
#include "CoreVideo.h"

#include "dirent.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "core_web";

#define CORE_CRYPTO_FILE_HEADER  20   // MRBI (4) + IV (16)
static httpd_handle_t s_server = nullptr;
static core_settings_t *s_settings = nullptr;
static bool s_boot_error_page = false;
static core_gpio_boot_snapshot_t s_boot_error_snap = {};
static bool s_wifi_events_registered = false;

static const char *gpio_level_label(int level) {
    return level ? "HIGH" : "LOW";
}

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
<style>
body{font-family:sans-serif;max-width:820px;margin:24px auto}
nav a{margin-right:12px}
ul{list-style:none;padding:0;margin:0}
.vid-row{display:flex;align-items:center;gap:12px;padding:10px 0;border-bottom:1px solid #e8e8e8;flex-wrap:wrap}
.vid-title{flex:1;min-width:160px;text-decoration:none;color:#2266cc;font-weight:500}
.vid-size{color:#666;font-size:14px;min-width:72px;text-align:right;flex-shrink:0}
.vid-actions{display:flex;align-items:center;gap:8px;flex-shrink:0}
.vid-btn{padding:6px 14px;border:none;border-radius:4px;cursor:pointer;font-size:13px;font-weight:600;
  text-decoration:none;display:inline-block;white-space:nowrap;line-height:1.35;font-family:sans-serif}
.vid-btn-dl{background:#2266cc;color:#fff !important}
.vid-btn-dl:hover{background:#1a52a3;color:#fff !important}
.vid-btn-del{background:#c33;color:#fff}
.vid-btn-del:hover{background:#a22}
.alert{padding:12px 14px;margin:12px 0;border-radius:6px}
.alert.ok{background:#e8f7ea;border:1px solid #8fd19a;color:#1b5e20}
.alert.err{background:#fdecea;border:1px solid #f5a8a0;color:#8a1f11}
.vid-empty{color:#666;padding:16px 0}
.vid-pending{color:#888;font-style:italic}
.sd-warn{padding:12px 14px;margin:12px 0;border-radius:6px;background:#fff8e6;border:1px solid #f0d080;color:#664400}
</style></head><body>
<nav><a href="/settings">Impostazioni</a><a href="/live">Live</a><a href="/videos">Video</a><a href="/logout">Logout</a></nav>
<h2>Registrazioni SD</h2>
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
.rec-bar{display:flex;align-items:center;gap:12px;margin:12px 0;flex-wrap:wrap}
.rec-bar button{padding:10px 18px;font-size:15px;border:none;border-radius:6px;cursor:pointer}
#btnRecStart{background:#c33;color:#fff}
#btnRecStop{background:#444;color:#fff}
#btnRecStart:disabled,#btnRecStop:disabled{opacity:.45;cursor:not-allowed}
.rec-status{font-size:14px;color:#333;min-height:1.4em}
.rec-status.rec-on{color:#c33;font-weight:bold}
.rec-overlay{color:#ccc;font-size:18px;padding:40px}
</style></head><body>
<nav><a href="/settings">Impostazioni</a><a href="/live">Live</a><a href="/videos">Video</a><a href="/logout">Logout</a></nav>
<h2>Live camera</h2>
)HTML";

static const char *LIVE_PAGE_OK = R"HTML(
<div class="rec-bar">
<button id="btnRecStart" type="button">&#9679; Registra</button>
<button id="btnRecStop" type="button" disabled>&#9632; Stop</button>
<span id="recStatus" class="rec-status"></span>
</div>
<div class="live-wrap" id="liveWrap">
<img id="liveImg" src="/live/stream" alt="Live camera">
</div>
<p class="live-meta">Live MJPEG hardware %dx%d @ %d fps (anteprima browser)</p>
<p class="live-meta">Registrazione SD: H264 hardware %dx%d @ %d fps — durante la registrazione il live viene sospeso</p>
<script>
async function recPost(path){
  try{
    const r=await fetch(path,{method:'POST',credentials:'same-origin'});
    const t=await r.text();
    try{return JSON.parse(t);}catch(_){return{ok:false,msg:t||('HTTP '+r.status)};}
  }catch(e){return{ok:false,msg:String(e)};}
}
async function recPoll(){
  try{
    const r=await fetch('/live/record/status',{credentials:'same-origin'});
    if(!r.ok)return;
    const j=await r.json();
    const st=document.getElementById('recStatus');
    const img=document.getElementById('liveImg');
    const wrap=document.getElementById('liveWrap');
    document.getElementById('btnRecStart').disabled=!!j.active;
    document.getElementById('btnRecStop').disabled=!j.active;
    if(j.active){
      st.textContent='Registrazione in corso… (LED acceso)';
      st.className='rec-status rec-on';
      if(img.getAttribute('src')){img.removeAttribute('src');}
      if(!wrap.querySelector('.rec-overlay')){
        const o=document.createElement('div');
        o.className='rec-overlay';
        o.textContent='Registrazione H264 su SD…';
        wrap.appendChild(o);
      }
    }else{
      st.className='rec-status';
      if(j.last_saved&&j.file){
        st.textContent='Salvato: '+j.file;
      }else if(j.error){
        st.textContent=j.error;
      }else{
        st.textContent='Pronto';
      }
      const o=wrap.querySelector('.rec-overlay');
      if(o) o.remove();
      if(!img.getAttribute('src')){
        img.src='/live/stream?ts='+Date.now();
      }
    }
  }catch(e){console.error('recPoll',e);}
}
document.getElementById('btnRecStart').onclick=async()=>{
  const img=document.getElementById('liveImg');
  if(img.getAttribute('src')){img.removeAttribute('src');}
  document.getElementById('recStatus').textContent='Avvio registrazione…';
  const j=await recPost('/live/record/start');
  if(!j.ok) alert(j.msg||'Avvio fallito');
  recPoll();
};
document.getElementById('btnRecStop').onclick=async()=>{
  document.getElementById('btnRecStop').disabled=true;
  document.getElementById('recStatus').textContent='Salvataggio e cifratura su SD…';
  const j=await recPost('/live/record/stop');
  if(j.ok) alert('Registrazione salvata su SD'+(j.file?'\n'+j.file:''));
  else alert(j.msg||'Stop fallito');
  recPoll();
};
setInterval(recPoll,2000);
recPoll();
</script>
</body></html>
)HTML";

static esp_err_t handle_live_stream(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");
    if (core_recorder_manual_is_active()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "Registrazione in corso", HTTPD_RESP_USE_STRLEN);
    }
    return core_live_stream(req);
}

static void recording_path_for_web(const char *full_path, char *out, size_t out_len) {
    if (!full_path || !out || out_len == 0) {
        return;
    }
    const char *prefix = CORE_SD_MOUNT_POINT "/";
    if (strncmp(full_path, prefix, strlen(prefix)) == 0) {
        snprintf(out, out_len, "%s", full_path + strlen(prefix));
    } else {
        snprintf(out, out_len, "%s", full_path);
    }
}

static esp_err_t send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_live_record_status(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");

    core_recorder_manual_status_t st;
    core_recorder_manual_get_status(&st);
    char rel[128] = {0};
    recording_path_for_web(st.last_path, rel, sizeof(rel));

    char json[384];
    snprintf(json, sizeof(json),
             "{\"active\":%s,\"last_saved\":%s,\"frames\":%lu,\"file\":\"%s\",\"error\":\"%s\"}",
             st.active ? "true" : "false",
             st.last_saved ? "true" : "false",
             (unsigned long)st.frame_count,
             rel,
             st.error);
    return send_json(req, json);
}

static esp_err_t handle_live_record_start(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");

    char json[256];
    if (core_recorder_manual_is_active()) {
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"Registrazione già in corso\"}");
        return send_json(req, json);
    }
    if (!s_settings) {
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"Impostazioni non disponibili\"}");
        return send_json(req, json);
    }
    if (!core_sd_is_mounted() && !core_sd_init()) {
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"SD non montata\"}");
        return send_json(req, json);
    }

    core_live_request_stop();
    for (int i = 0; i < 100 && core_live_is_running(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!core_recorder_manual_start(s_settings)) {
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"Avvio registrazione fallito\"}");
        return send_json(req, json);
    }

    ESP_LOGI(TAG, "Registrazione manuale avviata da Web GUI");
    snprintf(json, sizeof(json), "{\"ok\":true,\"msg\":\"Registrazione avviata\"}");
    return send_json(req, json);
}

static esp_err_t handle_live_record_stop(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");

    char json[384];
    if (!core_recorder_manual_is_active()) {
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"Nessuna registrazione attiva\"}");
        return send_json(req, json);
    }

    ESP_LOGI(TAG, "Stop registrazione manuale da Web GUI");
    core_recorder_manual_stop();

    core_recorder_manual_wait_done(120000);
    core_recorder_manual_status_t st;
    core_recorder_manual_get_status(&st);
    char rel[128] = {0};
    recording_path_for_web(st.last_path, rel, sizeof(rel));

    if (st.last_saved) {
        snprintf(json, sizeof(json),
                 "{\"ok\":true,\"msg\":\"Video registrato — cifratura in corso, aggiorna la pagina Video tra ~1 min\",\"file\":\"%s\"}",
                 rel);
    } else {
        snprintf(json, sizeof(json), "{\"ok\":false,\"msg\":\"%s\",\"file\":\"%s\"}",
                 st.error[0] ? st.error : "Salvataggio fallito", rel);
    }
    return send_json(req, json);
}

static esp_err_t handle_live_get(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");

    bool cam_ok = core_video_init();
    char body[8192];
    if (cam_ok) {
        char tail[6144];
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

static esp_err_t handle_root(httpd_req_t *req) {
    if (is_authed(req)) return send_redirect(req, "/settings");

    char body[768];
    if (s_boot_error_page) {
        snprintf(body, sizeof(body),
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>MrBin CORE</title>"
            "<style>body{font-family:sans-serif;max-width:520px;margin:40px auto}"
            "input{display:block;width:100%%;margin:8px 0;padding:8px}button{padding:10px 16px}"
            ".boot-err{color:#c00;font-weight:bold;font-size:1.05em;margin:0 0 20px;padding:12px;"
            "border:2px solid #c00;background:#fff0f0}</style></head><body><h2>MrBin CORE</h2>"
            "<p class=\"boot-err\">ERRORE BOOT PIN non riconosciuto, D1=%s D2=%s MODE=%s</p>"
            "<form method=\"POST\" action=\"/login\"><label>User</label><input name=\"user\" required>"
            "<label>Password</label><input name=\"pass\" type=\"password\" required>"
            "<button type=\"submit\">Login</button></form></body></html>",
            gpio_level_label(s_boot_error_snap.d1_level),
            gpio_level_label(s_boot_error_snap.d2_level),
            gpio_level_label(s_boot_error_snap.mode_level));
    } else {
        strncpy(body, LOGIN_HTML, sizeof(body) - 1);
        body[sizeof(body) - 1] = 0;
    }

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
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

static void url_decode(const char *in, char *out, size_t out_len) {
    if (!in || !out || out_len == 0) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_len; ++i) {
        if (in[i] == '%' && in[i + 1] && in[i + 2]) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out[o++] = (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if (in[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = 0;
}

static void url_encode_query(const char *in, char *out, size_t out_len) {
    if (!in || !out || out_len == 0) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < out_len; ++i) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '.' || c == '-') {
            out[o++] = c;
        } else if (c == '/') {
            out[o++] = '%';
            out[o++] = '2';
            out[o++] = 'F';
        } else {
            snprintf(out + o, out_len - o, "%%%02X", (unsigned char)c);
            o += 3;
        }
    }
    out[o] = 0;
}

static bool video_rel_path_valid(const char *rel);

static bool video_query_get_rel(httpd_req_t *req, char *rel, size_t rel_len) {
    char query[192] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char raw[128] = {0};
    if (httpd_query_key_value(query, "f", raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    url_decode(raw, rel, rel_len);
    return video_rel_path_valid(rel);
}

static bool video_rel_path_valid(const char *rel) {
    if (!rel || rel[0] == 0) {
        return false;
    }
    if (strstr(rel, "..") != nullptr) {
        return false;
    }
    const char *slash = strchr(rel, '/');
    if (!slash || slash == rel || slash[1] == 0) {
        return false;
    }
    size_t day_len = (size_t)(slash - rel);
    if (day_len != 8) {
        return false;
    }
    const char *name = slash + 1;
    size_t name_len = strlen(name);
    if (name_len < 5 || name_len > 64) {
        return false;
    }
    if (strstr(name, ".tmp") != nullptr) {
        return false;
    }
    if (strcmp(name + name_len - 4, ".mp4") != 0) {
        return false;
    }
    for (const char *p = rel; *p; ++p) {
        char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            c == '_' || c == '.' || c == '/') {
            continue;
        }
        return false;
    }
    return true;
}

static void videos_append_flash(char *body, size_t *off, size_t max, httpd_req_t *req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return;
    }
    char val[16] = {0};
    if (httpd_query_key_value(query, "deleted", val, sizeof(val)) == ESP_OK) {
        *off += snprintf(body + *off, max - *off,
                           "<div class='alert ok'>File eliminato.</div>");
    }
    if (httpd_query_key_value(query, "err", val, sizeof(val)) == ESP_OK && strcmp(val, "del") == 0) {
        *off += snprintf(body + *off, max - *off,
                           "<div class='alert err'>Eliminazione fallita.</div>");
    }
}

static bool video_delete_rel(const char *rel) {
    if (!video_rel_path_valid(rel)) {
        return false;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", CORE_SD_MOUNT_POINT, rel);

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    if (remove(path) != 0) {
        ESP_LOGE(TAG, "remove %s fallito (errno=%d)", path, errno);
        return false;
    }
    ESP_LOGI(TAG, "Video eliminato: %s", path);

    char daypath[48];
    snprintf(daypath, sizeof(daypath), "%s/%.8s", CORE_SD_MOUNT_POINT, rel);
    DIR *day_dir = opendir(daypath);
    if (day_dir) {
        bool empty = true;
        struct dirent *ent;
        while ((ent = readdir(day_dir)) != nullptr) {
            if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                empty = false;
                break;
            }
        }
        closedir(day_dir);
        if (empty) {
            rmdir(daypath);
        }
    }
    return true;
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
    char body[12288];
    size_t off = 0;
    off += snprintf(body + off, sizeof(body) - off, "%s", VIDEOS_HTML_HEAD);
    videos_append_flash(body, &off, sizeof(body), req);

    if (!core_sd_is_mounted() && !core_sd_init()) {
        off += snprintf(body + off, sizeof(body) - off,
                        "<div class='sd-warn'>SD non montata — inserire la scheda e ricaricare.</div>");
    }

    off += snprintf(body + off, sizeof(body) - off, "<ul>");

    size_t file_count = 0;
    DIR *root = opendir(CORE_SD_MOUNT_POINT);
    if (!root) {
        ESP_LOGW(TAG, "opendir %s fallito (errno=%d)", CORE_SD_MOUNT_POINT, errno);
    } else {
        struct dirent *day;
        while ((day = readdir(root)) != nullptr && off < sizeof(body) - 480) {
            if (day->d_name[0] == '.' || day->d_name[0] == 0) {
                continue;
            }
            char daypath[64];
            snprintf(daypath, sizeof(daypath), "%s/%s", CORE_SD_MOUNT_POINT, day->d_name);
            struct stat day_st;
            if (stat(daypath, &day_st) != 0 || !S_ISDIR(day_st.st_mode)) {
                continue;
            }
            DIR *subdir = opendir(daypath);
            if (!subdir) {
                continue;
            }
            struct dirent *f;
            while ((f = readdir(subdir)) != nullptr && off < sizeof(body) - 480) {
                if (f->d_name[0] == '.' || f->d_name[0] == 0) {
                    continue;
                }
                bool is_tmp = (strstr(f->d_name, ".mp4.tmp") != nullptr);
                if (!is_tmp && strstr(f->d_name, ".mp4") == nullptr) {
                    continue;
                }
                char fpath[160];
                snprintf(fpath, sizeof(fpath), "%s/%s", daypath, f->d_name);
                struct stat fst;
                if (stat(fpath, &fst) != 0 || !S_ISREG(fst.st_mode)) {
                    continue;
                }
                char size_h[32];
                format_bytes_human((uint64_t)fst.st_size, size_h, sizeof(size_h));
                if (is_tmp) {
                    off += snprintf(body + off, sizeof(body) - off,
                        "<li class='vid-row vid-pending'>"
                        "<span class='vid-title'>%s/%s</span>"
                        "<span class='vid-size'>%s</span>"
                        "<span>Elaborazione…</span></li>",
                        day->d_name, f->d_name, size_h);
                    file_count++;
                    continue;
                }
                char rel[96];
                snprintf(rel, sizeof(rel), "%s/%s", day->d_name, f->d_name);
                char rel_enc[128];
                url_encode_query(rel, rel_enc, sizeof(rel_enc));
                off += snprintf(body + off, sizeof(body) - off,
                    "<li class='vid-row'>"
                    "<a class='vid-title' href='/videos/watch?f=%s'>%s/%s</a>"
                    "<span class='vid-size'>%s</span>"
                    "<div class='vid-actions'>"
                    "<a class='vid-btn vid-btn-dl' href='/videos/download?f=%s' "
                    "download=\"%s\">Scarica</a>"
                    "<form method='POST' action='/videos/delete' style='margin:0' "
                    "onsubmit=\"return confirm('Eliminare %s/%s?');\">"
                    "<input type='hidden' name='f' value='%s/%s'>"
                    "<button type='submit' class='vid-btn vid-btn-del'>Elimina</button>"
                    "</form></div></li>",
                    rel_enc, day->d_name, f->d_name, size_h,
                    rel_enc, f->d_name,
                    day->d_name, f->d_name, day->d_name, f->d_name);
                file_count++;
            }
            closedir(subdir);
        }
        closedir(root);
    }
    if (file_count == 0) {
        off += snprintf(body + off, sizeof(body) - off,
                        "<li class='vid-empty'>Nessuna registrazione trovata.</li>");
    }
    off += snprintf(body + off, sizeof(body) - off, "</ul></body></html>");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_videos_delete(httpd_req_t *req) {
    if (!is_authed(req)) return send_redirect(req, "/");

    char buf[256] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return send_redirect(req, "/videos?err=del");
    }
    buf[len] = 0;

    char raw[128] = {0};
    char *f = strstr(buf, "f=");
    if (!f || sscanf(f, "f=%127[^&]", raw) != 1) {
        return send_redirect(req, "/videos?err=del");
    }

    char rel[128] = {0};
    url_decode(raw, rel, sizeof(rel));
    if (!core_sd_is_mounted() && !core_sd_init()) {
        return send_redirect(req, "/videos?err=del");
    }
    if (!video_delete_rel(rel)) {
        return send_redirect(req, "/videos?err=del");
    }
    return send_redirect(req, "/videos?deleted=1");
}

static esp_err_t stream_decrypted_mp4(httpd_req_t *req, const char *rel, bool as_download) {
    if (!video_rel_path_valid(rel)) {
        return ESP_FAIL;
    }

    char path[192];
    snprintf(path, sizeof(path), "%s/%s", CORE_SD_MOUNT_POINT, rel);

    struct stat fst;
    if (stat(path, &fst) != 0 || fst.st_size <= (off_t)CORE_CRYPTO_FILE_HEADER) {
        return ESP_FAIL;
    }
    uint64_t plain_len = (uint64_t)fst.st_size - CORE_CRYPTO_FILE_HEADER;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return ESP_FAIL;
    }

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

    char *fbuf = (char *)heap_caps_malloc(CORE_SD_FILE_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (fbuf) {
        setvbuf(fp, fbuf, _IOFBF, CORE_SD_FILE_BUF_BYTES);
    }

    size_t io_buf_size = CORE_CRYPTO_STREAM_IO_BYTES;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (uint8_t *)heap_caps_malloc(io_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        io_buf_size = 8192;
        buf = (uint8_t *)malloc(io_buf_size);
    }
    if (!buf) {
        if (fbuf) {
            heap_caps_free(fbuf);
        }
        fclose(fp);
        return ESP_FAIL;
    }

    char cl[24];
    snprintf(cl, sizeof(cl), "%llu", (unsigned long long)plain_len);
    httpd_resp_set_hdr(req, "Content-Length", cl);

    if (as_download) {
        const char *slash = strrchr(rel, '/');
        const char *fname = slash ? slash + 1 : rel;
        char disp[160];
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    } else {
        httpd_resp_set_type(req, "video/mp4");
        httpd_resp_set_hdr(req, "Content-Disposition", "inline");
    }

    uint64_t offset = 0;
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, io_buf_size, fp)) > 0) {
        core_crypto_crypt_buffer(&ctx, buf, n, offset);
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        offset += n;
    }
    fclose(fp);
    if (fbuf) {
        heap_caps_free(fbuf);
    }
    heap_caps_free(buf);
    core_crypto_deinit_ctx(&ctx);
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t handle_videos_watch(httpd_req_t *req) {
    if (!is_authed(req)) {
        return send_redirect(req, "/");
    }
    char rel[128] = {0};
    if (!video_query_get_rel(req, rel, sizeof(rel))) {
        return ESP_FAIL;
    }

    const char *slash = strrchr(rel, '/');
    const char *fname = slash ? slash + 1 : rel;
    char rel_enc[128];
    url_encode_query(rel, rel_enc, sizeof(rel_enc));

    char body[1536];
    snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%s</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:960px;margin:24px auto}"
        "nav a{margin-right:12px}"
        "video{display:block;width:100%%;max-height:80vh;background:#000;border-radius:8px}"
        ".bar{margin:14px 0;display:flex;gap:10px;flex-wrap:wrap;align-items:center}"
        ".btn{padding:8px 16px;border-radius:4px;text-decoration:none;font-size:14px;"
        "font-weight:600;color:#fff !important;white-space:nowrap}"
        ".btn-dl{background:#2266cc}.btn-dl:hover{background:#1a52a3}"
        ".btn-back{background:#555}.btn-back:hover{background:#444}"
        ".hint{color:#666;font-size:14px;margin-top:8px}"
        "</style></head><body>"
        "<nav><a href=\"/videos\">Video</a><a href=\"/settings\">Impostazioni</a>"
        "<a href=\"/logout\">Logout</a></nav>"
        "<h2>%s</h2>"
        "<video controls playsinline preload=\"auto\" src=\"/play?f=%s\"></video>"
        "<div class=\"bar\">"
        "<a class=\"btn btn-dl\" href=\"/videos/download?f=%s\" download=\"%s\">Scarica MP4</a>"
        "<a class=\"btn btn-back\" href=\"/videos\">Torna all'elenco</a>"
        "</div>"
        "<p class=\"hint\">Se il video non parte, usa Scarica MP4 e aprilo con VLC.</p>"
        "</body></html>",
        fname, fname, rel_enc, rel_enc, fname);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_videos_download(httpd_req_t *req) {
    if (!is_authed(req) || !s_settings || !s_settings->aes_key_valid) {
        return send_redirect(req, "/");
    }
    char rel[128] = {0};
    if (!video_query_get_rel(req, rel, sizeof(rel))) {
        return ESP_FAIL;
    }
    return stream_decrypted_mp4(req, rel, true);
}

static esp_err_t handle_play(httpd_req_t *req) {
    if (!is_authed(req) || !s_settings || !s_settings->aes_key_valid) {
        return send_redirect(req, "/");
    }
    char rel[128] = {0};
    if (!video_query_get_rel(req, rel, sizeof(rel))) {
        return ESP_FAIL;
    }
    return stream_decrypted_mp4(req, rel, false);
}

static bool s_wifi_driver_started = false;
static bool s_wifi_radio_started = false;
static core_web_wifi_mode_t s_active_wifi_mode = CORE_WEB_WIFI_STA;

static void wifi_ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi STA connesso — lampeggio LED x3");
        core_status_led_notify_sta_connected();
    }
}

static esp_err_t init_once(esp_err_t err, const char *what) {
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) return ESP_OK;
    ESP_LOGE(TAG, "%s fallito: %s", what, esp_err_to_name(err));
    return err;
}

static bool ensure_network_stack(void) {
    if (init_once(esp_netif_init(), "esp_netif_init") != ESP_OK ||
        init_once(esp_event_loop_create_default(), "esp_event_loop_create_default") != ESP_OK) {
        return false;
    }
    if (!s_wifi_events_registered) {
        esp_err_t err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_ip_event_handler, nullptr, nullptr);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "registrazione IP_EVENT fallita: %s", esp_err_to_name(err));
            return false;
        }
        s_wifi_events_registered = true;
    }
    return true;
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
    if (s_wifi_radio_started && s_active_wifi_mode == CORE_WEB_WIFI_STA) return true;
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
    s_active_wifi_mode = CORE_WEB_WIFI_STA;
    ESP_LOGI(TAG, "WiFi STA avviato: %s", s->wifi_ssid);
    return true;
}

static bool wifi_start_ap(const core_settings_t *s) {
    if (!s) return false;
    if (s_wifi_radio_started && s_active_wifi_mode == CORE_WEB_WIFI_AP) return true;
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
    s_active_wifi_mode = CORE_WEB_WIFI_AP;
    ESP_LOGI(TAG, "WiFi AP avviato: %s (pass 12345678)", ap_ssid);
    return true;
}

static bool start_http_server(void) {
    if (s_server) return true;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CORE_WEB_PORT;
    config.stack_size = 32768;
    config.max_uri_handlers = 18;
    config.max_open_sockets = 7;
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
        {"/live/record/start", HTTP_POST, handle_live_record_start, nullptr},
        {"/live/record/stop", HTTP_POST, handle_live_record_stop, nullptr},
        {"/live/record/status", HTTP_GET, handle_live_record_status, nullptr},
        {"/videos", HTTP_GET, handle_videos_get, nullptr},
        {"/videos/watch", HTTP_GET, handle_videos_watch, nullptr},
        {"/videos/delete", HTTP_POST, handle_videos_delete, nullptr},
        {"/videos/download", HTTP_GET, handle_videos_download, nullptr},
        {"/play", HTTP_GET, handle_play, nullptr},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "Web GUI attiva su porta %d", CORE_WEB_PORT);
    return true;
}

bool core_web_start(core_settings_t *settings, core_web_wifi_mode_t wifi_mode,
                    const core_gpio_boot_snapshot_t *boot_error) {
    s_settings = settings;
    s_boot_error_page = false;

    if (wifi_mode == CORE_WEB_WIFI_AP_BOOT_ERROR) {
        if (!boot_error) return false;
        s_boot_error_page = true;
        s_boot_error_snap = *boot_error;
        core_status_led_set_mode(CORE_LED_ERROR);
        if (!wifi_start_ap(settings)) {
            ESP_LOGE(TAG, "AP errore boot non avviato");
            return false;
        }
    } else if (wifi_mode == CORE_WEB_WIFI_AP) {
        core_status_led_set_mode(CORE_LED_AP);
        if (!wifi_start_ap(settings)) {
            ESP_LOGE(TAG, "WiFi AP non avviato");
            return false;
        }
    } else {
        if (!settings || settings->wifi_ssid[0] == 0) {
            ESP_LOGI(TAG, "SSID vuoto — uso AP configurazione");
            core_status_led_set_mode(CORE_LED_AP);
            if (!wifi_start_ap(settings)) {
                return false;
            }
        } else if (!wifi_start_sta(settings)) {
            ESP_LOGW(TAG, "WiFi STA fallito — fallback AP");
            core_status_led_set_mode(CORE_LED_AP);
            if (!wifi_start_ap(settings)) {
                return false;
            }
        }
    }

    if (!core_sd_is_mounted() && !core_sd_init()) {
        ESP_LOGW(TAG, "SD non montata — pagina video limitata");
    }

    core_live_async_init();

    return start_http_server();
}

void core_web_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
    core_status_led_set_mode(CORE_LED_OFF);
    s_boot_error_page = false;
}

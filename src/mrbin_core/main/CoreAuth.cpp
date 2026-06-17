#include "CoreAuth.h"

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>

// SHA-256("mm:123456") — credenziali non in chiaro nello sketch
const uint8_t CORE_AUTH_SHA256[32] = {
    0x23, 0x7f, 0xf5, 0xbb, 0xd5, 0x84, 0x49, 0xe2,
    0x06, 0x69, 0x52, 0x95, 0x22, 0x38, 0x51, 0xca,
    0xbf, 0x66, 0x56, 0x43, 0x8e, 0x5d, 0x7f, 0x21,
    0xb7, 0x97, 0x5a, 0x56, 0x59, 0x62, 0x1b, 0x69,
};

static const char *TAG = "core_auth";
static char s_session_token[65] = {0};

static void sha256_concat(const char *user, const char *pass, uint8_t out[32]) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s:%s", user ? user : "", pass ? pass : "");
    mbedtls_sha256((const unsigned char *)buf, strlen(buf), out, 0);
}

bool core_auth_check_credentials(const char *user, const char *pass) {
    uint8_t digest[32];
    sha256_concat(user, pass, digest);
    bool ok = (memcmp(digest, CORE_AUTH_SHA256, 32) == 0);
    if (!ok) ESP_LOGW(TAG, "Login fallito per user=%s", user ? user : "(null)");
    return ok;
}

bool core_auth_create_session_token(char *out_token, size_t out_len) {
    if (!out_token || out_len < 65) return false;
    uint8_t rnd[32];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 32; ++i) {
        snprintf(out_token + i * 2, 3, "%02x", rnd[i]);
    }
    strncpy(s_session_token, out_token, sizeof(s_session_token) - 1);
    return true;
}

bool core_auth_validate_session_token(const char *token) {
    if (!token || s_session_token[0] == 0) return false;
    return strcmp(token, s_session_token) == 0;
}

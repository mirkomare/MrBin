#include "CoreAuth.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/private/sha256.h"
#else
#include "mbedtls/sha256.h"
#endif
#include <stdio.h>
#include <string.h>

// SHA-256("mm:GaPaMi") — credenziali non in chiaro nello sketch
const uint8_t CORE_AUTH_SHA256[32] = {
    0x13, 0x99, 0x9a, 0x1d, 0x11, 0xcd, 0x42, 0x22,
    0xed, 0x3c, 0xe9, 0xde, 0x3a, 0xf0, 0x84, 0x61,
    0xa1, 0x7c, 0x2b, 0x78, 0x00, 0xd2, 0x57, 0xeb,
    0xfe, 0x42, 0x74, 0x35, 0x83, 0x79, 0x67, 0x4d,
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

#include "CoreCrypto.h"
#include "CoreConfig.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/private/aes.h"
#else
#include "mbedtls/aes.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "core_crypto";

static void ctr_crypt(core_crypto_ctx_t *ctx, uint8_t *buf, size_t len, uint64_t stream_offset) {
    if (!ctx->aes_ready) {
        return;
    }

    uint8_t stream_block[16];
    uint8_t nonce_counter[16];
    memcpy(nonce_counter, ctx->iv, 16);

    uint64_t block_idx = stream_offset / 16;
    for (int i = 15; i >= 0; --i) {
        nonce_counter[i] = (uint8_t)(block_idx & 0xFF);
        block_idx >>= 8;
    }
    size_t nc_off = stream_offset % 16;

    mbedtls_aes_crypt_ctr(&ctx->aes, len, &nc_off, nonce_counter, stream_block, buf, buf);
}

bool core_crypto_init_ctx(core_crypto_ctx_t *ctx, const uint8_t key[16], const uint8_t iv[16]) {
    if (!ctx || !key || !iv) return false;
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->key, key, 16);
    memcpy(ctx->iv, iv, 16);
    mbedtls_aes_init(&ctx->aes);
    if (mbedtls_aes_setkey_enc(&ctx->aes, key, 128) != 0) {
        mbedtls_aes_free(&ctx->aes);
        return false;
    }
    ctx->aes_ready = true;
    ctx->block_offset = 0;
    ctx->initialized = true;
    return true;
}

void core_crypto_deinit_ctx(core_crypto_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->aes_ready) {
        mbedtls_aes_free(&ctx->aes);
        ctx->aes_ready = false;
    }
    ctx->initialized = false;
}

void core_crypto_reset_block_offset(core_crypto_ctx_t *ctx, uint64_t block_offset) {
    if (ctx) ctx->block_offset = block_offset;
}

bool core_crypto_crypt_buffer(core_crypto_ctx_t *ctx, uint8_t *buf, size_t len, uint64_t stream_offset) {
    if (!ctx || !ctx->initialized || !buf) return false;
    ctr_crypt(ctx, buf, len, stream_offset);
    return true;
}

bool core_crypto_write_encrypted_chunk(core_crypto_ctx_t *ctx, FILE *fp,
                                       const uint8_t *plain, size_t len, uint64_t stream_offset) {
    if (!ctx || !fp || !plain || len == 0) return false;
    uint8_t *tmp = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) {
        tmp = (uint8_t *)malloc(len);
    }
    if (!tmp) {
        return false;
    }
    memcpy(tmp, plain, len);
    core_crypto_crypt_buffer(ctx, tmp, len, stream_offset);
    size_t written = fwrite(tmp, 1, len, fp);
    heap_caps_free(tmp);
    return written == len;
}

bool core_crypto_read_decrypted_chunk(core_crypto_ctx_t *ctx, FILE *fp,
                                      uint8_t *out, size_t len, uint64_t stream_offset, size_t *out_len) {
    if (!ctx || !fp || !out) return false;
    size_t rd = fread(out, 1, len, fp);
    if (out_len) *out_len = rd;
    if (rd == 0) return false;
    return core_crypto_crypt_buffer(ctx, out, rd, stream_offset);
}

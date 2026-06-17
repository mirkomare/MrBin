#include "CoreCrypto.h"
#include "CoreConfig.h"

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/aes.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "core_crypto";

static void ctr_crypt(core_crypto_ctx_t *ctx, uint8_t *buf, size_t len, uint64_t stream_offset) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, ctx->key, 128);

    uint8_t stream_block[16];
    uint8_t nonce_counter[16];
    memcpy(nonce_counter, ctx->iv, 16);

    uint64_t block_idx = stream_offset / 16;
    for (int i = 15; i >= 0; --i) {
        nonce_counter[i] = (uint8_t)(block_idx & 0xFF);
        block_idx >>= 8;
    }
    size_t nc_off = stream_offset % 16;

    mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce_counter, stream_block, buf, buf);
    mbedtls_aes_free(&aes);
}

bool core_crypto_init_ctx(core_crypto_ctx_t *ctx, const uint8_t key[16], const uint8_t iv[16]) {
    if (!ctx || !key || !iv) return false;
    memcpy(ctx->key, key, 16);
    memcpy(ctx->iv, iv, 16);
    ctx->block_offset = 0;
    ctx->initialized = true;
    return true;
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
    uint8_t *tmp = (uint8_t *)malloc(len);
    if (!tmp) return false;
    memcpy(tmp, plain, len);
    core_crypto_crypt_buffer(ctx, tmp, len, stream_offset);
    size_t written = fwrite(tmp, 1, len, fp);
    free(tmp);
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

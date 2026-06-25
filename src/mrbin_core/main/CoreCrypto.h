#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "mbedtls/aes.h"

typedef struct {
    uint8_t iv[16];
    uint8_t key[16];
    uint64_t block_offset;
    bool initialized;
    bool aes_ready;
    mbedtls_aes_context aes;
} core_crypto_ctx_t;

bool core_crypto_init_ctx(core_crypto_ctx_t *ctx, const uint8_t key[16], const uint8_t iv[16]);
void core_crypto_deinit_ctx(core_crypto_ctx_t *ctx);
void core_crypto_reset_block_offset(core_crypto_ctx_t *ctx, uint64_t block_offset);

// Cifra/decifra in-place (AES-128-CTR via mbedTLS)
bool core_crypto_crypt_buffer(core_crypto_ctx_t *ctx, uint8_t *buf, size_t len, uint64_t stream_offset);

// Scrive header MRBI + cifra chunk
bool core_crypto_write_encrypted_chunk(core_crypto_ctx_t *ctx, FILE *fp,
                                       const uint8_t *plain, size_t len, uint64_t stream_offset);

// Legge e decifra chunk da file con header MRBI
bool core_crypto_read_decrypted_chunk(core_crypto_ctx_t *ctx, FILE *fp,
                                      uint8_t *out, size_t len, uint64_t stream_offset, size_t *out_len);

#ifdef __cplusplus
}
#endif

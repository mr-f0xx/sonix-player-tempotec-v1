#ifndef SYSTEM_CRYPTO_KDF_H
#define SYSTEM_CRYPTO_KDF_H

#include <stddef.h>
#include <stdint.h>

#include "src/system/core/sha256.h"

// HMAC-SHA256 (RFC 2104) and PBKDF2-HMAC-SHA256 (RFC 8018), which is how a
// PIN becomes the two keys the vault is written with.
//
// PBKDF2 is deliberately slow. A four-digit PIN has ten thousand answers, so
// the only thing between somebody holding the card and those answers is the
// cost of each guess; the vault stores an iteration count beside every file so
// that a later firmware can raise it without making today's files unreadable.
// The count is a number of HMAC calls, and each one is two SHA-256 passes.

#define HMAC_SHA256_LEN 32

typedef struct {
	sha256_ctx inner; // the state after the inner pad, ready for the message
	sha256_ctx outer; // and after the outer pad, ready for the inner digest
} hmac_sha256_ctx;

// `key` may be any length: shorter keys are padded with zeros, longer ones are
// hashed first, both as RFC 2104 says.
void hmac_sha256_init(hmac_sha256_ctx *ctx, const void *key, size_t key_len);
void hmac_sha256_update(hmac_sha256_ctx *ctx, const void *data, size_t len);
void hmac_sha256_final(hmac_sha256_ctx *ctx, uint8_t out[HMAC_SHA256_LEN]);

// The whole thing in one call, for a message that is already in memory.
void hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[HMAC_SHA256_LEN]);

// `out_len` bytes of PBKDF2 output. Any length is allowed; the vault asks for
// 64 bytes, one 32-byte AES key and one 32-byte HMAC key. `iterations` of zero
// is treated as one, as the RFC does.
void pbkdf2_hmac_sha256(const void *password, size_t password_len, const uint8_t *salt, size_t salt_len,
						uint32_t iterations, uint8_t *out, size_t out_len);

#endif /* SYSTEM_CRYPTO_KDF_H */

#include "kdf.h"

#include <string.h>

#define SHA256_BLOCK_LEN 64

void hmac_sha256_init(hmac_sha256_ctx *ctx, const void *key, size_t key_len) {
	if (!ctx) {
		return;
	}

	unsigned char block[SHA256_BLOCK_LEN];
	memset(block, 0, sizeof(block));
	if (key && key_len) {
		if (key_len > SHA256_BLOCK_LEN) {
			// A key longer than the block is replaced by its digest, which is
			// then zero-padded like any other. The vault's keys are 32 bytes,
			// so this path is for callers with a passphrase rather than a
			// derived key.
			sha256_ctx hash;
			sha256_init(&hash);
			sha256_update(&hash, key, key_len);
			sha256_final(&hash, block);
		} else {
			memcpy(block, key, key_len);
		}
	}

	unsigned char pad[SHA256_BLOCK_LEN];
	for (size_t i = 0; i < sizeof(pad); i++) {
		pad[i] = (unsigned char)(block[i] ^ 0x36);
	}
	sha256_init(&ctx->inner);
	sha256_update(&ctx->inner, pad, sizeof(pad));
	for (size_t i = 0; i < sizeof(pad); i++) {
		pad[i] = (unsigned char)(block[i] ^ 0x5c);
	}
	sha256_init(&ctx->outer);
	sha256_update(&ctx->outer, pad, sizeof(pad));

	// The key has done its work; leave no copy of it on the stack.
	memset(block, 0, sizeof(block));
	memset(pad, 0, sizeof(pad));
}

void hmac_sha256_update(hmac_sha256_ctx *ctx, const void *data, size_t len) {
	if (ctx) {
		sha256_update(&ctx->inner, data, len);
	}
}

void hmac_sha256_final(hmac_sha256_ctx *ctx, uint8_t out[HMAC_SHA256_LEN]) {
	if (!ctx || !out) {
		return;
	}
	unsigned char inner_digest[SHA256_DIGEST_LEN];
	sha256_final(&ctx->inner, inner_digest);
	sha256_update(&ctx->outer, inner_digest, sizeof(inner_digest));
	sha256_final(&ctx->outer, out);
	memset(inner_digest, 0, sizeof(inner_digest));
}

void hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[HMAC_SHA256_LEN]) {
	hmac_sha256_ctx ctx;
	hmac_sha256_init(&ctx, key, key_len);
	hmac_sha256_update(&ctx, data, len);
	hmac_sha256_final(&ctx, out);
}

// One HMAC over `first || second`, reusing a context that has already absorbed
// its key. The two copies are cheap -- two SHA-256 states and their partial
// blocks -- and it is the copy that makes the iteration count possible without
// rehashing the key a hundred thousand times.
static void hmac_two_parts(const hmac_sha256_ctx *base, const uint8_t *first, size_t first_len,
						   const uint8_t *second, size_t second_len, uint8_t out[HMAC_SHA256_LEN]) {
	hmac_sha256_ctx ctx = *base;
	if (first && first_len) {
		sha256_update(&ctx.inner, first, first_len);
	}
	if (second && second_len) {
		sha256_update(&ctx.inner, second, second_len);
	}
	unsigned char inner_digest[SHA256_DIGEST_LEN];
	sha256_final(&ctx.inner, inner_digest);
	sha256_update(&ctx.outer, inner_digest, sizeof(inner_digest));
	sha256_final(&ctx.outer, out);
	memset(inner_digest, 0, sizeof(inner_digest));
}

void pbkdf2_hmac_sha256(const void *password, size_t password_len, const uint8_t *salt, size_t salt_len,
						uint32_t iterations, uint8_t *out, size_t out_len) {
	if (!out || !out_len) {
		return;
	}
	if (iterations == 0) {
		iterations = 1;
	}

	hmac_sha256_ctx base;
	hmac_sha256_init(&base, password, password_len);

	uint32_t block = 1;
	size_t written = 0;
	while (written < out_len) {
		uint8_t counter[4] = {(uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8),
							  (uint8_t)block};
		uint8_t u[HMAC_SHA256_LEN];
		uint8_t t[HMAC_SHA256_LEN];
		hmac_two_parts(&base, salt, salt_len, counter, sizeof(counter), u);
		memcpy(t, u, sizeof(t));

		for (uint32_t i = 1; i < iterations; i++) {
			hmac_two_parts(&base, u, sizeof(u), NULL, 0, u);
			for (size_t j = 0; j < sizeof(t); j++) {
				t[j] ^= u[j];
			}
		}

		size_t take = out_len - written < sizeof(t) ? out_len - written : sizeof(t);
		memcpy(out + written, t, take);
		written += take;
		block++;

		memset(u, 0, sizeof(u));
		memset(t, 0, sizeof(t));
	}
	memset(&base, 0, sizeof(base));
}

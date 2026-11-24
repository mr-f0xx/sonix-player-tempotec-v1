#ifndef SYSTEM_CRYPTO_AES256_H
#define SYSTEM_CRYPTO_AES256_H

#include <stddef.h>
#include <stdint.h>

// AES-256 (FIPS 197), encryption only, and the counter mode the key vault
// uses.
//
// There is no inverse cipher here on purpose. CTR turns a block cipher into a
// keystream -- encrypt the counter, XOR it with the text -- so both writing and
// reading a vault need only the forward direction. That is the smaller and
// easier-audited half of AES, and it has no padding to get wrong.
//
// The counter mode is only safe while a (key, counter) pair is never reused.
// The vault gives every write a fresh 128-bit counter from /dev/urandom; at
// that size a repeat is not a thing that happens (see vaultstore.c).

typedef struct {
	uint32_t round_key[60]; // 15 round keys of four words, for a 256-bit key
} aes256_ctx;

void aes256_init(aes256_ctx *ctx, const uint8_t key[32]);

void aes256_encrypt_block(const aes256_ctx *ctx, const uint8_t in[16], uint8_t out[16]);

// XORs `len` bytes of `in` with the AES-256-CTR keystream starting from
// `counter`, into `out`. `counter` is left as it was: the caller's nonce is
// the first block and nothing else. Encryption and decryption are the same
// call. `in` and `out` may be the same buffer.
void aes256_ctr_xor(const aes256_ctx *ctx, const uint8_t counter[16], const uint8_t *in, uint8_t *out,
					size_t len);

// How many bytes a CTR counter can carry before it wraps. The vault never gets
// near it; the number is here so a caller can say so rather than assume it.
#define AES256_CTR_MAX_BYTES ((uint64_t)1 << 36) // 64 GiB

#endif /* SYSTEM_CRYPTO_AES256_H */

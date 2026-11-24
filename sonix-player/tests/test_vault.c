// The pocket vault, headless: AES-256 and its counter mode against the NIST
// vectors, HMAC and PBKDF2 against the RFCs, and the file itself against a
// temporary card.
//
// The cryptography is checked against published answers rather than against
// itself, because a vault that encrypts and decrypts consistently can still be
// encrypting with the wrong cipher. The store's failure cases -- a wrong PIN, a
// flipped byte, a dropped iteration count, a file cut short -- are checked just
// as closely: those are the paths nobody exercises until the day they matter.

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/crypto/aes256.h"
#include "src/system/crypto/kdf.h"
#include "src/system/vault/vaultstore.h"

static void hex(const unsigned char *bytes, size_t len, char *out) {
	for (size_t i = 0; i < len; i++) {
		sprintf(out + i * 2, "%02x", bytes[i]);
	}
	out[len * 2] = '\0';
}

static void expect_hex(const unsigned char *bytes, size_t len, const char *expected) {
	char got[2 * 256 + 1];
	assert(len <= 256);
	hex(bytes, len, got);
	if (strcmp(got, expected) != 0) {
		fprintf(stderr, "expected %s, got %s\n", expected, got);
		assert(!"wrong digest");
	}
}

static void test_aes(void) {
	// The all-zero key and the all-zero block. A 256-bit key schedule that
	// dropped the last subword round would still look plausible here only if
	// it were also wrong in the cipher; this is the cheapest known answer.
	aes256_ctx ctx;
	unsigned char key[32] = {0};
	unsigned char plain[16] = {0};
	unsigned char cipher[16];
	aes256_init(&ctx, key);
	aes256_encrypt_block(&ctx, plain, cipher);
	expect_hex(cipher, 16, "dc95c078a2408989ad48a21492842087");

	// FIPS 197 Appendix C.3: the counting key over the counting plaintext.
	// This walks every round of the key schedule and every ShiftRows case.
	for (int i = 0; i < 32; i++) {
		key[i] = (unsigned char)i;
	}
	for (int i = 0; i < 16; i++) {
		plain[i] = (unsigned char)(i * 0x11);
	}
	aes256_init(&ctx, key);
	aes256_encrypt_block(&ctx, plain, cipher);
	expect_hex(cipher, 16, "8ea2b7ca516745bfeafc49904b496089");

	// NIST SP 800-38A F.5.5: AES-256-CTR, the first block. A counter that
	// increments the wrong byte would still pass the block test above and
	// fail here.
	unsigned char ctr_key[32] = {
		0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
		0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4,
	};
	unsigned char counter[16] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
								 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
	unsigned char ctr_plain[16] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
								   0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
	unsigned char ctr_cipher[16];
	aes256_init(&ctx, ctr_key);
	aes256_ctr_xor(&ctx, counter, ctr_plain, ctr_cipher, sizeof(ctr_cipher));
	expect_hex(ctr_cipher, 16, "601ec313775789a5b7a7f504bbf3d228");

	// CTR is its own inverse, and the caller's counter is not changed: the
	// vault hands the same nonce back to itself on a second save.
	unsigned char round[16];
	aes256_init(&ctx, ctr_key);
	aes256_ctr_xor(&ctx, counter, ctr_cipher, round, sizeof(round));
	assert(memcmp(round, ctr_plain, sizeof(round)) == 0);
	assert(counter[0] == 0xf0 && counter[15] == 0xff);
}

static void test_kdf(void) {
	// RFC 4231 test case 1: twenty bytes of 0x0b and "Hi There".
	unsigned char key[20];
	memset(key, 0x0b, sizeof(key));
	unsigned char mac[HMAC_SHA256_LEN];
	const char *data = "Hi There";
	hmac_sha256(key, sizeof(key), data, strlen(data), mac);
	expect_hex(mac, sizeof(mac), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

	// RFC 4231 test case 2, whose key is longer than "Jefe": the short-key
	// path and the block-size rules are both exercised.
	hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, mac);
	expect_hex(mac, sizeof(mac), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

	// PBKDF2-HMAC-SHA256, the well-known vectors for password/salt. The
	// 4096-iteration output is the one that would catch an iteration loop
	// that stopped early and quietly returned U1.
	unsigned char derived[32];
	pbkdf2_hmac_sha256("password", 8, (const unsigned char *)"salt", 4, 1, derived, sizeof(derived));
	expect_hex(derived, sizeof(derived), "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
	pbkdf2_hmac_sha256("password", 8, (const unsigned char *)"salt", 4, 2, derived, sizeof(derived));
	expect_hex(derived, sizeof(derived), "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
	pbkdf2_hmac_sha256("password", 8, (const unsigned char *)"salt", 4, 4096, derived, sizeof(derived));
	expect_hex(derived, sizeof(derived), "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");

	// A second block of output: 64 bytes is what the vault asks for, and the
	// last 32 become the MAC key. If the block index were not folded into the
	// salt, this half would silently repeat the first.
	unsigned char both[64];
	pbkdf2_hmac_sha256("password", 8, (const unsigned char *)"salt", 4, 1, both, sizeof(both));
	expect_hex(both, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
	expect_hex(both + 32, 32, "4dbf3a2f3dad3377264bb7b8e8330d4efc7451418617dabef683735361cdc18c");
}

static void write_bytes(const char *path, const void *data, size_t len) {
	FILE *f = fopen(path, "wb");
	assert(f);
	assert(len == 0 || fwrite(data, 1, len, f) == len);
	assert(fclose(f) == 0);
}

static unsigned char *read_bytes(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	assert(f);
	assert(fseek(f, 0, SEEK_END) == 0);
	long size = ftell(f);
	assert(size >= 0);
	rewind(f);
	unsigned char *data = malloc((size_t)size ? (size_t)size : 1);
	assert(data);
	assert(fread(data, 1, (size_t)size, f) == (size_t)size);
	fclose(f);
	*len = (size_t)size;
	return data;
}

static bool file_contains(const unsigned char *data, size_t len, const char *needle) {
	size_t needle_len = strlen(needle);
	for (size_t i = 0; i + needle_len <= len; i++) {
		if (memcmp(data + i, needle, needle_len) == 0) {
			return true;
		}
	}
	return false;
}

static void test_paths(void) {
	char dir[64];
	char path[128];
	assert(vault_dir(dir, sizeof(dir), "/card"));
	assert(strcmp(dir, "/card/Vault") == 0);
	assert(vault_path(path, sizeof(path), dir, VAULT_FILE));
	assert(strcmp(path, "/card/Vault/vault.kbx") == 0);
	assert(!vault_dir(dir, 4, "/card"));
	assert(!vault_dir(dir, sizeof(dir), ""));
	assert(!vault_path(path, sizeof(path), dir, ""));
	assert(!vault_path(path, sizeof(path), NULL, VAULT_FILE));
}

static void test_pins(void) {
	assert(vault_pin_ok("1234"));
	assert(vault_pin_ok("123456789012"));
	// Four digits is the floor, twelve the ceiling.
	assert(!vault_pin_ok("123"));
	assert(!vault_pin_ok("1234567890123"));
	assert(!vault_pin_ok("12a4"));
	assert(!vault_pin_ok("12 34"));
	assert(!vault_pin_ok("+1234"));
	assert(!vault_pin_ok(""));
	assert(!vault_pin_ok(NULL));
}

static void test_round_trip(const char *dir) {
	char path[512];
	assert(vault_path(path, sizeof(path), dir, VAULT_FILE));
	remove(path);

	const char *secret = "wifi: hunter2\nseed: alpha braVo charlie delta echo\n";
	vault_session *session = NULL;
	assert(vault_create(path, "123456", secret, &session) == VAULT_OK);
	assert(session);
	assert(vault_exists(path));
	assert(strcmp(vault_session_text(session), secret) == 0);

	// The file is opaque: the secret is not in it, and neither is the PIN.
	// The header is the only part with a recognisable shape.
	size_t len = 0;
	unsigned char *raw = read_bytes(path, &len);
	assert(len > 52 + 32);
	assert(memcmp(raw, "SONIXVLT", 8) == 0);
	assert(!file_contains(raw, len, "hunter2"));
	assert(!file_contains(raw, len, "123456"));
	assert(!file_contains(raw, len, "seed:"));
	free(raw);

	// It opens with the PIN that made it, and with nothing else.
	vault_session *opened = NULL;
	assert(vault_unlock(path, "123456", &opened) == VAULT_OK);
	assert(strcmp(vault_session_text(opened), secret) == 0);
	vault_session_lock(opened);
	opened = NULL;
	assert(vault_unlock(path, "123457", &opened) == VAULT_ERR_AUTH);
	assert(opened == NULL);
	assert(vault_unlock(path, "123", &opened) == VAULT_ERR_PIN);
	assert(vault_unlock(path, "abcdef", &opened) == VAULT_ERR_PIN);

	// A save keeps the keys and changes the file: the same PIN opens the new
	// text, and the file on the card is not the one from before.
	unsigned char *before = read_bytes(path, &len);
	size_t before_len = len;
	assert(vault_session_save(session, "changed\n") == VAULT_OK);
	unsigned char *after = read_bytes(path, &len);
	assert(len > 52 + 32 && len < before_len);
	// The counter is random, so at least the nonce has moved.
	assert(memcmp(before + 32, after + 32, 16) != 0);
	free(before);
	free(after);
	vault_session_lock(session);
	assert(vault_unlock(path, "123456", &opened) == VAULT_OK);
	assert(strcmp(vault_session_text(opened), "changed\n") == 0);

	// A new PIN re-encrypts with a new salt: the old PIN stops working.
	assert(vault_session_change_pin(opened, "987654", "changed\n") == VAULT_OK);
	assert(strcmp(vault_session_text(opened), "changed\n") == 0);
	vault_session_lock(opened);
	opened = NULL;
	assert(vault_unlock(path, "123456", &opened) == VAULT_ERR_AUTH);
	assert(vault_unlock(path, "987654", &opened) == VAULT_OK);
	assert(strcmp(vault_session_text(opened), "changed\n") == 0);
	vault_session_lock(opened);

	// An empty vault is a vault: the ciphertext can be zero bytes, and the
	// tag still covers the header.
	assert(vault_erase(path));
	assert(vault_create(path, "1234", "", &session) == VAULT_OK);
	assert(strcmp(vault_session_text(session), "") == 0);
	vault_session_lock(session);
	assert(vault_unlock(path, "1234", &opened) == VAULT_OK);
	assert(strcmp(vault_session_text(opened), "") == 0);
	vault_session_lock(opened);

	// Creating over an existing vault is refused rather than quietly
	// replacing it.
	assert(vault_create(path, "5555", "second", &session) == VAULT_ERR_EXISTS);

	// A card with no vault says so instead of pretending the PIN was wrong.
	char missing[VAULT_PATH_MAX];
	snprintf(missing, sizeof(missing), "%s/none.kbx", dir);
	assert(vault_unlock(missing, "1234", &opened) == VAULT_ERR_NO_FILE);
	assert(!vault_exists(missing));

	assert(vault_erase(path));
	assert(!vault_exists(path));
	assert(vault_erase(path)); // and erasing nothing is not an error
}

static void test_damage(const char *dir) {
	char path[VAULT_PATH_MAX];
	char other[VAULT_PATH_MAX];
	assert(vault_path(path, sizeof(path), dir, "damage.kbx"));
	snprintf(other, sizeof(other), "%s", path);
	remove(path);

	vault_session *session = NULL;
	assert(vault_create(path, "246810", "a secret that must not come out\n", &session) == VAULT_OK);
	vault_session_lock(session);

	size_t len = 0;
	unsigned char *raw = read_bytes(path, &len);
	assert(len > 52 + 32);
	unsigned char *copy = malloc(len);
	assert(copy);

	// A flipped ciphertext byte is refused by the tag.
	memcpy(copy, raw, len);
	copy[52] ^= 0x01;
	write_bytes(other, copy, len);
	vault_session *opened = NULL;
	assert(vault_unlock(other, "246810", &opened) == VAULT_ERR_AUTH);

	// A changed salt or iteration count is refused too: those fields tell the
	// KDF what to do, and an attacker who can edit them would otherwise be
	// choosing the key.
	memcpy(copy, raw, len);
	copy[16] ^= 0x80;
	write_bytes(other, copy, len);
	assert(vault_unlock(other, "246810", &opened) == VAULT_ERR_AUTH);

	memcpy(copy, raw, len);
	// A valid-looking count, still within the accepted range, so this is
	// caught by the tag rather than the field check.
	copy[12] = 0x00;
	copy[13] = 0x01;
	copy[14] = 0x00;
	copy[15] = 0x00;
	write_bytes(other, copy, len);
	assert(vault_unlock(other, "246810", &opened) == VAULT_ERR_AUTH);

	// A different magic is not a vault at all.
	memcpy(copy, raw, len);
	copy[0] = 'X';
	write_bytes(other, copy, len);
	assert(vault_unlock(other, "246810", &opened) == VAULT_ERR_FORMAT);

	// A file cut short, and one with a tail nobody signed.
	write_bytes(other, raw, len - 1);
	assert(vault_unlock(other, "246810", &opened) == VAULT_ERR_FORMAT);
	unsigned char *longer = malloc(len + 1);
	assert(longer);
	memcpy(longer, raw, len);
	longer[len] = 0;
	write_bytes(other, longer, len + 1);
	assert(vault_unlock(other, "246810", &opened) == VAULT_ERR_FORMAT);
	free(longer);

	// A length field smaller than the ciphertext present is refused before
	// any of it is read as text.
	memcpy(copy, raw, len);
	copy[48] = 0;
	copy[49] = 0;
	copy[50] = 0;
	copy[51] = 1;
	write_bytes(other, copy, len);
	assert(vault_unlock(other, "246810", &opened) == VAULT_ERR_FORMAT);

	free(copy);
	free(raw);
	remove(other);
	remove(path);
}

static void test_limits(const char *dir) {
	char path[512];
	assert(vault_path(path, sizeof(path), dir, "limits.kbx"));
	remove(path);

	char *big = malloc(VAULT_MAX_TEXT + 2);
	assert(big);
	memset(big, 'a', VAULT_MAX_TEXT);
	big[VAULT_MAX_TEXT] = '\0';

	vault_session *session = NULL;
	assert(vault_create(path, "1111", big, &session) == VAULT_OK);
	assert(strlen(vault_session_text(session)) == VAULT_MAX_TEXT);
	vault_session_lock(session);

	// One byte over the ceiling is refused, by create and by save.
	big[VAULT_MAX_TEXT] = 'a';
	big[VAULT_MAX_TEXT + 1] = '\0';
	assert(vault_create(path, "1111", big, &session) == VAULT_ERR_TOO_LONG);
	vault_session *opened = NULL;
	assert(vault_unlock(path, "1111", &opened) == VAULT_OK);
	assert(vault_session_save(opened, big) == VAULT_ERR_TOO_LONG);
	assert(strlen(vault_session_text(opened)) == VAULT_MAX_TEXT); // unchanged
	vault_session_lock(opened);

	// UTF-8 survives byte for byte: the store never decodes or re-encodes it.
	const char *unicode = "Wi-Fi: caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x94\x90\n";
	assert(vault_erase(path));
	assert(vault_create(path, "2222", unicode, &session) == VAULT_OK);
	vault_session_lock(session);
	assert(vault_unlock(path, "2222", &opened) == VAULT_OK);
	assert(strcmp(vault_session_text(opened), unicode) == 0);
	vault_session_lock(opened);

	free(big);
	remove(path);
}

static void test_no_temporary(const char *dir) {
	DIR *handle = opendir(dir);
	assert(handle);
	struct dirent *entry;
	while ((entry = readdir(handle)) != NULL) {
		size_t len = strlen(entry->d_name);
		assert(len < 8 || strcmp(entry->d_name + len - 8, ".kbx.tmp") != 0);
	}
	closedir(handle);
}

int main(void) {
	char root[] = "/tmp/sonix-vault-XXXXXX";
	assert(mkdtemp(root));
	char dir[512];
	assert(vault_dir(dir, sizeof(dir), root));
	assert(mkdir(dir, 0700) == 0);

	test_aes();
	test_kdf();
	test_paths();
	test_pins();
	test_round_trip(dir);
	test_damage(dir);
	test_limits(dir);
	test_no_temporary(dir);

	char command[600];
	snprintf(command, sizeof(command), "rm -rf '%s'", root);
	assert(system(command) == 0);
	printf("PASS vault store\n");
	return 0;
}

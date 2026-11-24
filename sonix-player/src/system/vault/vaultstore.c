#include "vaultstore.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/crypto/aes256.h"
#include "src/system/crypto/kdf.h"

// The file, byte by byte:
//
//   0   8   "SONIXVLT"
//   8   2   version, big endian (1)
//   10  2   reserved, zero
//   12  4   PBKDF2 iterations, big endian
//   16  16  salt
//   32  16  counter (the CTR nonce)
//   48  4   ciphertext length, big endian
//   52  n   ciphertext
//   52+n 32 HMAC-SHA256 over bytes 0 .. 52+n-1
//
// Every field that changes what the ciphertext means is inside the HMAC, so a
// damaged salt, a lowered iteration count or a shortened length fails the tag
// rather than quietly changing the plaintext.
#define VAULT_MAGIC "SONIXVLT"
#define VAULT_MAGIC_LEN 8
#define VAULT_VERSION 1
#define VAULT_HEADER_LEN 52
#define VAULT_TAG_LEN 32
#define VAULT_MAX_FILE (VAULT_HEADER_LEN + VAULT_MAX_TEXT + VAULT_TAG_LEN)

#define SALT_LEN 16
#define NONCE_LEN 16

struct vault_session {
	char path[VAULT_PATH_MAX];
	char *text;
	size_t text_len;
	uint8_t enc_key[32];
	uint8_t mac_key[32];
	uint8_t salt[SALT_LEN];
	uint32_t iterations;
};

// Wiping through a volatile pointer: a plain memset on a local that is never
// read again is exactly the sort of thing a compiler is entitled to remove.
static void secure_zero(void *data, size_t len) {
	volatile unsigned char *p = data;
	while (len--) {
		*p++ = 0;
	}
}

static bool tag_equal(const uint8_t *a, const uint8_t *b) {
	unsigned char diff = 0;
	for (int i = 0; i < VAULT_TAG_LEN; i++) {
		diff |= (unsigned char)(a[i] ^ b[i]);
	}
	return diff == 0;
}

static uint32_t get_be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------

bool vault_dir(char *out, size_t size, const char *card_root) {
	if (!out || !size || !card_root || !card_root[0]) {
		return false;
	}
	int n = snprintf(out, size, "%s/%s", card_root, VAULT_FOLDER);
	return n > 0 && (size_t)n < size;
}

bool vault_path(char *out, size_t size, const char *dir, const char *name) {
	if (!out || !size || !dir || !dir[0] || !name || !name[0]) {
		return false;
	}
	int n = snprintf(out, size, "%s/%s", dir, name);
	return n > 0 && (size_t)n < size;
}

bool vault_pin_ok(const char *pin) {
	if (!pin) {
		return false;
	}
	size_t len = strlen(pin);
	if (len < VAULT_PIN_MIN || len > VAULT_PIN_MAX) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (pin[i] < '0' || pin[i] > '9') {
			return false;
		}
	}
	return true;
}

bool vault_exists(const char *path) {
	if (!path || !path[0]) {
		return false;
	}
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// The folder the file lives in. `<card>/Vault` is one level under the card
// root, which is there; a missing Vault folder is made on the first write, as
// the notes store makes its own.
static bool make_parent_dir(const char *path) {
	char dir[VAULT_PATH_MAX];
	int n = snprintf(dir, sizeof(dir), "%s", path);
	if (n <= 0 || (size_t)n >= sizeof(dir)) {
		return false;
	}
	char *slash = strrchr(dir, '/');
	if (!slash || slash == dir) {
		return false;
	}
	*slash = '\0';
	if (mkdir(dir, 0700) == 0) {
		return true;
	}
	struct stat st;
	return errno == EEXIST && stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

// ---------------------------------------------------------------------------
// randomness, keys and the seal
// ---------------------------------------------------------------------------

// /dev/urandom or nothing. A nonce made of the clock would turn two saves into
// two uses of the same keystream, and a salt made of the clock would let one
// guessed PIN open more than the vault it was guessed from -- so a failed read
// is a refusal to write, not a weaker file.
static bool random_bytes(uint8_t *out, size_t len) {
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) {
		return false;
	}
	size_t got = 0;
	while (got < len) {
		ssize_t n = read(fd, out + got, len - got);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			close(fd);
			return false;
		}
		if (n == 0) {
			close(fd);
			return false;
		}
		got += (size_t)n;
	}
	close(fd);
	return true;
}

static void derive_keys(const char *pin, const uint8_t salt[SALT_LEN], uint32_t iterations,
						uint8_t enc_key[32], uint8_t mac_key[32]) {
	uint8_t keys[64];
	pbkdf2_hmac_sha256(pin, strlen(pin), salt, SALT_LEN, iterations, keys, sizeof(keys));
	memcpy(enc_key, keys, 32);
	memcpy(mac_key, keys + 32, 32);
	secure_zero(keys, sizeof(keys));
}

static void build_header(uint8_t header[VAULT_HEADER_LEN], uint32_t iterations, const uint8_t salt[SALT_LEN],
						 const uint8_t nonce[NONCE_LEN], uint32_t text_len) {
	memset(header, 0, VAULT_HEADER_LEN);
	memcpy(header, VAULT_MAGIC, VAULT_MAGIC_LEN);
	header[8] = 0; // version, big endian
	header[9] = VAULT_VERSION;
	put_be32(header + 12, iterations);
	memcpy(header + 16, salt, SALT_LEN);
	memcpy(header + 32, nonce, NONCE_LEN);
	put_be32(header + 48, text_len);
}

static bool write_atomic(const char *path, const uint8_t header[VAULT_HEADER_LEN], const uint8_t *ciphertext,
						 size_t cipher_len, const uint8_t tag[VAULT_TAG_LEN]) {
	char temp[VAULT_PATH_MAX + 8];
	int n = snprintf(temp, sizeof(temp), "%s.tmp", path);
	if (n <= 0 || (size_t)n >= sizeof(temp)) {
		return false;
	}
	if (!make_parent_dir(path)) {
		return false;
	}

	FILE *f = fopen(temp, "wb");
	if (!f) {
		return false;
	}
	// A vault is nobody else's business, on the card as on the player. FAT
	// ignores the mode; the filesystems that do not ignore it are the ones
	// where it matters.
	(void)fchmod(fileno(f), 0600);

	bool ok = fwrite(header, 1, VAULT_HEADER_LEN, f) == VAULT_HEADER_LEN;
	if (ok && cipher_len) {
		ok = fwrite(ciphertext, 1, cipher_len, f) == cipher_len;
	}
	if (ok) {
		ok = fwrite(tag, 1, VAULT_TAG_LEN, f) == VAULT_TAG_LEN;
	}
	// The bytes have to be on the card before the rename, or a card pulled out
	// in between leaves the vault's name pointing at half a file.
	if (ok) {
		ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	}
	ok = fclose(f) == 0 && ok;
	if (!ok) {
		remove(temp);
		return false;
	}
	if (rename(temp, path) != 0) {
		remove(temp);
		return false;
	}
	return true;
}

// Encrypts `text` under the keys and writes the whole file. The counter is new
// on every call; the keys and salt are the caller's.
static vault_status_t seal(const char *path, const uint8_t enc_key[32], const uint8_t mac_key[32],
						   uint32_t iterations, const uint8_t salt[SALT_LEN], const char *text) {
	size_t len = text ? strlen(text) : 0;
	if (len > VAULT_MAX_TEXT) {
		return VAULT_ERR_TOO_LONG;
	}

	uint8_t nonce[NONCE_LEN];
	if (!random_bytes(nonce, sizeof(nonce))) {
		return VAULT_ERR_RANDOM;
	}

	uint8_t *ciphertext = malloc(len ? len : 1);
	if (!ciphertext) {
		return VAULT_ERR_NOMEM;
	}

	aes256_ctx aes;
	aes256_init(&aes, enc_key);
	aes256_ctr_xor(&aes, nonce, (const uint8_t *)(text ? text : ""), ciphertext, len);
	secure_zero(&aes, sizeof(aes));

	uint8_t header[VAULT_HEADER_LEN];
	build_header(header, iterations, salt, nonce, (uint32_t)len);

	uint8_t tag[VAULT_TAG_LEN];
	hmac_sha256_ctx mac;
	hmac_sha256_init(&mac, mac_key, 32);
	hmac_sha256_update(&mac, header, sizeof(header));
	if (len) {
		hmac_sha256_update(&mac, ciphertext, len);
	}
	hmac_sha256_final(&mac, tag);

	bool written = write_atomic(path, header, ciphertext, len, tag);
	secure_zero(&mac, sizeof(mac));
	secure_zero(tag, sizeof(tag));
	secure_zero(ciphertext, len);
	free(ciphertext);

	return written ? VAULT_OK : VAULT_ERR_IO;
}

// ---------------------------------------------------------------------------
// reading the file back
// ---------------------------------------------------------------------------

typedef struct {
	uint32_t iterations;
	uint8_t salt[SALT_LEN];
	uint8_t nonce[NONCE_LEN];
	size_t text_len;
	uint8_t ciphertext[VAULT_MAX_TEXT];
	uint8_t tag[VAULT_TAG_LEN];
} vault_file_t;

static vault_status_t read_file(const char *path, vault_file_t *out) {
	if (!path || !out) {
		return VAULT_ERR_ARG;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		return errno == ENOENT ? VAULT_ERR_NO_FILE : VAULT_ERR_IO;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return VAULT_ERR_IO;
	}
	long size = ftell(f);
	if (size < 0) {
		fclose(f);
		return VAULT_ERR_IO;
	}
	if ((unsigned long)size > VAULT_MAX_FILE || (unsigned long)size < VAULT_HEADER_LEN + VAULT_TAG_LEN) {
		fclose(f);
		return VAULT_ERR_FORMAT;
	}
	rewind(f);

	uint8_t *raw = malloc((size_t)size);
	if (!raw) {
		fclose(f);
		return VAULT_ERR_NOMEM;
	}
	size_t got = fread(raw, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size) {
		free(raw);
		return VAULT_ERR_IO;
	}

	vault_status_t status = VAULT_ERR_FORMAT;
	if (memcmp(raw, VAULT_MAGIC, VAULT_MAGIC_LEN) != 0) {
		goto done;
	}
	if (((uint32_t)raw[8] << 8 | raw[9]) != VAULT_VERSION) {
		goto done;
	}
	out->iterations = get_be32(raw + 12);
	if (out->iterations < VAULT_PBKDF2_MIN || out->iterations > VAULT_PBKDF2_MAX) {
		goto done;
	}
	out->text_len = get_be32(raw + 48);
	if (out->text_len > VAULT_MAX_TEXT) {
		goto done;
	}
	// The length has to account for the whole file: a length smaller than the
	// bytes present would leave a tail nobody authenticates, and a larger one
	// would read past the buffer.
	if ((uint64_t)VAULT_HEADER_LEN + out->text_len + VAULT_TAG_LEN != (uint64_t)size) {
		goto done;
	}
	memcpy(out->salt, raw + 16, SALT_LEN);
	memcpy(out->nonce, raw + 32, NONCE_LEN);
	if (out->text_len) {
		memcpy(out->ciphertext, raw + VAULT_HEADER_LEN, out->text_len);
	}
	memcpy(out->tag, raw + VAULT_HEADER_LEN + out->text_len, VAULT_TAG_LEN);
	status = VAULT_OK;

done:
	secure_zero(raw, (size_t)size);
	free(raw);
	return status;
}

static vault_status_t verify_and_decrypt(const vault_file_t *file, const uint8_t mac_key[32],
										 const uint8_t enc_key[32], char **text_out, size_t *len_out) {
	// The tag over exactly the bytes the header and ciphertext would be. The
	// header is rebuilt from the parsed fields rather than the raw file, so
	// there is one definition of what is signed.
	uint8_t header[VAULT_HEADER_LEN];
	build_header(header, file->iterations, file->salt, file->nonce, (uint32_t)file->text_len);

	uint8_t tag[VAULT_TAG_LEN];
	hmac_sha256_ctx mac;
	hmac_sha256_init(&mac, mac_key, 32);
	hmac_sha256_update(&mac, header, sizeof(header));
	if (file->text_len) {
		hmac_sha256_update(&mac, file->ciphertext, file->text_len);
	}
	hmac_sha256_final(&mac, tag);
	secure_zero(&mac, sizeof(mac));

	if (!tag_equal(tag, file->tag)) {
		secure_zero(tag, sizeof(tag));
		return VAULT_ERR_AUTH;
	}
	secure_zero(tag, sizeof(tag));

	char *text = malloc(file->text_len + 1);
	if (!text) {
		return VAULT_ERR_NOMEM;
	}

	aes256_ctx aes;
	aes256_init(&aes, enc_key);
	aes256_ctr_xor(&aes, file->nonce, file->ciphertext, (uint8_t *)text, file->text_len);
	secure_zero(&aes, sizeof(aes));
	text[file->text_len] = '\0';

	*text_out = text;
	*len_out = file->text_len;
	return VAULT_OK;
}

// ---------------------------------------------------------------------------
// sessions
// ---------------------------------------------------------------------------

static vault_session *session_new(const char *path, char *text, size_t text_len, const uint8_t enc_key[32],
								  const uint8_t mac_key[32], const uint8_t salt[SALT_LEN], uint32_t iterations) {
	vault_session *session = calloc(1, sizeof(*session));
	if (!session) {
		return NULL;
	}
	snprintf(session->path, sizeof(session->path), "%s", path);
	session->text = text;
	session->text_len = text_len;
	memcpy(session->enc_key, enc_key, 32);
	memcpy(session->mac_key, mac_key, 32);
	memcpy(session->salt, salt, SALT_LEN);
	session->iterations = iterations;
	return session;
}

vault_status_t vault_create(const char *path, const char *pin, const char *text, vault_session **out) {
	if (!path || !path[0] || !out || !text) {
		return VAULT_ERR_ARG;
	}
	*out = NULL;
	if (!vault_pin_ok(pin)) {
		return VAULT_ERR_PIN;
	}
	if (strlen(text) > VAULT_MAX_TEXT) {
		return VAULT_ERR_TOO_LONG;
	}
	if (vault_exists(path)) {
		return VAULT_ERR_EXISTS;
	}

	char *copy = malloc(strlen(text) + 1);
	if (!copy) {
		return VAULT_ERR_NOMEM;
	}
	memcpy(copy, text, strlen(text) + 1);

	uint8_t salt[SALT_LEN];
	if (!random_bytes(salt, sizeof(salt))) {
		free(copy);
		return VAULT_ERR_RANDOM;
	}
	uint8_t enc_key[32], mac_key[32];
	derive_keys(pin, salt, VAULT_PBKDF2_ITERATIONS, enc_key, mac_key);

	vault_status_t status = seal(path, enc_key, mac_key, VAULT_PBKDF2_ITERATIONS, salt, text);
	if (status == VAULT_OK) {
		vault_session *session =
			session_new(path, copy, strlen(copy), enc_key, mac_key, salt, VAULT_PBKDF2_ITERATIONS);
		if (!session) {
			status = VAULT_ERR_NOMEM;
		} else {
			*out = session;
			copy = NULL;
		}
	}

	secure_zero(enc_key, sizeof(enc_key));
	secure_zero(mac_key, sizeof(mac_key));
	if (copy) {
		secure_zero(copy, strlen(copy));
		free(copy);
	}
	return status;
}

vault_status_t vault_unlock(const char *path, const char *pin, vault_session **out) {
	if (!path || !path[0] || !out) {
		return VAULT_ERR_ARG;
	}
	*out = NULL;
	if (!vault_pin_ok(pin)) {
		return VAULT_ERR_PIN;
	}

	vault_file_t *file = malloc(sizeof(*file));
	if (!file) {
		return VAULT_ERR_NOMEM;
	}
	vault_status_t status = read_file(path, file);
	if (status != VAULT_OK) {
		free(file);
		return status;
	}

	uint8_t enc_key[32], mac_key[32];
	derive_keys(pin, file->salt, file->iterations, enc_key, mac_key);

	char *text = NULL;
	size_t text_len = 0;
	status = verify_and_decrypt(file, mac_key, enc_key, &text, &text_len);

	if (status == VAULT_OK) {
		vault_session *session =
			session_new(path, text, text_len, enc_key, mac_key, file->salt, file->iterations);
		if (!session) {
			status = VAULT_ERR_NOMEM;
		} else {
			*out = session;
			text = NULL; // the session owns it now
		}
	}

	secure_zero(enc_key, sizeof(enc_key));
	secure_zero(mac_key, sizeof(mac_key));
	secure_zero(file, sizeof(*file));
	free(file);
	if (text) {
		secure_zero(text, text_len);
		free(text);
	}
	return status;
}

const char *vault_session_text(const vault_session *session) { return session ? session->text : NULL; }

vault_status_t vault_session_save(vault_session *session, const char *text) {
	if (!session || !text) {
		return VAULT_ERR_ARG;
	}
	size_t len = strlen(text);
	if (len > VAULT_MAX_TEXT) {
		return VAULT_ERR_TOO_LONG;
	}

	// The new text is copied before anything else, so the caller may pass the
	// session's own string back to it (the editor does not, but the API should
	// not be a trap).
	char *copy = malloc(len + 1);
	if (!copy) {
		return VAULT_ERR_NOMEM;
	}
	memcpy(copy, text, len + 1);

	vault_status_t status =
		seal(session->path, session->enc_key, session->mac_key, session->iterations, session->salt, text);
	if (status == VAULT_OK) {
		secure_zero(session->text, session->text_len);
		free(session->text);
		session->text = copy;
		session->text_len = len;
		copy = NULL;
	}
	if (copy) {
		secure_zero(copy, len);
		free(copy);
	}
	return status;
}

vault_status_t vault_session_change_pin(vault_session *session, const char *new_pin, const char *text) {
	if (!session || !text) {
		return VAULT_ERR_ARG;
	}
	if (!vault_pin_ok(new_pin)) {
		return VAULT_ERR_PIN;
	}
	size_t len = strlen(text);
	if (len > VAULT_MAX_TEXT) {
		return VAULT_ERR_TOO_LONG;
	}

	char *copy = malloc(len + 1);
	if (!copy) {
		return VAULT_ERR_NOMEM;
	}
	memcpy(copy, text, len + 1);

	uint8_t salt[SALT_LEN];
	if (!random_bytes(salt, sizeof(salt))) {
		secure_zero(copy, len);
		free(copy);
		return VAULT_ERR_RANDOM;
	}
	uint8_t enc_key[32], mac_key[32];
	derive_keys(new_pin, salt, session->iterations, enc_key, mac_key);

	vault_status_t status = seal(session->path, enc_key, mac_key, session->iterations, salt, text);
	if (status == VAULT_OK) {
		secure_zero(session->enc_key, sizeof(session->enc_key));
		secure_zero(session->mac_key, sizeof(session->mac_key));
		memcpy(session->enc_key, enc_key, sizeof(enc_key));
		memcpy(session->mac_key, mac_key, sizeof(mac_key));
		memcpy(session->salt, salt, sizeof(salt));
		secure_zero(session->text, session->text_len);
		free(session->text);
		session->text = copy;
		session->text_len = len;
		copy = NULL;
	}

	secure_zero(enc_key, sizeof(enc_key));
	secure_zero(mac_key, sizeof(mac_key));
	if (copy) {
		secure_zero(copy, len);
		free(copy);
	}
	return status;
}

void vault_session_lock(vault_session *session) {
	if (!session) {
		return;
	}
	secure_zero(session->enc_key, sizeof(session->enc_key));
	secure_zero(session->mac_key, sizeof(session->mac_key));
	secure_zero(session->salt, sizeof(session->salt));
	if (session->text) {
		secure_zero(session->text, session->text_len);
		free(session->text);
	}
	secure_zero(session, sizeof(*session));
	free(session);
}

bool vault_erase(const char *path) {
	if (!path || !path[0]) {
		return false;
	}
	char temp[VAULT_PATH_MAX + 8];
	int n = snprintf(temp, sizeof(temp), "%s.tmp", path);
	if (n > 0 && (size_t)n < sizeof(temp)) {
		remove(temp);
	}
	if (remove(path) == 0) {
		return true;
	}
	return errno == ENOENT;
}

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
//   8   2   version, big endian (2; 1 is the old single-text vault)
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
#define VAULT_VERSION 2
#define VAULT_VERSION_LEGACY 1
#define VAULT_HEADER_LEN 52
#define VAULT_TAG_LEN 32
#define VAULT_MAX_FILE (VAULT_HEADER_LEN + VAULT_MAX_DATA + VAULT_TAG_LEN)

// The decrypted table.
#define VAULT_BLOB_MAGIC "VLT2"
#define VAULT_BLOB_MAGIC_LEN 4
#define VAULT_BLOB_HEADER_LEN 8

#define SALT_LEN 16
#define NONCE_LEN 16

// The name the one entry of an old single-text vault is given when it is
// opened by this build. Not translated: it is data in the vault, and the page
// draws entry titles as they stand.
#define VAULT_LEGACY_TITLE "Vault"

typedef struct {
	char *title;
	char *secret;
} vault_entry;

struct vault_session {
	char path[VAULT_PATH_MAX];
	vault_entry *entries;
	size_t count;
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

static void build_header(uint8_t header[VAULT_HEADER_LEN], uint32_t version, uint32_t iterations,
						 const uint8_t salt[SALT_LEN], const uint8_t nonce[NONCE_LEN], uint32_t text_len) {
	memset(header, 0, VAULT_HEADER_LEN);
	memcpy(header, VAULT_MAGIC, VAULT_MAGIC_LEN);
	header[8] = (uint8_t)(version >> 8);
	header[9] = (uint8_t)version;
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

// Encrypts `blob` (the entry table) under the keys and writes the whole file.
// The counter is new on every call; the keys and salt are the caller's. Every
// file this writes is version 2, including one that was version 1 on the card.
static vault_status_t seal(const char *path, const uint8_t enc_key[32], const uint8_t mac_key[32],
						   uint32_t iterations, const uint8_t salt[SALT_LEN], const uint8_t *blob,
						   size_t len) {
	if (len > VAULT_MAX_DATA) {
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
	aes256_ctr_xor(&aes, nonce, blob ? blob : (const uint8_t *)"", ciphertext, len);
	secure_zero(&aes, sizeof(aes));

	uint8_t header[VAULT_HEADER_LEN];
	build_header(header, VAULT_VERSION, iterations, salt, nonce, (uint32_t)len);

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
// the entry table
// ---------------------------------------------------------------------------

static void entry_free(vault_entry *entry) {
	if (!entry) {
		return;
	}
	if (entry->title) {
		secure_zero(entry->title, strlen(entry->title));
		free(entry->title);
		entry->title = NULL;
	}
	if (entry->secret) {
		secure_zero(entry->secret, strlen(entry->secret));
		free(entry->secret);
		entry->secret = NULL;
	}
}

static void entries_free(vault_entry *entries, size_t count) {
	if (!entries) {
		return;
	}
	for (size_t i = 0; i < count; i++) {
		entry_free(&entries[i]);
	}
	free(entries);
}

static char *dup_text(const uint8_t *bytes, size_t len) {
	char *text = malloc(len + 1);
	if (!text) {
		return NULL;
	}
	if (len) {
		memcpy(text, bytes, len);
	}
	text[len] = '\0';
	return text;
}

// The size an entry takes in the table, or 0 when it cannot be stored at all.
static size_t entry_size(const char *title, const char *secret) {
	if (!title || !secret || !title[0] || !secret[0]) {
		return 0;
	}
	size_t title_len = strlen(title);
	size_t secret_len = strlen(secret);
	if (title_len > VAULT_MAX_TITLE || secret_len > VAULT_MAX_SECRET) {
		return 0;
	}
	return 8 + title_len + secret_len;
}

// A title is one line in the list and one line in the editor. A newline would
// be drawn as a blank strip (or, in a one-line textarea, silently lost), so it
// is refused rather than stored and shown wrong later. The secret may contain
// anything, newlines included: a seed phrase is often written in lines.
static bool title_ok(const char *title) {
	return title && title[0] && !strpbrk(title, "\r\n");
}

static size_t table_size(const vault_session *session) {
	size_t size = VAULT_BLOB_HEADER_LEN;
	for (size_t i = 0; i < session->count; i++) {
		size += 8 + strlen(session->entries[i].title) + strlen(session->entries[i].secret);
	}
	return size;
}

static vault_status_t blob_build(const vault_session *session, uint8_t **out, size_t *len_out) {
	if (!session || !out || !len_out) {
		return VAULT_ERR_ARG;
	}
	size_t total = table_size(session);
	if (total > VAULT_MAX_DATA) {
		return VAULT_ERR_TOO_LONG;
	}

	uint8_t *blob = malloc(total);
	if (!blob) {
		return VAULT_ERR_NOMEM;
	}
	memcpy(blob, VAULT_BLOB_MAGIC, VAULT_BLOB_MAGIC_LEN);
	put_be32(blob + 4, (uint32_t)session->count);
	size_t at = VAULT_BLOB_HEADER_LEN;
	for (size_t i = 0; i < session->count; i++) {
		size_t title_len = strlen(session->entries[i].title);
		size_t secret_len = strlen(session->entries[i].secret);
		put_be32(blob + at, (uint32_t)title_len);
		put_be32(blob + at + 4, (uint32_t)secret_len);
		at += 8;
		memcpy(blob + at, session->entries[i].title, title_len);
		at += title_len;
		memcpy(blob + at, session->entries[i].secret, secret_len);
		at += secret_len;
	}

	*out = blob;
	*len_out = total;
	return VAULT_OK;
}

// The old single-text vault: the whole decrypted block is one entry. An empty
// old vault is an empty list.
static vault_status_t legacy_blob(const uint8_t *data, size_t len, vault_entry **out, size_t *count_out) {
	*out = NULL;
	*count_out = 0;
	if (len == 0) {
		return VAULT_OK;
	}
	if (len > VAULT_MAX_SECRET || memchr(data, '\0', len)) {
		return VAULT_ERR_FORMAT;
	}
	vault_entry *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		return VAULT_ERR_NOMEM;
	}
	entry->title = dup_text((const uint8_t *)VAULT_LEGACY_TITLE, strlen(VAULT_LEGACY_TITLE));
	entry->secret = dup_text(data, len);
	if (!entry->title || !entry->secret) {
		entry_free(entry);
		free(entry);
		return VAULT_ERR_NOMEM;
	}
	*out = entry;
	*count_out = 1;
	return VAULT_OK;
}

static vault_status_t blob_parse(uint32_t version, const uint8_t *data, size_t len, vault_entry **out,
								 size_t *count_out) {
	if (!out || !count_out) {
		return VAULT_ERR_ARG;
	}
	*out = NULL;
	*count_out = 0;

	if (version == VAULT_VERSION_LEGACY) {
		return legacy_blob(data, len, out, count_out);
	}
	if (version != VAULT_VERSION || !data || len < VAULT_BLOB_HEADER_LEN ||
		memcmp(data, VAULT_BLOB_MAGIC, VAULT_BLOB_MAGIC_LEN) != 0) {
		return VAULT_ERR_FORMAT;
	}

	uint32_t count = get_be32(data + 4);
	if (count > VAULT_MAX_ENTRIES) {
		return VAULT_ERR_FORMAT;
	}
	vault_entry *entries = count ? calloc(count, sizeof(*entries)) : NULL;
	if (count && !entries) {
		return VAULT_ERR_NOMEM;
	}

	size_t at = VAULT_BLOB_HEADER_LEN;
	for (uint32_t i = 0; i < count; i++) {
		if (at + 8 > len) {
			goto bad;
		}
		uint32_t title_len = get_be32(data + at);
		uint32_t secret_len = get_be32(data + at + 4);
		at += 8;
		if (!title_len || !secret_len || title_len > VAULT_MAX_TITLE || secret_len > VAULT_MAX_SECRET ||
			(size_t)title_len + (size_t)secret_len > len - at) {
			goto bad;
		}
		if (memchr(data + at, '\0', title_len) || memchr(data + at + title_len, '\0', secret_len)) {
			goto bad;
		}
		entries[i].title = dup_text(data + at, title_len);
		entries[i].secret = dup_text(data + at + title_len, secret_len);
		if (!entries[i].title || !entries[i].secret) {
			entries_free(entries, count);
			return VAULT_ERR_NOMEM;
		}
		at += (size_t)title_len + (size_t)secret_len;
	}
	if (at != len) {
		goto bad;
	}

	*out = entries;
	*count_out = count;
	return VAULT_OK;

bad:
	entries_free(entries, count);
	return VAULT_ERR_FORMAT;
}

// ---------------------------------------------------------------------------
// reading the file back
// ---------------------------------------------------------------------------

typedef struct {
	uint32_t version;
	uint32_t iterations;
	uint8_t salt[SALT_LEN];
	uint8_t nonce[NONCE_LEN];
	size_t text_len;
	uint8_t *ciphertext;
	uint8_t tag[VAULT_TAG_LEN];
} vault_file_t;

static void file_clear(vault_file_t *file) {
	if (!file) {
		return;
	}
	if (file->ciphertext) {
		secure_zero(file->ciphertext, file->text_len);
		free(file->ciphertext);
		file->ciphertext = NULL;
	}
}

static vault_status_t read_file(const char *path, vault_file_t *out) {
	if (!path || !out) {
		return VAULT_ERR_ARG;
	}
	memset(out, 0, sizeof(*out));

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
	uint32_t version = ((uint32_t)raw[8] << 8) | raw[9];
	if (version != VAULT_VERSION && version != VAULT_VERSION_LEGACY) {
		goto done;
	}
	out->version = version;
	out->iterations = get_be32(raw + 12);
	if (out->iterations < VAULT_PBKDF2_MIN || out->iterations > VAULT_PBKDF2_MAX) {
		goto done;
	}
	out->text_len = get_be32(raw + 48);
	if (out->text_len > VAULT_MAX_DATA) {
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
		out->ciphertext = malloc(out->text_len);
		if (!out->ciphertext) {
			status = VAULT_ERR_NOMEM;
			goto done;
		}
		memcpy(out->ciphertext, raw + VAULT_HEADER_LEN, out->text_len);
	}
	memcpy(out->tag, raw + VAULT_HEADER_LEN + out->text_len, VAULT_TAG_LEN);
	status = VAULT_OK;

done:
	secure_zero(raw, (size_t)size);
	free(raw);
	if (status != VAULT_OK) {
		file_clear(out);
	}
	return status;
}

static vault_status_t verify_and_decrypt(const vault_file_t *file, const uint8_t mac_key[32],
										 const uint8_t enc_key[32], uint8_t **text_out, size_t *len_out) {
	// The tag over exactly the bytes the header and ciphertext would be. The
	// header is rebuilt from the parsed fields rather than the raw file, so
	// there is one definition of what is signed.
	uint8_t header[VAULT_HEADER_LEN];
	build_header(header, file->version, file->iterations, file->salt, file->nonce, (uint32_t)file->text_len);

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

	uint8_t *text = malloc(file->text_len + 1);
	if (!text) {
		return VAULT_ERR_NOMEM;
	}

	aes256_ctx aes;
	aes256_init(&aes, enc_key);
	aes256_ctr_xor(&aes, file->nonce, file->ciphertext, text, file->text_len);
	secure_zero(&aes, sizeof(aes));
	text[file->text_len] = '\0';

	*text_out = text;
	*len_out = file->text_len;
	return VAULT_OK;
}

// ---------------------------------------------------------------------------
// sessions
// ---------------------------------------------------------------------------

static vault_session *session_new(const char *path, vault_entry *entries, size_t count,
								  const uint8_t enc_key[32], const uint8_t mac_key[32],
								  const uint8_t salt[SALT_LEN], uint32_t iterations) {
	if (!path || (count && !entries)) {
		return NULL;
	}
	vault_session *session = calloc(1, sizeof(*session));
	if (!session) {
		return NULL;
	}
	snprintf(session->path, sizeof(session->path), "%s", path);
	session->entries = entries;
	session->count = count;
	memcpy(session->enc_key, enc_key, 32);
	memcpy(session->mac_key, mac_key, 32);
	memcpy(session->salt, salt, SALT_LEN);
	session->iterations = iterations;
	return session;
}

size_t vault_session_count(const vault_session *session) { return session ? session->count : 0; }

const char *vault_session_title(const vault_session *session, size_t index) {
	if (!session || index >= session->count) {
		return NULL;
	}
	return session->entries[index].title;
}

const char *vault_session_secret(const vault_session *session, size_t index) {
	if (!session || index >= session->count) {
		return NULL;
	}
	return session->entries[index].secret;
}

vault_status_t vault_session_add(vault_session *session, const char *title, const char *secret,
								 size_t *index_out) {
	if (index_out) {
		*index_out = 0;
	}
	if (!session || !title_ok(title) || !secret || !secret[0]) {
		return VAULT_ERR_ARG;
	}
	size_t extra = entry_size(title, secret);
	if (extra == 0) {
		return VAULT_ERR_TOO_LONG;
	}
	if (session->count >= VAULT_MAX_ENTRIES) {
		return VAULT_ERR_FULL;
	}
	if (table_size(session) + extra > VAULT_MAX_DATA) {
		return VAULT_ERR_TOO_LONG;
	}

	char *title_copy = dup_text((const uint8_t *)title, strlen(title));
	char *secret_copy = dup_text((const uint8_t *)secret, strlen(secret));
	if (!title_copy || !secret_copy) {
		entry_free(&(vault_entry){.title = title_copy, .secret = secret_copy});
		return VAULT_ERR_NOMEM;
	}

	vault_entry *grown = realloc(session->entries, (session->count + 1) * sizeof(*grown));
	if (!grown) {
		entry_free(&(vault_entry){.title = title_copy, .secret = secret_copy});
		return VAULT_ERR_NOMEM;
	}
	session->entries = grown;
	session->entries[session->count].title = title_copy;
	session->entries[session->count].secret = secret_copy;
	if (index_out) {
		*index_out = session->count;
	}
	session->count++;
	return VAULT_OK;
}

vault_status_t vault_session_update(vault_session *session, size_t index, const char *title,
									const char *secret) {
	if (!session || index >= session->count || !title_ok(title) || !secret || !secret[0]) {
		return VAULT_ERR_ARG;
	}
	size_t extra = entry_size(title, secret);
	if (extra == 0) {
		return VAULT_ERR_TOO_LONG;
	}
	size_t old = 8 + strlen(session->entries[index].title) + strlen(session->entries[index].secret);
	if (table_size(session) - old + extra > VAULT_MAX_DATA) {
		return VAULT_ERR_TOO_LONG;
	}

	char *title_copy = dup_text((const uint8_t *)title, strlen(title));
	char *secret_copy = dup_text((const uint8_t *)secret, strlen(secret));
	if (!title_copy || !secret_copy) {
		entry_free(&(vault_entry){.title = title_copy, .secret = secret_copy});
		return VAULT_ERR_NOMEM;
	}

	entry_free(&session->entries[index]);
	session->entries[index].title = title_copy;
	session->entries[index].secret = secret_copy;
	return VAULT_OK;
}

vault_status_t vault_session_remove(vault_session *session, size_t index) {
	if (!session || index >= session->count) {
		return VAULT_ERR_ARG;
	}
	entry_free(&session->entries[index]);
	if (index + 1 < session->count) {
		memmove(&session->entries[index], &session->entries[index + 1],
				(session->count - index - 1) * sizeof(*session->entries));
	}
	session->count--;
	return VAULT_OK;
}

vault_status_t vault_session_save(vault_session *session) {
	if (!session) {
		return VAULT_ERR_ARG;
	}
	uint8_t *blob = NULL;
	size_t len = 0;
	vault_status_t status = blob_build(session, &blob, &len);
	if (status != VAULT_OK) {
		return status;
	}
	status = seal(session->path, session->enc_key, session->mac_key, session->iterations, session->salt, blob, len);
	secure_zero(blob, len);
	free(blob);
	return status;
}

vault_status_t vault_session_change_pin(vault_session *session, const char *new_pin) {
	if (!session) {
		return VAULT_ERR_ARG;
	}
	if (!vault_pin_ok(new_pin)) {
		return VAULT_ERR_PIN;
	}
	uint8_t *blob = NULL;
	size_t len = 0;
	vault_status_t status = blob_build(session, &blob, &len);
	if (status != VAULT_OK) {
		return status;
	}

	uint8_t salt[SALT_LEN];
	if (!random_bytes(salt, sizeof(salt))) {
		secure_zero(blob, len);
		free(blob);
		return VAULT_ERR_RANDOM;
	}
	uint8_t enc_key[32], mac_key[32];
	// A PIN change is also the moment to bring an old file up to the current
	// work factor, not to preserve whatever count it was created with.
	derive_keys(new_pin, salt, VAULT_PBKDF2_ITERATIONS, enc_key, mac_key);

	status = seal(session->path, enc_key, mac_key, VAULT_PBKDF2_ITERATIONS, salt, blob, len);
	if (status == VAULT_OK) {
		secure_zero(session->enc_key, sizeof(session->enc_key));
		secure_zero(session->mac_key, sizeof(session->mac_key));
		memcpy(session->enc_key, enc_key, sizeof(enc_key));
		memcpy(session->mac_key, mac_key, sizeof(mac_key));
		memcpy(session->salt, salt, sizeof(salt));
		session->iterations = VAULT_PBKDF2_ITERATIONS;
	}

	secure_zero(enc_key, sizeof(enc_key));
	secure_zero(mac_key, sizeof(mac_key));
	secure_zero(blob, len);
	free(blob);
	return status;
}

void vault_session_lock(vault_session *session) {
	if (!session) {
		return;
	}
	secure_zero(session->enc_key, sizeof(session->enc_key));
	secure_zero(session->mac_key, sizeof(session->mac_key));
	secure_zero(session->salt, sizeof(session->salt));
	entries_free(session->entries, session->count);
	session->entries = NULL;
	session->count = 0;
	secure_zero(session, sizeof(*session));
	free(session);
}

vault_status_t vault_create(const char *path, const char *pin, vault_session **out) {
	if (!path || !path[0] || !out) {
		return VAULT_ERR_ARG;
	}
	*out = NULL;
	if (!vault_pin_ok(pin)) {
		return VAULT_ERR_PIN;
	}
	if (vault_exists(path)) {
		return VAULT_ERR_EXISTS;
	}

	uint8_t salt[SALT_LEN];
	if (!random_bytes(salt, sizeof(salt))) {
		return VAULT_ERR_RANDOM;
	}
	uint8_t enc_key[32], mac_key[32];
	derive_keys(pin, salt, VAULT_PBKDF2_ITERATIONS, enc_key, mac_key);

	// The empty table: the magic and a count of zero.
	uint8_t blob[VAULT_BLOB_HEADER_LEN];
	memcpy(blob, VAULT_BLOB_MAGIC, VAULT_BLOB_MAGIC_LEN);
	put_be32(blob + 4, 0);

	// Hold the session before anything is written. A failed allocation after a
	// successful seal would leave a file the page could not show, and the next
	// vault_create would refuse it as an existing vault.
	vault_session *session =
		session_new(path, NULL, 0, enc_key, mac_key, salt, VAULT_PBKDF2_ITERATIONS);
	if (!session) {
		secure_zero(enc_key, sizeof(enc_key));
		secure_zero(mac_key, sizeof(mac_key));
		return VAULT_ERR_NOMEM;
	}

	vault_status_t status = seal(path, enc_key, mac_key, VAULT_PBKDF2_ITERATIONS, salt, blob, sizeof(blob));
	if (status == VAULT_OK) {
		*out = session;
	} else {
		vault_session_lock(session);
	}

	secure_zero(enc_key, sizeof(enc_key));
	secure_zero(mac_key, sizeof(mac_key));
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

	vault_file_t file;
	vault_status_t status = read_file(path, &file);
	if (status != VAULT_OK) {
		return status;
	}

	uint8_t enc_key[32], mac_key[32];
	derive_keys(pin, file.salt, file.iterations, enc_key, mac_key);

	uint8_t *plain = NULL;
	size_t plain_len = 0;
	status = verify_and_decrypt(&file, mac_key, enc_key, &plain, &plain_len);
	file_clear(&file);
	if (status != VAULT_OK) {
		secure_zero(enc_key, sizeof(enc_key));
		secure_zero(mac_key, sizeof(mac_key));
		return status;
	}

	vault_entry *entries = NULL;
	size_t count = 0;
	status = blob_parse(file.version, plain, plain_len, &entries, &count);
	secure_zero(plain, plain_len);
	free(plain);
	if (status != VAULT_OK) {
		entries_free(entries, count);
		secure_zero(enc_key, sizeof(enc_key));
		secure_zero(mac_key, sizeof(mac_key));
		return status;
	}

	vault_session *session =
		session_new(path, entries, count, enc_key, mac_key, file.salt, file.iterations);
	secure_zero(enc_key, sizeof(enc_key));
	secure_zero(mac_key, sizeof(mac_key));
	if (!session) {
		entries_free(entries, count);
		return VAULT_ERR_NOMEM;
	}
	*out = session;
	return VAULT_OK;
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

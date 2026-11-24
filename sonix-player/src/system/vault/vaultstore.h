#ifndef SYSTEM_VAULT_VAULTSTORE_H
#define SYSTEM_VAULT_VAULTSTORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The pocket vault: a list of titled passwords, locked behind a PIN and
// encrypted with AES-256 before it reaches the card.
//
// The shape is Rockbox's Keybox, brought to the V1: one opaque file in `Vault`
// on the microSD, no account, no network, nothing in the device's settings that
// could be used to open it. Inside the file is an entry table -- a title and a
// secret per entry -- because one growing block of text stops being readable
// after the fifth password. The PIN is never stored: the file holds only what
// is needed to check a PIN by trying it, a random salt, the PBKDF2 iteration
// count, a random counter and the ciphertext.
//
// Cryptography:
//
//   keys  = PBKDF2-HMAC-SHA256(PIN, salt, iterations, 64)
//           enc_key = keys[0..31], mac_key = keys[32..63]
//   file  = header || AES-256-CTR(enc_key, nonce, table) || tag
//   tag   = HMAC-SHA256(mac_key, header || ciphertext)
//
// The tag is checked before anything is decrypted, so a wrong PIN, a flipped
// byte and a file cut short by a card pulled out all look the same from here:
// the vault does not open. The header is authenticated too, so the iteration
// count and salt cannot be changed underneath the ciphertext.
//
// The table is length-prefixed rather than text, so a password may hold any
// UTF-8, newlines included, without quoting rules. A title is one line: it has
// no newlines, because it is one row in the list.
//
//   "VLT2"                                     4 bytes
//   entry count                                4 bytes, big endian
//   per entry: title length, secret length,    4 + 4 bytes, big endian
//              title bytes, secret bytes
//
// Version 1 of the file held one block of text. It still opens: the text is
// shown as an entry called "Vault", and the next save writes the table.
//
// This file is plain C with no LVGL, so the format and the failure cases can be
// tested without a screen (tests/test_vault.c).

#define VAULT_FOLDER "Vault"
#define VAULT_FILE "vault.kbx"

// A shorter PIN is a four-digit PIN by another name; a longer one is beyond
// what anybody types on a 240-pixel keypad. The PIN is digits only.
#define VAULT_PIN_MIN 4
#define VAULT_PIN_MAX 12

// The list the page draws. Two hundred is the notes shelf's own ceiling and
// more passwords than anybody scrolls on a 240x320 panel; the byte ceiling
// below is what actually stops a vault that is all long seed phrases.
#define VAULT_MAX_ENTRIES 200
#define VAULT_MAX_TITLE 96
#define VAULT_MAX_SECRET (16 * 1024)

// The whole decrypted table, titles and secrets together. 64 KB holds a few
// hundred ordinary passwords; the file itself is this plus 84 bytes of header
// and tag.
#define VAULT_MAX_DATA (64 * 1024)

// Big enough for any card path the rest of the player builds.
#define VAULT_PATH_MAX 1024

// The work between a PIN and its keys. Stored in every file, checked on read,
// and raised only with the old files still readable. A four-digit PIN is not
// made strong by any number here -- see docs/vault.md -- but the cost of each
// guess is what stands between a lost card and the list.
#define VAULT_PBKDF2_ITERATIONS 120000u
#define VAULT_PBKDF2_MIN 1000u
#define VAULT_PBKDF2_MAX 2000000u

typedef enum {
	VAULT_OK = 0,
	VAULT_ERR_ARG,		// the caller passed something that cannot be used
	VAULT_ERR_PIN,		// not 4 to 12 digits
	VAULT_ERR_NO_FILE,	// there is no vault at that path
	VAULT_ERR_EXISTS,	// refusing to write over a vault that is already there
	VAULT_ERR_AUTH,		// wrong PIN, damaged file, or a tag that does not match
	VAULT_ERR_FORMAT,	// not a vault file, or one from a version this build cannot read
	VAULT_ERR_IO,		// the card refused a read or a write
	VAULT_ERR_TOO_LONG, // an entry does not fit, or the table is full
	VAULT_ERR_FULL,		// no room for another entry
	VAULT_ERR_NOMEM,	// no room to hold the file or the entries
	VAULT_ERR_RANDOM,	// /dev/urandom could not be read; refusing to write a weak file
} vault_status_t;

// An open vault: the decrypted entries and the keys it was opened with. Opaque
// so that the secrets have one place to be wiped from (vault_session_lock).
typedef struct vault_session vault_session;

// `<card_root>/Vault`, and `<dir>/<name>`.
bool vault_dir(char *out, size_t size, const char *card_root);
bool vault_path(char *out, size_t size, const char *dir, const char *name);

// Whether a PIN can be used at all: 4 to 12 ASCII digits. No spaces, no
// letters, no leading-plus any of the keypad cannot type.
bool vault_pin_ok(const char *pin);

// Whether there is a vault file at `path` (a regular file, not a directory or
// something that merely has the name).
bool vault_exists(const char *path);

// Creates an empty vault and opens it. Refuses to replace an existing one:
// making a second vault over the first is how a vault is lost.
vault_status_t vault_create(const char *path, const char *pin, vault_session **out);

// Opens an existing vault with `pin`. A wrong PIN and a damaged file both come
// back as VAULT_ERR_AUTH, because from the outside they are the same thing:
// there is no way to tell without the key, and saying which would be telling.
// A version 1 file opens as one entry titled "Vault".
vault_status_t vault_unlock(const char *path, const char *pin, vault_session **out);

// The entries, in the order the page lists them. The pointers stay valid until
// the entry is changed or removed, or the session is locked.
size_t vault_session_count(const vault_session *session);
const char *vault_session_title(const vault_session *session, size_t index);
const char *vault_session_secret(const vault_session *session, size_t index);

// Adds an entry. A title and a secret are both required; the entry is refused
// with VAULT_ERR_ARG when either is empty, VAULT_ERR_TOO_LONG when either is
// past its ceiling or the table would pass its byte ceiling, and VAULT_ERR_FULL
// when the list has no room for another entry.
// `index_out` may be NULL; otherwise it receives the new entry's position.
vault_status_t vault_session_add(vault_session *session, const char *title, const char *secret,
								size_t *index_out);

// Replaces the title and secret of an existing entry. The entry itself keeps
// its place in the list.
vault_status_t vault_session_update(vault_session *session, size_t index, const char *title,
								   const char *secret);

// Removes an entry, shifting the ones after it up.
vault_status_t vault_session_remove(vault_session *session, size_t index);

// Rewrites the vault with the entries as they stand, with a fresh counter from
// /dev/urandom and the same keys. The file is replaced atomically: a card
// pulled out mid-write leaves the old vault, not half a new one.
vault_status_t vault_session_save(vault_session *session);

// Re-encrypts the entries under `new_pin`, with a new salt and a new counter,
// and makes the new keys the session's. The old PIN stops working when this
// returns VAULT_OK.
vault_status_t vault_session_change_pin(vault_session *session, const char *new_pin);

// Wipes the keys and the entries from memory and frees the session. Safe on
// NULL.
void vault_session_lock(vault_session *session);

// Removes the vault file. True when there is no vault afterwards, including
// when there was none to begin with.
bool vault_erase(const char *path);

#endif /* SYSTEM_VAULT_VAULTSTORE_H */

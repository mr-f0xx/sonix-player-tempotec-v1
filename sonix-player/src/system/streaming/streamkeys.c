#include "streamkeys.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build_gen/streamkeys_key.h"
#include "src/system/core/config.h"
#include "src/system/core/sha1.h"

#define STREAMKEYS_MAX_FILE 16384 // mirrored as MAX_FILE in the tool

#define SEAL_MAGIC "SXK1"
#define SEAL_MAGIC_LEN 4
#define SEAL_KEY_LEN 32
#define SEAL_NONCE_LEN 16

// The values stay here for the life of the program: short strings, read once,
// and every caller expects a pointer that does not die under it.
static char qobuz_app_id[64];
static char qobuz_app_secret[128];
static char tidal_client_id[64];
static char tidal_client_secret[128];
static char podcast_key[64];
static char podcast_secret[128];
static char lastfm_key[64];
static char lastfm_secret[64];
static char soundcloud_client_id[64];
static char soundcloud_oauth_token[256];
static char soundcloud_username[128];
static char source_path[256];
static bool loaded_any;
static bool loaded_from_image;

// Not a plain memset: one on a buffer about to be freed may be dropped by the
// compiler.
static void wipe(void *p, size_t n) {
	volatile unsigned char *v = p;
	while (n--) {
		*v++ = 0;
	}
}

static void trim(char *s) {
	char *start = s;
	while (*start == ' ' || *start == '\t') {
		start++;
	}
	if (start != s) {
		memmove(s, start, strlen(start) + 1);
	}
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
		s[--len] = '\0';
	}
}

// A tiny INI reader rather than config.c, for two reasons: this file is
// read-only and must not join what the player rewrites, and a failure here must
// not be able to corrupt device_config.ini.
//
// One line of the INI, already trimmed. `section` carries across lines.
static void parse_line(char *line, char section[32]) {
	if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
		return;
	}

	if (line[0] == '[') {
		char *close = strchr(line, ']');
		if (close) {
			*close = '\0';
			// Clamped to the buffer width: a section name half a kilobyte long
			// is not a section of interest, and the truncation is written out
			// rather than left to happen.
			snprintf(section, 32, "%.31s", line + 1);
			trim(section);
		}
		return;
	}

	char *eq = strchr(line, '=');
	if (!eq) {
		return;
	}
	*eq = '\0';
	char *key = line;
	char *value = eq + 1;
	trim(key);
	trim(value);
	if (value[0] == '\0') {
		return;
	}

	char *dest = NULL;
	size_t dest_size = 0;
	if (strcmp(section, "qobuz") == 0) {
		if (strcmp(key, "app_id") == 0) {
			dest = qobuz_app_id;
			dest_size = sizeof(qobuz_app_id);
		} else if (strcmp(key, "app_secret") == 0) {
			dest = qobuz_app_secret;
			dest_size = sizeof(qobuz_app_secret);
		}
	} else if (strcmp(section, "podcast") == 0) {
		if (strcmp(key, "api_key") == 0) {
			dest = podcast_key;
			dest_size = sizeof(podcast_key);
		} else if (strcmp(key, "api_secret") == 0) {
			dest = podcast_secret;
			dest_size = sizeof(podcast_secret);
		}
	} else if (strcmp(section, "lastfm") == 0) {
		if (strcmp(key, "api_key") == 0) {
			dest = lastfm_key;
			dest_size = sizeof(lastfm_key);
		} else if (strcmp(key, "api_secret") == 0) {
			dest = lastfm_secret;
			dest_size = sizeof(lastfm_secret);
		}
	} else if (strcmp(section, "tidal") == 0) {
		if (strcmp(key, "client_id") == 0) {
			dest = tidal_client_id;
			dest_size = sizeof(tidal_client_id);
		} else if (strcmp(key, "client_secret") == 0) {
			dest = tidal_client_secret;
			dest_size = sizeof(tidal_client_secret);
		}
	} else if (strcmp(section, "soundcloud") == 0) {
		if (strcmp(key, "client_id") == 0) {
			dest = soundcloud_client_id;
			dest_size = sizeof(soundcloud_client_id);
		} else if (strcmp(key, "oauth_token") == 0 || strcmp(key, "token") == 0) {
			dest = soundcloud_oauth_token;
			dest_size = sizeof(soundcloud_oauth_token);
		} else if (strcmp(key, "username") == 0 || strcmp(key, "user") == 0 || strcmp(key, "profile") == 0) {
			dest = soundcloud_username;
			dest_size = sizeof(soundcloud_username);
		}
	}

	if (dest) {
		snprintf(dest, dest_size, "%s", value);
		loaded_any = true;
	}
}

// Each line is copied out and wiped after use, so the text itself is left
// whole for the caller to wipe.
static void parse_text(char *text, size_t len) {
	char section[32] = "";
	char *p = text;
	char *end = text + len;
	while (p < end) {
		char *nl = memchr(p, '\n', (size_t)(end - p));
		char *stop = nl ? nl : end;
		char line[512];
		size_t n = (size_t)(stop - p);
		if (n >= sizeof(line)) {
			n = sizeof(line) - 1;
		}
		memcpy(line, p, n);
		line[n] = '\0';
		trim(line);
		parse_line(line, section);
		wipe(line, sizeof(line));
		p = stop + 1;
	}
}

#if STREAMKEYS_KEY_LEN == SEAL_KEY_LEN
// SHA1(key | nonce | block as 32-bit big-endian), XORed over `buf`.
static void apply_keystream(const unsigned char *key, const unsigned char *nonce, unsigned char *buf,
							size_t len) {
	unsigned char in[SEAL_KEY_LEN + SEAL_NONCE_LEN + 4];
	unsigned char block[SHA1_DIGEST_LEN];
	memcpy(in, key, SEAL_KEY_LEN);
	memcpy(in + SEAL_KEY_LEN, nonce, SEAL_NONCE_LEN);
	for (size_t off = 0, i = 0; off < len; off += SHA1_DIGEST_LEN, i++) {
		unsigned char *ctr = in + SEAL_KEY_LEN + SEAL_NONCE_LEN;
		ctr[0] = (unsigned char)(i >> 24);
		ctr[1] = (unsigned char)(i >> 16);
		ctr[2] = (unsigned char)(i >> 8);
		ctr[3] = (unsigned char)i;
		sha1(in, sizeof(in), block);
		size_t n = len - off < SHA1_DIGEST_LEN ? len - off : SHA1_DIGEST_LEN;
		for (size_t j = 0; j < n; j++) {
			buf[off + j] ^= block[j];
		}
	}
	wipe(in, sizeof(in));
	wipe(block, sizeof(block));
}
#endif

// The sealed form, laid out in tools/seal_streamkeys.py. False, with the reason
// in the log, when this build cannot open it.
static bool parse_sealed(const char *path, unsigned char *data, size_t len) {
#if STREAMKEYS_KEY_LEN == SEAL_KEY_LEN
	const size_t head = SEAL_MAGIC_LEN + SEAL_NONCE_LEN + SHA1_DIGEST_LEN;
	if (len < head) {
		printf("streamkeys: %s is cut short\n", path);
		return false;
	}
	const unsigned char *nonce = data + SEAL_MAGIC_LEN;
	const unsigned char *tag = nonce + SEAL_NONCE_LEN;
	unsigned char *text = data + head;
	size_t text_len = len - head;

	apply_keystream(streamkeys_key, nonce, text, text_len);

	unsigned char check[SHA1_DIGEST_LEN];
	unsigned char *signed_part = malloc(SEAL_KEY_LEN + SEAL_NONCE_LEN + text_len);
	if (!signed_part) {
		return false;
	}
	memcpy(signed_part, streamkeys_key, SEAL_KEY_LEN);
	memcpy(signed_part + SEAL_KEY_LEN, nonce, SEAL_NONCE_LEN);
	memcpy(signed_part + SEAL_KEY_LEN + SEAL_NONCE_LEN, text, text_len);
	sha1(signed_part, SEAL_KEY_LEN + SEAL_NONCE_LEN + text_len, check);
	wipe(signed_part, SEAL_KEY_LEN + SEAL_NONCE_LEN + text_len);
	free(signed_part);

	if (memcmp(check, tag, SHA1_DIGEST_LEN) != 0) {
		printf("streamkeys: %s was sealed with a key this build does not have\n", path);
		return false;
	}
	parse_text((char *)text, text_len);
	return true;
#else
	(void)data;
	(void)len;
	printf("streamkeys: %s is sealed and this build has no key to open it\n", path);
	return false;
#endif
}

// Reads `path`, sealed or plain. False when it is not there.
static bool load(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	unsigned char *data = malloc(STREAMKEYS_MAX_FILE + 1);
	if (!data) {
		fclose(f);
		return false;
	}
	size_t len = fread(data, 1, STREAMKEYS_MAX_FILE + 1, f);
	fclose(f);
	if (len > STREAMKEYS_MAX_FILE) {
		printf("streamkeys: %s is over %d bytes, ignored\n", path, STREAMKEYS_MAX_FILE);
		free(data);
		return true;
	}

	if (len >= SEAL_MAGIC_LEN && memcmp(data, SEAL_MAGIC, SEAL_MAGIC_LEN) == 0) {
		parse_sealed(path, data, len);
	} else {
		parse_text((char *)data, len);
	}
	wipe(data, len);
	free(data);
	return true;
}

// The log records that the keys are present, never their values: the log
// lands on the card and from there goes to whoever asks for it about any
// problem at all.
static void report(const char *path) {
	snprintf(source_path, sizeof(source_path), "%s", path);

	printf("streamkeys: from %s -- Qobuz %s, Tidal %s, Podcast %s, Last.fm %s\n", path,
		   qobuz_app_id[0] && qobuz_app_secret[0] ? "yes" : "no",
		   tidal_client_id[0] && tidal_client_secret[0] ? "yes" : "no",
		   podcast_key[0] && podcast_secret[0] ? "yes" : "no",
		   lastfm_key[0] && lastfm_secret[0] ? "yes" : "no");
}

const char *streamkeys_data_path(void) {
	const char *env = getenv("SONIX_STREAMKEYS_DATA");
	if (env && env[0]) {
		return env;
	}
	return STREAMKEYS_DATA_DEFAULT_PATH;
}

void streamkeys_init(void) {
	const char *configured = config_get("streaming", "keys_file", NULL);
	const char *path = NULL;
	loaded_from_image = false;
	if (configured && configured[0]) {
		if (load(configured) && loaded_any) {
			path = configured;
			loaded_from_image = true;
		}
	} else if (load(STREAMKEYS_PATH) && loaded_any) {
		path = STREAMKEYS_PATH;
		loaded_from_image = true;
	} else if (load(STREAMKEYS_PLAIN_PATH) && loaded_any) {
		path = STREAMKEYS_PLAIN_PATH;
		loaded_from_image = true;
	} else {
		const char *data_ini = streamkeys_data_path();
		if (data_ini && data_ini[0] && load(data_ini) && loaded_any) {
			path = data_ini;
		}
	}

	if (!path) {
		printf("streamkeys: no %s, Tidal, Qobuz, podcasts and Last.fm stay off\n",
			   configured && configured[0] ? configured : STREAMKEYS_PATH);
		return;
	}

	report(path);
}

bool streamkeys_load_card(const char *card_root) {
	// A firmware that carries its own keys in the rootfs keeps them. The card
	// is a way in for builds that ship none, not a way to replace what an image
	// says. When /usr/data/streaming-keys.ini holds on-device extracted pairs,
	// the card can still supply [podcast]/[lastfm] or updated pairs.
	if (loaded_from_image) {
		return false;
	}
	if (!card_root || !card_root[0]) {
		return false;
	}

	char prev_qid[sizeof(qobuz_app_id)], prev_qsec[sizeof(qobuz_app_secret)];
	char prev_tid[sizeof(tidal_client_id)], prev_tsec[sizeof(tidal_client_secret)];
	char prev_pk[sizeof(podcast_key)], prev_ps[sizeof(podcast_secret)];
	char prev_lk[sizeof(lastfm_key)], prev_ls[sizeof(lastfm_secret)];
	char prev_sc_cid[sizeof(soundcloud_client_id)];
	char prev_sc_tok[sizeof(soundcloud_oauth_token)];
	char prev_sc_usr[sizeof(soundcloud_username)];
	memcpy(prev_qid, qobuz_app_id, sizeof(prev_qid));
	memcpy(prev_qsec, qobuz_app_secret, sizeof(prev_qsec));
	memcpy(prev_tid, tidal_client_id, sizeof(prev_tid));
	memcpy(prev_tsec, tidal_client_secret, sizeof(prev_tsec));
	memcpy(prev_pk, podcast_key, sizeof(prev_pk));
	memcpy(prev_ps, podcast_secret, sizeof(prev_ps));
	memcpy(prev_lk, lastfm_key, sizeof(prev_lk));
	memcpy(prev_ls, lastfm_secret, sizeof(prev_ls));
	memcpy(prev_sc_cid, soundcloud_client_id, sizeof(prev_sc_cid));
	memcpy(prev_sc_tok, soundcloud_oauth_token, sizeof(prev_sc_tok));
	memcpy(prev_sc_usr, soundcloud_username, sizeof(prev_sc_usr));

	char path[512];
	const char *const names[] = {STREAMKEYS_CARD_BIN, STREAMKEYS_CARD_INI};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", card_root, names[i]);
		if (load(path)) {
			// A .bin sealed with another key reads as a file and yields
			// nothing; the .ini beside it, if there is one, still gets its
			// turn.
			bool changed = strcmp(prev_qid, qobuz_app_id) != 0 || strcmp(prev_qsec, qobuz_app_secret) != 0 ||
						   strcmp(prev_tid, tidal_client_id) != 0 || strcmp(prev_tsec, tidal_client_secret) != 0 ||
						   strcmp(prev_pk, podcast_key) != 0 || strcmp(prev_ps, podcast_secret) != 0 ||
						   strcmp(prev_lk, lastfm_key) != 0 || strcmp(prev_ls, lastfm_secret) != 0 ||
						   strcmp(prev_sc_cid, soundcloud_client_id) != 0 ||
						   strcmp(prev_sc_tok, soundcloud_oauth_token) != 0 ||
						   strcmp(prev_sc_usr, soundcloud_username) != 0;
			if (!loaded_any) {
				continue;
			}
			wipe(prev_qid, sizeof(prev_qid));
			wipe(prev_qsec, sizeof(prev_qsec));
			wipe(prev_tid, sizeof(prev_tid));
			wipe(prev_tsec, sizeof(prev_tsec));
			wipe(prev_pk, sizeof(prev_pk));
			wipe(prev_ps, sizeof(prev_ps));
			wipe(prev_lk, sizeof(prev_lk));
			wipe(prev_ls, sizeof(prev_ls));
			wipe(prev_sc_cid, sizeof(prev_sc_cid));
			wipe(prev_sc_tok, sizeof(prev_sc_tok));
			wipe(prev_sc_usr, sizeof(prev_sc_usr));
			if (changed) {
				printf("streamkeys: the microSD card has them\n");
				report(path);
			}
			return changed;
		}
	}
	wipe(prev_qid, sizeof(prev_qid));
	wipe(prev_qsec, sizeof(prev_qsec));
	wipe(prev_tid, sizeof(prev_tid));
	wipe(prev_tsec, sizeof(prev_tsec));
	wipe(prev_pk, sizeof(prev_pk));
	wipe(prev_ps, sizeof(prev_ps));
	wipe(prev_lk, sizeof(prev_lk));
	wipe(prev_ls, sizeof(prev_ls));
	wipe(prev_sc_cid, sizeof(prev_sc_cid));
	wipe(prev_sc_tok, sizeof(prev_sc_tok));
	wipe(prev_sc_usr, sizeof(prev_sc_usr));
	return false;
}

bool streamkeys_reload(const char *card_root) {
	if (loaded_from_image) {
		return true;
	}
	wipe(qobuz_app_id, sizeof(qobuz_app_id));
	wipe(qobuz_app_secret, sizeof(qobuz_app_secret));
	wipe(tidal_client_id, sizeof(tidal_client_id));
	wipe(tidal_client_secret, sizeof(tidal_client_secret));
	wipe(podcast_key, sizeof(podcast_key));
	wipe(podcast_secret, sizeof(podcast_secret));
	wipe(lastfm_key, sizeof(lastfm_key));
	wipe(lastfm_secret, sizeof(lastfm_secret));
	wipe(soundcloud_client_id, sizeof(soundcloud_client_id));
	wipe(soundcloud_oauth_token, sizeof(soundcloud_oauth_token));
	wipe(soundcloud_username, sizeof(soundcloud_username));
	source_path[0] = '\0';
	loaded_any = false;

	streamkeys_init();
	(void)streamkeys_load_card(card_root);
	return loaded_any;
}

static void ensure_parent_dir(const char *path) {
	char dir[512];
	const char *slash = strrchr(path, '/');
	if (!slash || slash == path) {
		return;
	}
	size_t len = (size_t)(slash - path);
	if (len >= sizeof(dir)) {
		return;
	}
	memcpy(dir, path, len);
	dir[len] = '\0';
	for (char *p = dir + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			(void)mkdir(dir, 0755);
			*p = '/';
		}
	}
	(void)mkdir(dir, 0755);
}

static bool write_ini_atomic(const char *dest, const char *content, size_t len) {
	if (!dest || !dest[0]) {
		return false;
	}
	ensure_parent_dir(dest);

	char tmp[540];
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", dest) >= (int)sizeof(tmp)) {
		return false;
	}
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(content, 1, len, f) == len && fflush(f) == 0;
	if (ok) {
		(void)fsync(fileno(f));
	}
	if (fclose(f) != 0) {
		ok = false;
	}
	if (!ok) {
		unlink(tmp);
		return false;
	}
	(void)chmod(tmp, 0600);
	if (rename(tmp, dest) != 0) {
		// On FAT/exFAT some kernels refuse replacing an existing destination
		// unless it is unlinked first.
		(void)unlink(dest);
		if (rename(tmp, dest) != 0) {
			unlink(tmp);
			return false;
		}
	}
	return true;
}

bool streamkeys_write_extracted(const char *qobuz_id, const char *qobuz_sec, const char *tidal_id,
								const char *tidal_sec, const char *card_root) {
	bool have_qobuz = qobuz_id && qobuz_id[0] && qobuz_sec && qobuz_sec[0];
	bool have_tidal = tidal_id && tidal_id[0] && tidal_sec && tidal_sec[0];
	if (!have_qobuz && !have_tidal) {
		return false;
	}

	// Preserve any [podcast] / [lastfm] keys already loaded or sitting in either
	// existing INI file before we rewrite them.
	if (!loaded_from_image) {
		const char *data_ini = streamkeys_data_path();
		if (data_ini && data_ini[0]) {
			(void)load(data_ini);
		}
		if (card_root && card_root[0]) {
			char card_ini[512];
			snprintf(card_ini, sizeof(card_ini), "%s/%s", card_root, STREAMKEYS_CARD_INI);
			(void)load(card_ini);
		}
	}

	const char *qid = have_qobuz ? qobuz_id : qobuz_app_id;
	const char *qsec = have_qobuz ? qobuz_sec : qobuz_app_secret;
	const char *tid = have_tidal ? tidal_id : tidal_client_id;
	const char *tsec = have_tidal ? tidal_sec : tidal_client_secret;

	char buf[2048];
	int pos = snprintf(buf, sizeof(buf),
					   "# Streaming service credentials, extracted from the stock firmware of\n"
					   "# this device. Do not commit or share this file, and do not publish an\n"
					   "# image that carries it: these pairs belong to HiBy, not to Sonix.\n\n");
	if (pos < 0 || (size_t)pos >= sizeof(buf)) {
		return false;
	}

	if (qid[0] && qsec[0]) {
		int n = snprintf(buf + pos, sizeof(buf) - (size_t)pos,
						 "[qobuz]\napp_id = %s\napp_secret = %s\n\n", qid, qsec);
		if (n < 0 || (size_t)(pos + n) >= sizeof(buf)) {
			wipe(buf, sizeof(buf));
			return false;
		}
		pos += n;
	}
	if (tid[0] && tsec[0]) {
		int n = snprintf(buf + pos, sizeof(buf) - (size_t)pos,
						 "[tidal]\nclient_id = %s\nclient_secret = %s\n\n", tid, tsec);
		if (n < 0 || (size_t)(pos + n) >= sizeof(buf)) {
			wipe(buf, sizeof(buf));
			return false;
		}
		pos += n;
	}
	if (podcast_key[0] && podcast_secret[0]) {
		int n = snprintf(buf + pos, sizeof(buf) - (size_t)pos,
						 "[podcast]\napi_key = %s\napi_secret = %s\n\n", podcast_key, podcast_secret);
		if (n < 0 || (size_t)(pos + n) >= sizeof(buf)) {
			wipe(buf, sizeof(buf));
			return false;
		}
		pos += n;
	}
	if (lastfm_key[0] && lastfm_secret[0]) {
		int n = snprintf(buf + pos, sizeof(buf) - (size_t)pos,
						 "[lastfm]\napi_key = %s\napi_secret = %s\n\n", lastfm_key, lastfm_secret);
		if (n < 0 || (size_t)(pos + n) >= sizeof(buf)) {
			wipe(buf, sizeof(buf));
			return false;
		}
		pos += n;
	}
	if (soundcloud_client_id[0] || soundcloud_oauth_token[0] || soundcloud_username[0]) {
		int n = snprintf(buf + pos, sizeof(buf) - (size_t)pos, "[soundcloud]\n");
		if (n < 0 || (size_t)(pos + n) >= sizeof(buf)) {
			wipe(buf, sizeof(buf));
			return false;
		}
		pos += n;
		if (soundcloud_client_id[0]) {
			n = snprintf(buf + pos, sizeof(buf) - (size_t)pos, "client_id = %s\n", soundcloud_client_id);
			if (n < 0 || (size_t)(pos + n) >= sizeof(buf)) {
				wipe(buf, sizeof(buf));
				return false;
			}
			pos += n;
		}
		if (soundcloud_username[0]) {
			n = snprintf(buf + pos, sizeof(buf) - (size_t)pos, "username = %s\n", soundcloud_username);
			if (n < 0 || (size_t)(pos + n) >= sizeof(buf)) {
				wipe(buf, sizeof(buf));
				return false;
			}
			pos += n;
		}
		if (soundcloud_oauth_token[0]) {
			n = snprintf(buf + pos, sizeof(buf) - (size_t)pos, "oauth_token = %s\n", soundcloud_oauth_token);
			if (n < 0 || (size_t)(pos + n) >= sizeof(buf)) {
				wipe(buf, sizeof(buf));
				return false;
			}
			pos += n;
		}
		n = snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\n");
		if (n < 0 || (size_t)(pos + n) >= sizeof(buf)) {
			wipe(buf, sizeof(buf));
			return false;
		}
		pos += n;
	}

	bool wrote_data = false;
	const char *data_ini = streamkeys_data_path();
	if (data_ini && data_ini[0]) {
		wrote_data = write_ini_atomic(data_ini, buf, (size_t)pos);
		if (wrote_data) {
			printf("streamkeys: saved extracted keys to %s\n", data_ini);
		}
	}

	bool wrote_card = false;
	if (card_root && card_root[0]) {
		char card_ini[512];
		if (snprintf(card_ini, sizeof(card_ini), "%s/%s", card_root, STREAMKEYS_CARD_INI) < (int)sizeof(card_ini)) {
			wrote_card = write_ini_atomic(card_ini, buf, (size_t)pos);
			if (wrote_card) {
				printf("streamkeys: saved extracted keys to %s\n", card_ini);
			}
		}
	}

	wipe(buf, sizeof(buf));
	return wrote_data || wrote_card;
}

static const char *or_null(const char *s) { return s[0] ? s : NULL; }

const char *streamkeys_qobuz_app_id(void) { return or_null(qobuz_app_id); }
const char *streamkeys_qobuz_app_secret(void) { return or_null(qobuz_app_secret); }
const char *streamkeys_tidal_client_id(void) { return or_null(tidal_client_id); }
const char *streamkeys_tidal_client_secret(void) { return or_null(tidal_client_secret); }

const char *streamkeys_podcast_key(void) { return or_null(podcast_key); }
const char *streamkeys_podcast_secret(void) { return or_null(podcast_secret); }

const char *streamkeys_lastfm_key(void) { return or_null(lastfm_key); }
const char *streamkeys_lastfm_secret(void) { return or_null(lastfm_secret); }

const char *streamkeys_soundcloud_client_id(void) { return or_null(soundcloud_client_id); }
const char *streamkeys_soundcloud_oauth_token(void) { return or_null(soundcloud_oauth_token); }
const char *streamkeys_soundcloud_username(void) { return or_null(soundcloud_username); }

const char *streamkeys_source(void) { return loaded_any ? source_path : NULL; }

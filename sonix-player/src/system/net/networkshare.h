#ifndef NETWORKSHARE_H
#define NETWORKSHARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Outbound, read-only access to a user-configured music share. A folder is a
// decoded absolute path (with a trailing slash), relative to the URL host.
// FTP paths are relative to the login directory, as in a normal FTP URL.
#define SHARE_ADDRESS_MAX 1024
#define SHARE_FOLDER_MAX 768
#define SHARE_NAME_MAX 256
#define SHARE_LOGIN_MAX 128
#define SHARE_LIST_MAX 384

typedef enum { SHARE_WEBDAV, SHARE_FTP } share_kind_t;

typedef struct {
	share_kind_t kind;
	char origin[272]; // scheme + authority; never contains credentials
	char root[SHARE_FOLDER_MAX];
	char username[SHARE_LOGIN_MAX];
	char password[SHARE_LOGIN_MAX];
	char ca_file[512]; // optional CA bundle for secure URLs; never bypass verification
} share_server_t;

typedef struct {
	char name[SHARE_NAME_MAX];
	bool directory;
} share_entry_t;

typedef struct {
	share_entry_t entries[SHARE_LIST_MAX];
	size_t count;
	bool truncated;
} share_listing_t;

typedef enum {
	SHARE_OK,
	SHARE_BAD_ADDRESS,
	SHARE_UNAVAILABLE,
	SHARE_NETWORK_ERROR,
	SHARE_TIMEOUT,
	SHARE_AUTH_ERROR,
	SHARE_CERT_ERROR,
	SHARE_SERVER_ERROR,
	SHARE_BAD_LIST,
	SHARE_LIST_TOO_LARGE,
	SHARE_CANCELLED,
	SHARE_NO_CARD,
	SHARE_DISK_ERROR,
	SHARE_CARD_FULL,
	SHARE_FILE_TOO_LARGE,
	SHARE_PATH_TOO_LONG,
	SHARE_EXISTS,
	SHARE_BUSY,
	SHARE_NO_MEMORY,
} share_status_t;

typedef bool (*share_cancel_fn)(void *user);
typedef share_status_t (*share_write_fn)(void *user, const void *data, size_t size);
typedef void (*share_size_fn)(void *user, uint64_t size);

// Address is required, with a protocol appropriate to `kind`. A path may
// contain raw UTF-8, spaces and/or URL escapes. Query, fragment and userinfo
// are not accepted. Passwords are passed to libcurl separately from URLs.
bool networkshare_server_init(share_server_t *out, share_kind_t kind, const char *address,
							  const char *username, const char *password, const char *ca_file);
bool networkshare_valid_folder(const share_server_t *server, const char *folder);
bool networkshare_valid_track(const char *name);
bool networkshare_child_folder(const share_server_t *server, const char *folder, const char *name,
							  char *out, size_t size);

// Synchronous I/O on the caller's worker. No redirects, external XML entity
// resolution, or FTP data connection to an address supplied by PASV.
share_status_t networkshare_list(const share_server_t *server, const char *folder, share_listing_t *out,
								 share_cancel_fn cancel, void *user);
share_status_t networkshare_fetch(const share_server_t *server, const char *folder, const char *name,
								  share_cancel_fn cancel, share_write_fn write, share_size_fn content_size, void *user);

#endif

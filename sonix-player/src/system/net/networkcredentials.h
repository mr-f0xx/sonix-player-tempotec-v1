#ifndef NETWORKCREDENTIALS_H
#define NETWORKCREDENTIALS_H

#include <stdbool.h>
#include <stddef.h>
#include "src/system/net/networkshare.h"

// Remembered passwords for Network folders. The address and username live in
// device_config.ini as today; the password is kept only when the user asked
// for it, in a private JSON file next to the config file. The file is written
// atomically with mode 0600. It is unencrypted — the player is a small shared
// device and the settings page says exactly that — and is never written for
// public shares.

// Loads the file (missing or unreadable means nothing remembered). Called once
// after config_init().
void networkcredentials_init(void);

// Copies the stored password for `kind` into `out` ("" when none). `remembered`
// receives whether a password is stored for that kind.
void networkcredentials_get(share_kind_t kind, char *out, size_t size, bool *remembered);

// remember == true: stores the password for `kind`, overwriting any stored one.
// remember == false: removes the stored password for that kind, leaving the
// other protocol's entry alone. An empty password always removes. Returns false
// when the file could not be written or removed, and writes the reason into
// `error` when non-NULL.
bool networkcredentials_apply(share_kind_t kind, const char *password, bool remember,
						  char *error, size_t error_size);

#endif

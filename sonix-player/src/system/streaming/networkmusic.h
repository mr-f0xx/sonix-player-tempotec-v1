#ifndef NETWORKMUSIC_H
#define NETWORKMUSIC_H

#include "src/system/net/networkshare.h"

// One background listing or download per player. Start copies the arguments;
// take_result transfers ownership of `listing` to the caller (free it).
#define NETWORKMUSIC_LOCAL_PATH 512

typedef struct {
	uint64_t bytes_done;
	uint64_t bytes_total; // zero if not yet known
	size_t completed, total; // completed includes files already on the card
} networkmusic_progress_t;

typedef struct {
	share_status_t status;
	share_listing_t *listing;
	size_t saved, existing;
	char path[NETWORKMUSIC_LOCAL_PATH]; // local path after a single-file success
} networkmusic_result_t;

share_status_t networkmusic_list_start(const share_server_t *server, const char *folder);
// Only audio entries in `entries` are saved, in order. No recursion. Existing
// files are skipped, and the first transfer error stops the remaining batch.
share_status_t networkmusic_save_start(const share_server_t *server, const char *folder,
									  const share_entry_t *entries, size_t count);
void networkmusic_progress(networkmusic_progress_t *out);
bool networkmusic_busy(void);
bool networkmusic_take_result(networkmusic_result_t *out);
void networkmusic_cancel(void);
// Synchronous card handoff: cancels the download, removes its unfinished file
// and closes its SD descriptors without waiting on the network/resolver.
void networkmusic_release_card(void);

#endif

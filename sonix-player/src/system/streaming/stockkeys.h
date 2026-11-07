#ifndef STOCKKEYS_H
#define STOCKKEYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Local extraction of Qobuz and Tidal application credentials from the
// official TempoTec V1 stock firmware, directly on the player.
//
// Neither this repository nor any published v1.upt release image ships HiBy's
// application credentials. Instead, the player can extract them locally on the
// device itself:
//
//   1. Offline from the microSD card, when the user has placed a stock
//      firmware package (v1_stock.upt, v1_original.upt, v1.upt, v1.zip,
//      v1.zip.part1+.part2, or hiby_player, at the card root or in stock/).
//   2. One-tap over Wi-Fi, streaming and inflating the official stock 1.3
//      archive (sonix-packer/firmware-input/v1.zip.part1 + .part2) from the
//      repository into a temporary file, extracting usr/bin/hiby_player out of
//      the ISO 9660 + SquashFS 4.0 (LZO1X/zlib) rootfs, removing the temporary
//      file, and saving streaming-keys.ini to /usr/data/streaming-keys.ini and
//      (when a writable card is mounted) <card_root>/streaming-keys.ini.
//
// Nothing here touches the GUI: see src/gui/streaming/stockkeys_ui.h.
// ---------------------------------------------------------------------------

typedef struct {
	char qobuz_app_id[64];
	char qobuz_app_secret[128];
	char tidal_client_id[64];
	char tidal_client_secret[128];
	bool has_qobuz;
	bool has_tidal;
} stockkeys_pairs_t;

typedef enum {
	STOCKKEYS_IDLE = 0,
	STOCKKEYS_RUNNING,
	STOCKKEYS_FINISHED,
} stockkeys_state_t;

typedef enum {
	STOCKKEYS_PHASE_SCANNING = 0,
	STOCKKEYS_PHASE_DOWNLOADING,
	STOCKKEYS_PHASE_EXTRACTING,
} stockkeys_phase_t;

typedef enum {
	STOCKKEYS_OK = 0,
	STOCKKEYS_CANCELLED,
	STOCKKEYS_NO_SOURCE_OFFLINE, // nothing on the card and Wi-Fi is disconnected
	STOCKKEYS_NO_SPACE,          // < 48 MB free on both the card and /usr/data
	STOCKKEYS_DOWNLOAD_FAILED,
	STOCKKEYS_EXTRACT_FAILED,
	STOCKKEYS_SAVE_FAILED,
} stockkeys_result_t;

// Safe LZO1X block decompressor used for SquashFS compression=3 blocks.
bool stockkeys_lzo1x_decompress(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap, size_t *out_len);

// Extracts [qobuz] and/or [tidal] pairs from an in-memory hiby_player ELF.
// Never guesses on ties: returns true only when at least one service pair was
// identified unambiguously.
bool stockkeys_extract_from_elf(const uint8_t *buf, size_t len, stockkeys_pairs_t *out);

// Extracts [qobuz] and/or [tidal] pairs from a file on disk: raw hiby_player,
// SquashFS 4.0 rootfs, ISO 9660 .upt, .zip, or split .zip.part1 (+ .part2).
// `temp_dir` provides scratch space when a .zip must be inflated (may be NULL
// to use the file's own directory).
bool stockkeys_extract_from_file(const char *path, const char *temp_dir, stockkeys_pairs_t *out);

// True when any recognised stock firmware file or directory entry exists on
// the card (fast filename check only, safe to call on the UI thread).
bool stockkeys_has_sd_candidate(const char *card_root);

// Fast boot-time check: if explicit stock files (hiby_player, v1_stock.upt,
// v1_original.upt, v1_stock.zip, v1.zip.part1, or stock/*) sit on the card and
// credentials are still missing, extracts and writes them synchronously.
// Returns true when new keys were saved and loaded into streamkeys.
bool stockkeys_try_boot_card(const char *card_root);

// Starts the background worker (card scan -> optional Wi-Fi fetch -> extract ->
// save to /usr/data + card). False if already running.
bool stockkeys_start(void);
void stockkeys_cancel(void);

stockkeys_state_t stockkeys_state(void);
stockkeys_phase_t stockkeys_phase(void);
stockkeys_result_t stockkeys_result(void);
void stockkeys_progress(long *done, long *total);
const char *stockkeys_error_detail(void);
void stockkeys_acknowledge(void);
bool stockkeys_busy(void);

#endif /* STOCKKEYS_H */

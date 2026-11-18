#ifndef LISTENTOGETHER_H
#define LISTENTOGETHER_H

#include <stdbool.h>
#include <stdint.h>

#define LT_AUDIO_PORT     7788
#define LT_CTRL_PORT      7789
#define LT_MULTICAST_ADDR "239.255.77.88"
#define LT_MAX_HOSTS      8
#define LT_PACKET_FRAMES  256
#define LT_TARGET_BUF_MS  100

typedef enum {
	LT_MODE_OFF = 0,
	LT_MODE_HOST,
	LT_MODE_CLIENT,
} lt_mode_t;

typedef struct {
	char name[64];
	char ip[46];
	uint16_t port;
	uint32_t sample_rate;
	uint64_t last_seen_ms;
} lt_host_info_t;

typedef struct {
	lt_mode_t mode;
	int buffer_ms;
	int target_ms;
	int drift_ppm;
	uint64_t packets_tx;
	uint64_t packets_rx;
	uint64_t packets_lost;
	int peer_count;
	int active_rate;
	char peer_ip[46];
} lt_stats_t;

bool lt_start_host(void);
bool lt_start_client(const char *host_ip);
void lt_stop(void);
lt_mode_t lt_get_mode(void);
void lt_get_stats(lt_stats_t *out);
int lt_discover_hosts(lt_host_info_t *out, int max_hosts, int timeout_ms);

/* --- Internal/testable DSP & protocol helpers --- */
#define LT_PKT_MAGIC 0x4C544F47u /* "LTOG" */

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint32_t seq;
	uint64_t timestamp_us;
	uint32_t sample_rate;
	uint8_t channels;
	uint8_t bits;
	uint16_t frames;
} lt_pkt_header_t;

typedef struct {
	double integral;
	double ratio;
	double phase;
	int16_t prev_l;
	int16_t prev_r;
	int target_frames;
} lt_sync_resampler_t;

void lt_sync_resampler_init(lt_sync_resampler_t *r, int target_frames);
double lt_sync_resampler_update_pi(lt_sync_resampler_t *r, int current_fill_frames);
int lt_sync_resampler_process(lt_sync_resampler_t *r, const int16_t *in_stereo, int in_frames, int *in_consumed,
							  int16_t *out_stereo, int max_out_frames);

#endif // LISTENTOGETHER_H

#include "listentogether.h"
#include "src/system/audio/audio.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HOST_RING_FRAMES   16384
#define CLIENT_RING_FRAMES 16384
#define MAX_PEERS          8
#define PEER_TIMEOUT_MS    10000

#define CTRL_MAGIC_DISC 0x4C544431u /* "LTD1" */
#define CTRL_MAGIC_ANN  0x4C544131u /* "LTA1" */
#define CTRL_MAGIC_SUB  0x4C545331u /* "LTS1" */
#define CTRL_MAGIC_UNSB 0x4C545531u /* "LTU1" */

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint16_t port;
	uint32_t sample_rate;
	char name[64];
} lt_ctrl_pkt_t;

typedef struct {
	struct sockaddr_in addr;
	uint64_t last_seen_ms;
	bool active;
} lt_peer_t;

static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static uint64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* --- PI-controlled fractional resampler (zero-drift, zero-stutter) --- */

void lt_sync_resampler_init(lt_sync_resampler_t *r, int target_frames) {
	memset(r, 0, sizeof(*r));
	r->ratio = 1.0;
	r->target_frames = target_frames > 0 ? target_frames : 4410;
}

double lt_sync_resampler_update_pi(lt_sync_resampler_t *r, int current_fill_frames) {
	double err = (double)(current_fill_frames - r->target_frames);
	r->integral += err;
	if (r->integral > 200000.0) {
		r->integral = 200000.0;
	} else if (r->integral < -200000.0) {
		r->integral = -200000.0;
	}
	const double kp = 2.0e-7;
	const double ki = 5.0e-9;
	double ratio = 1.0 + kp * err + ki * r->integral;
	if (ratio > 1.002) {
		ratio = 1.002;
	} else if (ratio < 0.998) {
		ratio = 0.998;
	}
	r->ratio = ratio;
	return ratio;
}

int lt_sync_resampler_process(lt_sync_resampler_t *r, const int16_t *in_stereo, int in_frames, int *in_consumed,
							  int16_t *out_stereo, int max_out_frames) {
	int out_count = 0;
	int idx = 0;

	while (out_count < max_out_frames && idx < in_frames) {
		double frac = r->phase;
		int16_t l0, r0, l1, r1;
		if (idx == 0) {
			l0 = r->prev_l;
			r0 = r->prev_r;
			l1 = in_stereo[0];
			r1 = in_stereo[1];
		} else {
			l0 = in_stereo[(idx - 1) * 2];
			r0 = in_stereo[(idx - 1) * 2 + 1];
			l1 = in_stereo[idx * 2];
			r1 = in_stereo[idx * 2 + 1];
		}

		double out_l = (double)l0 + ((double)l1 - (double)l0) * frac;
		double out_r = (double)r0 + ((double)r1 - (double)r0) * frac;
		if (out_l > 32767.0) out_l = 32767.0;
		if (out_l < -32768.0) out_l = -32768.0;
		if (out_r > 32767.0) out_r = 32767.0;
		if (out_r < -32768.0) out_r = -32768.0;

		out_stereo[out_count * 2] = (int16_t)out_l;
		out_stereo[out_count * 2 + 1] = (int16_t)out_r;
		out_count++;

		r->phase += r->ratio;
		while (r->phase >= 1.0 && idx < in_frames) {
			r->prev_l = in_stereo[idx * 2];
			r->prev_r = in_stereo[idx * 2 + 1];
			idx++;
			r->phase -= 1.0;
		}
	}

	if (in_consumed) {
		*in_consumed = idx;
	}
	return out_count;
}

/* --- Global state --- */

static pthread_mutex_t lt_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lt_cond = PTHREAD_COND_INITIALIZER;
static volatile bool lt_running = false;
static lt_mode_t lt_mode = LT_MODE_OFF;
static pthread_t lt_thread;
static pthread_t lt_ctrl_thread;
static bool lt_threads_started = false;

static int lt_active_rate = 44100;
static char lt_peer_ip[46] = {0};
static uint64_t lt_packets_tx = 0;
static uint64_t lt_packets_rx = 0;
static uint64_t lt_packets_lost = 0;
static int lt_last_buffer_ms = 0;
static int lt_last_drift_ppm = 0;

/* Host ring buffer */
static int16_t host_ring[HOST_RING_FRAMES * 2];
static int host_head = 0;
static int host_tail = 0;
static int host_fill = 0;
static double host_src_phase = 0.0;
static lt_peer_t host_peers[MAX_PEERS];

/* Client ring buffer */
static int16_t client_ring[CLIENT_RING_FRAMES * 2];
static int client_head = 0;
static int client_tail = 0;
static int client_fill = 0;

static int host_target_rate(int src_rate) {
	if (src_rate > 0 && (src_rate % 48000) == 0) {
		return 48000;
	}
	return 44100;
}

static void host_pcm_tap(const void *buf, int frames, int sample_rate, int channels, int bits) {
	if (!lt_running || lt_mode != LT_MODE_HOST || !buf || frames <= 0 || sample_rate <= 0) {
		return;
	}

	int target_rate = host_target_rate(sample_rate);
	double step = (double)sample_rate / (double)target_rate;
	const uint8_t *raw = (const uint8_t *)buf;
	int ch = channels > 1 ? channels : 1;

	pthread_mutex_lock(&lt_lock);
	lt_active_rate = target_rate;

	while (host_src_phase < (double)frames) {
		int i = (int)host_src_phase;
		int16_t l = 0, r = 0;

		if (bits == 16) {
			const int16_t *s = (const int16_t *)raw;
			l = s[i * ch];
			r = (ch >= 2) ? s[i * ch + 1] : l;
		} else if (bits == 24) {
			const uint8_t *p = raw + (size_t)i * (size_t)ch * 3;
			int32_t v0 = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 16;
			l = (int16_t)v0;
			if (ch >= 2) {
				p += 3;
				int32_t v1 = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 16;
				r = (int16_t)v1;
			} else {
				r = l;
			}
		} else if (bits == 32) {
			const int32_t *s = (const int32_t *)raw;
			l = (int16_t)(s[i * ch] >> 16);
			r = (ch >= 2) ? (int16_t)(s[i * ch + 1] >> 16) : l;
		}

		if (host_fill < HOST_RING_FRAMES) {
			host_ring[host_head * 2] = l;
			host_ring[host_head * 2 + 1] = r;
			host_head = (host_head + 1) % HOST_RING_FRAMES;
			host_fill++;
		}
		host_src_phase += step;
	}
	host_src_phase -= (double)frames;

	if (host_fill >= LT_PACKET_FRAMES) {
		pthread_cond_signal(&lt_cond);
	}
	pthread_mutex_unlock(&lt_lock);
}

static void *host_ctrl_worker(void *arg) {
	(void)arg;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return NULL;
	}
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(LT_CTRL_PORT);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return NULL;
	}

	while (lt_running && lt_mode == LT_MODE_HOST) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		if (poll(&pfd, 1, 200) <= 0) {
			continue;
		}
		lt_ctrl_pkt_t pkt;
		struct sockaddr_in src = {0};
		socklen_t slen = sizeof(src);
		ssize_t n = recvfrom(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&src, &slen);
		if (n < (ssize_t)sizeof(uint32_t)) {
			continue;
		}

		if (pkt.magic == CTRL_MAGIC_DISC) {
			lt_ctrl_pkt_t ann = {0};
			ann.magic = CTRL_MAGIC_ANN;
			ann.port = LT_AUDIO_PORT;
			ann.sample_rate = (uint32_t)lt_active_rate;
			gethostname(ann.name, sizeof(ann.name) - 1);
			if (ann.name[0] == '\0') {
				snprintf(ann.name, sizeof(ann.name), "TempoTec V1");
			}
			sendto(fd, &ann, sizeof(ann), 0, (struct sockaddr *)&src, slen);
		} else if (pkt.magic == CTRL_MAGIC_SUB) {
			uint64_t t = now_ms();
			pthread_mutex_lock(&lt_lock);
			int free_idx = -1;
			int found = -1;
			for (int i = 0; i < MAX_PEERS; i++) {
				if (host_peers[i].active && host_peers[i].addr.sin_addr.s_addr == src.sin_addr.s_addr) {
					found = i;
					break;
				}
				if (!host_peers[i].active && free_idx < 0) {
					free_idx = i;
				}
			}
			int idx = found >= 0 ? found : free_idx;
			if (idx >= 0) {
				host_peers[idx].addr = src;
				host_peers[idx].addr.sin_port = htons(LT_AUDIO_PORT);
				host_peers[idx].last_seen_ms = t;
				host_peers[idx].active = true;
			}
			pthread_mutex_unlock(&lt_lock);
		} else if (pkt.magic == CTRL_MAGIC_UNSB) {
			pthread_mutex_lock(&lt_lock);
			for (int i = 0; i < MAX_PEERS; i++) {
				if (host_peers[i].active && host_peers[i].addr.sin_addr.s_addr == src.sin_addr.s_addr) {
					host_peers[i].active = false;
				}
			}
			pthread_mutex_unlock(&lt_lock);
		}
	}

	close(fd);
	return NULL;
}

static void *host_tx_worker(void *arg) {
	(void)arg;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return NULL;
	}
	int ttl = 1;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

	struct sockaddr_in mcast = {0};
	mcast.sin_family = AF_INET;
	mcast.sin_port = htons(LT_AUDIO_PORT);
	inet_pton(AF_INET, LT_MULTICAST_ADDR, &mcast.sin_addr);

	uint32_t seq = 0;
	uint8_t pkt_buf[sizeof(lt_pkt_header_t) + LT_PACKET_FRAMES * 4];

	while (lt_running && lt_mode == LT_MODE_HOST) {
		pthread_mutex_lock(&lt_lock);
		while (lt_running && lt_mode == LT_MODE_HOST && host_fill < LT_PACKET_FRAMES) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 50000000;
			if (ts.tv_nsec >= 1000000000) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000;
			}
			pthread_cond_timedwait(&lt_cond, &lt_lock, &ts);
		}
		if (!lt_running || lt_mode != LT_MODE_HOST) {
			pthread_mutex_unlock(&lt_lock);
			break;
		}

		lt_pkt_header_t hdr = {
			.magic = LT_PKT_MAGIC,
			.seq = seq++,
			.timestamp_us = now_us(),
			.sample_rate = (uint32_t)lt_active_rate,
			.channels = 2,
			.bits = 16,
			.frames = LT_PACKET_FRAMES,
		};
		memcpy(pkt_buf, &hdr, sizeof(hdr));
		int16_t *payload = (int16_t *)(pkt_buf + sizeof(hdr));
		for (int i = 0; i < LT_PACKET_FRAMES; i++) {
			payload[i * 2] = host_ring[host_tail * 2];
			payload[i * 2 + 1] = host_ring[host_tail * 2 + 1];
			host_tail = (host_tail + 1) % HOST_RING_FRAMES;
		}
		host_fill -= LT_PACKET_FRAMES;

		lt_peer_t peers_copy[MAX_PEERS];
		memcpy(peers_copy, host_peers, sizeof(peers_copy));
		uint64_t t = now_ms();
		for (int i = 0; i < MAX_PEERS; i++) {
			if (host_peers[i].active && (t - host_peers[i].last_seen_ms) > PEER_TIMEOUT_MS) {
				host_peers[i].active = false;
			}
		}
		pthread_mutex_unlock(&lt_lock);

		size_t pkt_len = sizeof(hdr) + LT_PACKET_FRAMES * 4;
		sendto(fd, pkt_buf, pkt_len, 0, (struct sockaddr *)&mcast, sizeof(mcast));
		for (int i = 0; i < MAX_PEERS; i++) {
			if (peers_copy[i].active) {
				sendto(fd, pkt_buf, pkt_len, 0, (struct sockaddr *)&peers_copy[i].addr, sizeof(peers_copy[i].addr));
			}
		}
		lt_packets_tx++;
	}

	close(fd);
	return NULL;
}

static bool client_release_cb(void) {
	lt_stop();
	return true;
}

static void *client_rx_worker(void *arg) {
	(void)arg;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return NULL;
	}
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(LT_AUDIO_PORT);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return NULL;
	}

	struct ip_mreq mreq = {0};
	inet_pton(AF_INET, LT_MULTICAST_ADDR, &mreq.imr_multiaddr);
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

	int ctrl_fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in host_ctrl = {0};
	bool has_host_ip = (lt_peer_ip[0] != '\0');
	if (has_host_ip) {
		host_ctrl.sin_family = AF_INET;
		host_ctrl.sin_port = htons(LT_CTRL_PORT);
		inet_pton(AF_INET, lt_peer_ip, &host_ctrl.sin_addr);
	}

	uint64_t last_sub_ms = 0;
	uint32_t expected_seq = 0;
	bool seq_init = false;
	bool audio_started = false;
	int stream_rate = 44100;
	int target_frames = (stream_rate * LT_TARGET_BUF_MS) / 1000;

	lt_sync_resampler_t resampler;
	lt_sync_resampler_init(&resampler, target_frames);

	audio_set_external_release_cb(client_release_cb);

	uint8_t pkt_buf[sizeof(lt_pkt_header_t) + LT_PACKET_FRAMES * 4];
	int16_t chunk_in[LT_PACKET_FRAMES * 2];
	int16_t chunk_out[LT_PACKET_FRAMES * 2 + 16];

	while (lt_running && lt_mode == LT_MODE_CLIENT) {
		uint64_t t = now_ms();
		if (has_host_ip && ctrl_fd >= 0 && (t - last_sub_ms) >= 2000) {
			lt_ctrl_pkt_t sub = {.magic = CTRL_MAGIC_SUB, .port = LT_AUDIO_PORT};
			sendto(ctrl_fd, &sub, sizeof(sub), 0, (struct sockaddr *)&host_ctrl, sizeof(host_ctrl));
			last_sub_ms = t;
		}

		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		if (poll(&pfd, 1, 10) > 0) {
			ssize_t n = recvfrom(fd, pkt_buf, sizeof(pkt_buf), 0, NULL, NULL);
			if (n >= (ssize_t)sizeof(lt_pkt_header_t)) {
				lt_pkt_header_t hdr;
				memcpy(&hdr, pkt_buf, sizeof(hdr));
				if (hdr.magic == LT_PKT_MAGIC && hdr.frames > 0 && hdr.frames <= LT_PACKET_FRAMES &&
					n >= (ssize_t)(sizeof(hdr) + hdr.frames * 4)) {
					if ((int)hdr.sample_rate != stream_rate && !audio_started) {
						stream_rate = (int)hdr.sample_rate;
						target_frames = (stream_rate * LT_TARGET_BUF_MS) / 1000;
						lt_sync_resampler_init(&resampler, target_frames);
						lt_active_rate = stream_rate;
					}

					if (!seq_init) {
						expected_seq = hdr.seq;
						seq_init = true;
					} else if (hdr.seq > expected_seq) {
						uint32_t lost = hdr.seq - expected_seq;
						if (lost > 8) lost = 8;
						lt_packets_lost += lost;
						/* Packet loss concealment: insert soft-faded silence frames */
						for (uint32_t l = 0; l < lost * hdr.frames && client_fill < CLIENT_RING_FRAMES; l++) {
							client_ring[client_head * 2] = 0;
							client_ring[client_head * 2 + 1] = 0;
							client_head = (client_head + 1) % CLIENT_RING_FRAMES;
							client_fill++;
						}
					}
					expected_seq = hdr.seq + 1;
					lt_packets_rx++;

					const int16_t *pcm = (const int16_t *)(pkt_buf + sizeof(hdr));
					for (int i = 0; i < hdr.frames && client_fill < CLIENT_RING_FRAMES; i++) {
						client_ring[client_head * 2] = pcm[i * 2];
						client_ring[client_head * 2 + 1] = pcm[i * 2 + 1];
						client_head = (client_head + 1) % CLIENT_RING_FRAMES;
						client_fill++;
					}
				}
			}
		}

		if (!audio_started) {
			if (client_fill >= target_frames) {
				if (audio_external_begin_exclusive_latency(stream_rate, 2, 16, 60)) {
					audio_started = true;
				}
			}
			continue;
		}

		if (client_fill >= LT_PACKET_FRAMES) {
			double ratio = lt_sync_resampler_update_pi(&resampler, client_fill);
			lt_last_buffer_ms = (client_fill * 1000) / stream_rate;
			lt_last_drift_ppm = (int)llround((ratio - 1.0) * 1000000.0);

			int avail = client_fill < LT_PACKET_FRAMES ? client_fill : LT_PACKET_FRAMES;
			for (int i = 0; i < avail; i++) {
				int idx = (client_tail + i) % CLIENT_RING_FRAMES;
				chunk_in[i * 2] = client_ring[idx * 2];
				chunk_in[i * 2 + 1] = client_ring[idx * 2 + 1];
			}

			int consumed = 0;
			int out_frames = lt_sync_resampler_process(&resampler, chunk_in, avail, &consumed, chunk_out,
													   LT_PACKET_FRAMES + 8);
			client_tail = (client_tail + consumed) % CLIENT_RING_FRAMES;
			client_fill -= consumed;

			if (out_frames > 0) {
				audio_external_write(chunk_out, out_frames);
			}
		}
	}

	if (has_host_ip && ctrl_fd >= 0) {
		lt_ctrl_pkt_t unsb = {.magic = CTRL_MAGIC_UNSB, .port = LT_AUDIO_PORT};
		sendto(ctrl_fd, &unsb, sizeof(unsb), 0, (struct sockaddr *)&host_ctrl, sizeof(host_ctrl));
	}
	if (ctrl_fd >= 0) {
		close(ctrl_fd);
	}
	if (audio_started) {
		audio_external_end();
	}
	audio_set_external_release_cb(NULL);
	close(fd);
	return NULL;
}

bool lt_start_host(void) {
	lt_stop();
	pthread_mutex_lock(&lt_lock);
	host_head = host_tail = host_fill = 0;
	host_src_phase = 0.0;
	memset(host_peers, 0, sizeof(host_peers));
	lt_packets_tx = lt_packets_rx = lt_packets_lost = 0;
	lt_mode = LT_MODE_HOST;
	lt_running = true;
	pthread_mutex_unlock(&lt_lock);

	audio_set_pcm_tap_cb(host_pcm_tap);
	pthread_create(&lt_thread, NULL, host_tx_worker, NULL);
	pthread_create(&lt_ctrl_thread, NULL, host_ctrl_worker, NULL);
	lt_threads_started = true;
	return true;
}

bool lt_start_client(const char *host_ip) {
	lt_stop();
	pthread_mutex_lock(&lt_lock);
	client_head = client_tail = client_fill = 0;
	lt_packets_tx = lt_packets_rx = lt_packets_lost = 0;
	lt_last_buffer_ms = 0;
	lt_last_drift_ppm = 0;
	if (host_ip && host_ip[0] != '\0') {
		snprintf(lt_peer_ip, sizeof(lt_peer_ip), "%s", host_ip);
	} else {
		lt_peer_ip[0] = '\0';
	}
	lt_mode = LT_MODE_CLIENT;
	lt_running = true;
	pthread_mutex_unlock(&lt_lock);

	pthread_create(&lt_thread, NULL, client_rx_worker, NULL);
	lt_threads_started = true;
	return true;
}

void lt_stop(void) {
	audio_set_pcm_tap_cb(NULL);
	if (!lt_running && !lt_threads_started) {
		lt_mode = LT_MODE_OFF;
		return;
	}
	pthread_mutex_lock(&lt_lock);
	lt_running = false;
	lt_mode_t prev = lt_mode;
	lt_mode = LT_MODE_OFF;
	pthread_cond_broadcast(&lt_cond);
	pthread_mutex_unlock(&lt_lock);

	if (lt_threads_started) {
		pthread_join(lt_thread, NULL);
		if (prev == LT_MODE_HOST) {
			pthread_join(lt_ctrl_thread, NULL);
		}
		lt_threads_started = false;
	}
}

lt_mode_t lt_get_mode(void) {
	return lt_mode;
}

void lt_get_stats(lt_stats_t *out) {
	if (!out) return;
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&lt_lock);
	out->mode = lt_mode;
	out->buffer_ms = lt_last_buffer_ms;
	out->target_ms = LT_TARGET_BUF_MS;
	out->drift_ppm = lt_last_drift_ppm;
	out->packets_tx = lt_packets_tx;
	out->packets_rx = lt_packets_rx;
	out->packets_lost = lt_packets_lost;
	out->active_rate = lt_active_rate;
	snprintf(out->peer_ip, sizeof(out->peer_ip), "%s", lt_peer_ip);
	int peers = 0;
	for (int i = 0; i < MAX_PEERS; i++) {
		if (host_peers[i].active) peers++;
	}
	out->peer_count = peers;
	pthread_mutex_unlock(&lt_lock);
}

int lt_discover_hosts(lt_host_info_t *out, int max_hosts, int timeout_ms) {
	if (!out || max_hosts <= 0) return 0;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return 0;

	int bcast = 1;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

	lt_ctrl_pkt_t disc = {.magic = CTRL_MAGIC_DISC, .port = LT_CTRL_PORT};
	struct sockaddr_in baddr = {0};
	baddr.sin_family = AF_INET;
	baddr.sin_port = htons(LT_CTRL_PORT);
	baddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sendto(fd, &disc, sizeof(disc), 0, (struct sockaddr *)&baddr, sizeof(baddr));

	int count = 0;
	uint64_t deadline = now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 500);
	while (count < max_hosts) {
		uint64_t t = now_ms();
		if (t >= deadline) break;
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		if (poll(&pfd, 1, (int)(deadline - t)) <= 0) break;

		lt_ctrl_pkt_t ann;
		struct sockaddr_in src = {0};
		socklen_t slen = sizeof(src);
		ssize_t n = recvfrom(fd, &ann, sizeof(ann), 0, (struct sockaddr *)&src, &slen);
		if (n >= (ssize_t)sizeof(ann) && ann.magic == CTRL_MAGIC_ANN) {
			char ip[46] = {0};
			inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
			bool dup = false;
			for (int i = 0; i < count; i++) {
				if (strcmp(out[i].ip, ip) == 0) {
					dup = true;
					break;
				}
			}
			if (!dup) {
				snprintf(out[count].name, sizeof(out[count].name), "%s", ann.name);
				snprintf(out[count].ip, sizeof(out[count].ip), "%s", ip);
				out[count].port = ann.port;
				out[count].sample_rate = ann.sample_rate;
				out[count].last_seen_ms = now_ms();
				count++;
			}
		}
	}

	close(fd);
	return count;
}

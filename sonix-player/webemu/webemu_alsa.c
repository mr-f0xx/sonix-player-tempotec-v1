#ifdef HOST_WEB_EMU

#include <alsa/asoundlib.h>
#include "webemu.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct snd_pcm {
	char name[128];
	snd_pcm_stream_t stream;
	snd_pcm_hw_params_t hw;
	snd_pcm_sw_params_t sw;
	snd_pcm_state_t state;
	int nonblock;
};

struct snd_config {
	int dummy;
};

struct snd_input {
	int dummy;
};

struct snd_output {
	char buf[512];
	size_t len;
};

struct snd_ctl {
	int dummy;
};

static struct snd_config g_snd_config;
snd_config_t *snd_config = &g_snd_config;

const char *snd_strerror(int errnum) { return strerror(errnum < 0 ? -errnum : errnum); }

const char *snd_pcm_format_name(snd_pcm_format_t format) {
	switch (format) {
	case SND_PCM_FORMAT_U8:
		return "U8";
	case SND_PCM_FORMAT_S16_LE:
		return "S16_LE";
	case SND_PCM_FORMAT_S24_LE:
		return "S24_LE";
	case SND_PCM_FORMAT_S24_3LE:
		return "S24_3LE";
	case SND_PCM_FORMAT_S32_LE:
		return "S32_LE";
	default:
		return "UNKNOWN";
	}
}

const char *snd_pcm_state_name(snd_pcm_state_t state) {
	switch (state) {
	case SND_PCM_STATE_OPEN:
		return "OPEN";
	case SND_PCM_STATE_SETUP:
		return "SETUP";
	case SND_PCM_STATE_PREPARED:
		return "PREPARED";
	case SND_PCM_STATE_RUNNING:
		return "RUNNING";
	case SND_PCM_STATE_XRUN:
		return "XRUN";
	case SND_PCM_STATE_DRAINING:
		return "DRAINING";
	case SND_PCM_STATE_PAUSED:
		return "PAUSED";
	case SND_PCM_STATE_SUSPENDED:
		return "SUSPENDED";
	case SND_PCM_STATE_DISCONNECTED:
		return "DISCONNECTED";
	default:
		return "UNKNOWN";
	}
}

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode) {
	if (!pcm) {
		return -EINVAL;
	}
	if (stream == SND_PCM_STREAM_CAPTURE) {
		return -ENODEV;
	}
	snd_pcm_t *p = calloc(1, sizeof(*p));
	if (!p) {
		return -ENOMEM;
	}
	snprintf(p->name, sizeof(p->name), "%s", name ? name : "default");
	p->stream = stream;
	p->nonblock = (mode & SND_PCM_NONBLOCK) != 0;
	p->hw.access = SND_PCM_ACCESS_RW_INTERLEAVED;
	p->hw.format = SND_PCM_FORMAT_S16_LE;
	p->hw.channels = 2;
	p->hw.rate = 44100;
	p->hw.period_size = 1024;
	p->hw.periods = 4;
	p->hw.buffer_size = 4096;
	p->state = SND_PCM_STATE_OPEN;
	*pcm = p;
	return 0;
}

int snd_pcm_open_lconf(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode, snd_config_t *lconf) {
	(void)lconf;
	return snd_pcm_open(pcm, name, stream, mode);
}

int snd_pcm_close(snd_pcm_t *pcm) {
	free(pcm);
	return 0;
}

const char *snd_pcm_name(snd_pcm_t *pcm) { return pcm ? pcm->name : "default"; }

int snd_pcm_nonblock(snd_pcm_t *pcm, int nonblock) {
	if (!pcm) return -EINVAL;
	pcm->nonblock = nonblock;
	return 0;
}

int snd_pcm_prepare(snd_pcm_t *pcm) {
	if (!pcm) return -EINVAL;
	pcm->state = SND_PCM_STATE_PREPARED;
	return 0;
}

int snd_pcm_start(snd_pcm_t *pcm) {
	if (!pcm) return -EINVAL;
	pcm->state = SND_PCM_STATE_RUNNING;
	return 0;
}

int snd_pcm_drop(snd_pcm_t *pcm) {
	if (!pcm) return -EINVAL;
	pcm->state = SND_PCM_STATE_SETUP;
	return 0;
}

int snd_pcm_drain(snd_pcm_t *pcm) {
	if (!pcm) return -EINVAL;
	pcm->state = SND_PCM_STATE_SETUP;
	return 0;
}

int snd_pcm_pause(snd_pcm_t *pcm, int enable) {
	if (!pcm) return -EINVAL;
	pcm->state = enable ? SND_PCM_STATE_PAUSED : SND_PCM_STATE_RUNNING;
	return 0;
}

int snd_pcm_resume(snd_pcm_t *pcm) {
	if (!pcm) return -EINVAL;
	pcm->state = SND_PCM_STATE_PREPARED;
	return 0;
}

int snd_pcm_recover(snd_pcm_t *pcm, int err, int silent) {
	(void)silent;
	if (!pcm) return err;
	if (err == -EINTR || err == -EPIPE || err == -ESTRPIPE) {
		pcm->state = SND_PCM_STATE_PREPARED;
		return 0;
	}
	return err;
}

int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delayp) {
	if (!pcm || !delayp) return -EINVAL;
	*delayp = 0;
	return 0;
}

int snd_pcm_wait(snd_pcm_t *pcm, int timeout) {
	(void)pcm;
	(void)timeout;
	return 1;
}

snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm) { return pcm ? pcm->state : SND_PCM_STATE_DISCONNECTED; }

int snd_pcm_dump(snd_pcm_t *pcm, snd_output_t *out) {
	if (!pcm || !out) return -EINVAL;
	out->len = (size_t)snprintf(out->buf, sizeof(out->buf), "WebEmu Virtual PCM (%s, %u Hz, %u ch, %s)\n", pcm->name,
								pcm->hw.rate, pcm->hw.channels, snd_pcm_format_name(pcm->hw.format));
	return 0;
}

static inline int16_t scale_sample(int32_t s16, int vol_gain_q15) {
	int32_t v = (s16 * vol_gain_q15) >> 15;
	if (v > 32767) v = 32767;
	if (v < -32768) v = -32768;
	return (int16_t)v;
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size) {
	if (!pcm || !buffer) {
		return -EINVAL;
	}
	pcm->state = SND_PCM_STATE_RUNNING;

	unsigned int ch = pcm->hw.channels;
	if (ch < 1 || ch > 2 || size == 0) {
		return (snd_pcm_sframes_t)size;
	}

	/* Perceptual volume curve: (pct / 100)^2 scaled to Q15. */
	int pct = webemu_get_volume_percent();
	int gain_q15 = (pct * pct * 32768) / 10000;

	/* Stream in small chunks (<= 2048 frames) to keep stack bounded and latency low. */
	int16_t tmp[2048 * 2];
	const uint8_t *raw = (const uint8_t *)buffer;
	snd_pcm_uframes_t done = 0;

	while (done < size) {
		snd_pcm_uframes_t n = size - done;
		if (n > 2048) {
			n = 2048;
		}
		size_t total_samples = (size_t)n * (size_t)ch;

		if (pcm->hw.format == SND_PCM_FORMAT_S16_LE) {
			const int16_t *in = (const int16_t *)raw + (size_t)done * (size_t)ch;
			for (size_t i = 0; i < total_samples; i++) {
				tmp[i] = scale_sample(in[i], gain_q15);
			}
		} else if (pcm->hw.format == SND_PCM_FORMAT_S32_LE) {
			const int32_t *in = (const int32_t *)raw + (size_t)done * (size_t)ch;
			for (size_t i = 0; i < total_samples; i++) {
				tmp[i] = scale_sample(in[i] >> 16, gain_q15);
			}
		} else if (pcm->hw.format == SND_PCM_FORMAT_S24_LE) {
			const int32_t *in = (const int32_t *)raw + (size_t)done * (size_t)ch;
			for (size_t i = 0; i < total_samples; i++) {
				/* Sign-extend 24-bit in low 24 bits of 32-bit word, then shift to 16-bit. */
				int32_t v = (in[i] << 8) >> 16;
				tmp[i] = scale_sample(v, gain_q15);
			}
		} else if (pcm->hw.format == SND_PCM_FORMAT_S24_3LE) {
			const uint8_t *in = raw + (size_t)done * (size_t)ch * 3u;
			for (size_t i = 0; i < total_samples; i++) {
				int32_t v = ((int32_t)(int8_t)in[i * 3u + 2u] << 8) | (int32_t)in[i * 3u + 1u];
				tmp[i] = scale_sample(v, gain_q15);
			}
		} else if (pcm->hw.format == SND_PCM_FORMAT_U8) {
			const uint8_t *in = raw + (size_t)done * (size_t)ch;
			for (size_t i = 0; i < total_samples; i++) {
				int32_t v = ((int32_t)in[i] - 128) << 8;
				tmp[i] = scale_sample(v, gain_q15);
			}
		} else {
			memset(tmp, 0, total_samples * sizeof(int16_t));
		}

		webemu_send_audio_pcm(pcm->hw.rate, (uint16_t)ch, tmp, (size_t)n);
		done += n;
	}

	return (snd_pcm_sframes_t)size;
}

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t size) {
	(void)pcm;
	(void)buffer;
	(void)size;
	return -ENODEV;
}

int snd_pcm_set_params(snd_pcm_t *pcm, snd_pcm_format_t format, snd_pcm_access_t access, unsigned int channels,
					   unsigned int rate, int soft_resample, unsigned int latency) {
	(void)soft_resample;
	if (!pcm) return -EINVAL;
	pcm->hw.format = format;
	pcm->hw.access = access;
	pcm->hw.channels = channels;
	pcm->hw.rate = rate;
	if (latency > 0 && rate > 0) {
		pcm->hw.buffer_size = (snd_pcm_uframes_t)((uint64_t)rate * latency / 1000000ULL);
		if (pcm->hw.buffer_size < 1024) pcm->hw.buffer_size = 1024;
		pcm->hw.period_size = pcm->hw.buffer_size / 4;
		if (pcm->hw.period_size < 256) pcm->hw.period_size = 256;
	}
	pcm->state = SND_PCM_STATE_PREPARED;
	return 0;
}

int snd_pcm_poll_descriptors_count(snd_pcm_t *pcm) {
	(void)pcm;
	return 0;
}

int snd_pcm_poll_descriptors(snd_pcm_t *pcm, struct pollfd *pfds, unsigned int space) {
	(void)pcm;
	(void)pfds;
	(void)space;
	return 0;
}

int snd_pcm_poll_descriptors_revents(snd_pcm_t *pcm, struct pollfd *pfds, unsigned int nfds, unsigned short *revents) {
	(void)pcm;
	(void)pfds;
	(void)nfds;
	if (revents) *revents = 0;
	return 0;
}

int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) {
	if (!pcm || !params) return -EINVAL;
	*params = pcm->hw;
	return 0;
}

int snd_pcm_hw_params_current(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) {
	if (!pcm || !params) return -EINVAL;
	*params = pcm->hw;
	return 0;
}

void snd_pcm_hw_params_copy(snd_pcm_hw_params_t *dst, const snd_pcm_hw_params_t *src) {
	if (dst && src) {
		*dst = *src;
	}
}

int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) {
	if (!pcm || !params) return -EINVAL;
	pcm->hw = *params;
	if (pcm->hw.period_size == 0) pcm->hw.period_size = 1024;
	if (pcm->hw.periods == 0) pcm->hw.periods = 4;
	if (pcm->hw.buffer_size < pcm->hw.period_size) {
		pcm->hw.buffer_size = pcm->hw.period_size * pcm->hw.periods;
	}
	pcm->state = SND_PCM_STATE_PREPARED;
	return 0;
}

int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_access_t access) {
	(void)pcm;
	if (!params) return -EINVAL;
	params->access = access;
	return 0;
}

int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_format_t format) {
	(void)pcm;
	if (!params) return -EINVAL;
	switch (format) {
	case SND_PCM_FORMAT_U8:
	case SND_PCM_FORMAT_S16_LE:
	case SND_PCM_FORMAT_S24_LE:
	case SND_PCM_FORMAT_S24_3LE:
	case SND_PCM_FORMAT_S32_LE:
		params->format = format;
		return 0;
	default:
		return -EINVAL;
	}
}

int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val) {
	(void)pcm;
	if (!params || val < 1 || val > 2) return -EINVAL;
	params->channels = val;
	return 0;
}

int snd_pcm_hw_params_set_channels_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val) {
	(void)pcm;
	if (!params || !val) return -EINVAL;
	if (*val < 1) *val = 1;
	if (*val > 2) *val = 2;
	params->channels = *val;
	return 0;
}

int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir) {
	(void)pcm;
	(void)dir;
	if (!params || val == 0) return -EINVAL;
	params->rate = val;
	return 0;
}

int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir) {
	(void)pcm;
	if (!params || !val || *val == 0) return -EINVAL;
	params->rate = *val;
	if (dir) *dir = 0;
	return 0;
}

int snd_pcm_hw_params_set_buffer_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir) {
	(void)pcm;
	if (!params || !val) return -EINVAL;
	if (params->rate > 0 && *val > 0) {
		params->buffer_size = (snd_pcm_uframes_t)((uint64_t)params->rate * (*val) / 1000000ULL);
		if (params->buffer_size < 1024) params->buffer_size = 1024;
	}
	if (dir) *dir = 0;
	return 0;
}

int snd_pcm_hw_params_set_period_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir) {
	(void)pcm;
	if (!params || !val) return -EINVAL;
	if (params->rate > 0 && *val > 0) {
		params->period_size = (snd_pcm_uframes_t)((uint64_t)params->rate * (*val) / 1000000ULL);
		if (params->period_size < 256) params->period_size = 256;
	}
	if (dir) *dir = 0;
	return 0;
}

int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir) {
	(void)pcm;
	if (!params || !val) return -EINVAL;
	if (*val < 2) *val = 2;
	params->periods = *val;
	params->buffer_size = params->period_size * params->periods;
	if (dir) *dir = 0;
	return 0;
}

int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val,
										   int *dir) {
	(void)pcm;
	if (!params || !val) return -EINVAL;
	if (*val < 256) *val = 256;
	params->period_size = *val;
	if (params->periods == 0) params->periods = 4;
	params->buffer_size = params->period_size * params->periods;
	if (dir) *dir = 0;
	return 0;
}

int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *params, unsigned int *val, int *dir) {
	if (!params || !val) return -EINVAL;
	*val = params->rate;
	if (dir) *dir = 0;
	return 0;
}

int snd_pcm_hw_params_get_format(const snd_pcm_hw_params_t *params, snd_pcm_format_t *val) {
	if (!params || !val) return -EINVAL;
	*val = params->format;
	return 0;
}

int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *params, unsigned int *val) {
	if (!params || !val) return -EINVAL;
	*val = params->channels;
	return 0;
}

int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *params, snd_pcm_uframes_t *frames, int *dir) {
	if (!params || !frames) return -EINVAL;
	*frames = params->period_size ? params->period_size : 1024;
	if (dir) *dir = 0;
	return 0;
}

int snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val) {
	if (!params || !val) return -EINVAL;
	*val = params->buffer_size ? params->buffer_size : 4096;
	return 0;
}

int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params) {
	if (!pcm || !params) return -EINVAL;
	*params = pcm->sw;
	return 0;
}

int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val) {
	(void)pcm;
	if (!params) return -EINVAL;
	params->avail_min = val;
	return 0;
}

int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val) {
	(void)pcm;
	if (!params) return -EINVAL;
	params->start_threshold = val;
	return 0;
}

int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params) {
	if (!pcm || !params) return -EINVAL;
	pcm->sw = *params;
	return 0;
}

int snd_config_update(void) { return 0; }
int snd_config_copy(snd_config_t **dst, snd_config_t *src) {
	(void)src;
	if (!dst) return -EINVAL;
	*dst = calloc(1, sizeof(snd_config_t));
	return *dst ? 0 : -ENOMEM;
}
int snd_config_load(snd_config_t *config, snd_input_t *in) {
	(void)config;
	(void)in;
	return 0;
}
int snd_config_delete(snd_config_t *config) {
	free(config);
	return 0;
}

int snd_input_buffer_open(snd_input_t **inputp, const char *buffer, ssize_t size) {
	(void)buffer;
	(void)size;
	if (!inputp) return -EINVAL;
	*inputp = calloc(1, sizeof(snd_input_t));
	return *inputp ? 0 : -ENOMEM;
}
int snd_input_close(snd_input_t *input) {
	free(input);
	return 0;
}

int snd_output_buffer_open(snd_output_t **outputp) {
	if (!outputp) return -EINVAL;
	*outputp = calloc(1, sizeof(snd_output_t));
	return *outputp ? 0 : -ENOMEM;
}
size_t snd_output_buffer_string(snd_output_t *output, char **buf) {
	if (!output) return 0;
	if (buf) *buf = output->buf;
	return output->len;
}
int snd_output_close(snd_output_t *output) {
	free(output);
	return 0;
}

/* Control APIs: no CS43131 hardware mixer in the virtual web emulator. */
int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode) {
	(void)ctl;
	(void)name;
	(void)mode;
	return -ENOENT;
}
int snd_ctl_close(snd_ctl_t *ctl) {
	free(ctl);
	return 0;
}
int snd_ctl_elem_list(snd_ctl_t *ctl, snd_ctl_elem_list_t *list) {
	(void)ctl;
	if (list) list->count = 0;
	return 0;
}
int snd_ctl_elem_list_alloc_space(snd_ctl_elem_list_t *obj, unsigned int entries) {
	(void)obj;
	(void)entries;
	return 0;
}
void snd_ctl_elem_list_free_space(snd_ctl_elem_list_t *obj) { (void)obj; }
unsigned int snd_ctl_elem_list_get_count(const snd_ctl_elem_list_t *obj) { return obj ? obj->count : 0; }
void snd_ctl_elem_list_get_id(const snd_ctl_elem_list_t *obj, unsigned int idx, snd_ctl_elem_id_t *ptr) {
	(void)obj;
	(void)idx;
	if (ptr) memset(ptr, 0, sizeof(*ptr));
}
int snd_ctl_elem_info(snd_ctl_t *ctl, snd_ctl_elem_info_t *info) {
	(void)ctl;
	(void)info;
	return -ENOENT;
}
int snd_ctl_elem_info_get_type(const snd_ctl_elem_info_t *obj) { return obj ? obj->type : 0; }
int snd_ctl_elem_info_is_writable(const snd_ctl_elem_info_t *obj) { return obj ? obj->writable : 0; }
int snd_ctl_elem_info_is_tlv_readable(const snd_ctl_elem_info_t *obj) { return obj ? obj->tlv_readable : 0; }
unsigned int snd_ctl_elem_info_get_count(const snd_ctl_elem_info_t *obj) { return obj ? obj->count : 0; }
long snd_ctl_elem_info_get_min(const snd_ctl_elem_info_t *obj) { return obj ? obj->min_val : 0; }
long snd_ctl_elem_info_get_max(const snd_ctl_elem_info_t *obj) { return obj ? obj->max_val : 0; }
const char *snd_ctl_elem_info_get_name(const snd_ctl_elem_info_t *obj) { return obj ? obj->name : ""; }
void snd_ctl_elem_info_set_id(snd_ctl_elem_info_t *obj, const snd_ctl_elem_id_t *ptr) {
	if (obj && ptr) snprintf(obj->name, sizeof(obj->name), "%s", ptr->name);
}
void snd_ctl_elem_id_set_interface(snd_ctl_elem_id_t *obj, int val) {
	if (obj) obj->iface = val;
}
void snd_ctl_elem_id_set_name(snd_ctl_elem_id_t *obj, const char *val) {
	if (obj && val) snprintf(obj->name, sizeof(obj->name), "%s", val);
}
int snd_ctl_elem_tlv_read(snd_ctl_t *ctl, const snd_ctl_elem_id_t *id, unsigned int *tlv, unsigned int tlv_size) {
	(void)ctl;
	(void)id;
	(void)tlv;
	(void)tlv_size;
	return -ENOENT;
}
int snd_ctl_elem_read(snd_ctl_t *ctl, snd_ctl_elem_value_t *control) {
	(void)ctl;
	(void)control;
	return -ENOENT;
}
int snd_ctl_elem_write(snd_ctl_t *ctl, snd_ctl_elem_value_t *control) {
	(void)ctl;
	(void)control;
	return -ENOENT;
}
void snd_ctl_elem_value_set_id(snd_ctl_elem_value_t *obj, const snd_ctl_elem_id_t *ptr) {
	(void)obj;
	(void)ptr;
}
void snd_ctl_elem_value_set_integer(snd_ctl_elem_value_t *obj, unsigned int idx, long val) {
	if (obj && idx < 8) obj->values[idx] = val;
}
long snd_ctl_elem_value_get_integer(const snd_ctl_elem_value_t *obj, unsigned int idx) {
	return (obj && idx < 8) ? obj->values[idx] : 0;
}
int snd_tlv_get_dB_range(unsigned int *tlv, long rangemin, long rangemax, long *min, long *max) {
	(void)tlv;
	(void)rangemin;
	(void)rangemax;
	(void)min;
	(void)max;
	return -ENOENT;
}
int snd_tlv_convert_from_dB(unsigned int *tlv, long rangemin, long rangemax, long db_gain, long *value, int xdir) {
	(void)tlv;
	(void)rangemin;
	(void)rangemax;
	(void)db_gain;
	(void)value;
	(void)xdir;
	return -ENOENT;
}
int snd_tlv_convert_to_dB(unsigned int *tlv, long rangemin, long rangemax, long volume, long *db_gain) {
	(void)tlv;
	(void)rangemin;
	(void)rangemax;
	(void)volume;
	(void)db_gain;
	return -ENOENT;
}

#endif /* HOST_WEB_EMU */

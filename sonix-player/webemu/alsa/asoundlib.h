#ifndef SONIX_WEBEMU_ALSA_ASOUNDLIB_H
#define SONIX_WEBEMU_ALSA_ASOUNDLIB_H

/* Virtual ALSA interface used exclusively by the browser-based V1 emulator
 * (HOST_WEB_EMU). Production target and SDL host builds use real libasound. */

#include <alloca.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

typedef enum {
	SND_PCM_STREAM_PLAYBACK = 0,
	SND_PCM_STREAM_CAPTURE = 1
} snd_pcm_stream_t;

typedef enum {
	SND_PCM_ACCESS_RW_INTERLEAVED = 3
} snd_pcm_access_t;

typedef enum {
	SND_PCM_FORMAT_UNKNOWN = -1,
	SND_PCM_FORMAT_U8 = 1,
	SND_PCM_FORMAT_S16_LE = 2,
	SND_PCM_FORMAT_S24_LE = 6,
	SND_PCM_FORMAT_S32_LE = 10,
	SND_PCM_FORMAT_S24_3LE = 32
} snd_pcm_format_t;

typedef enum {
	SND_PCM_STATE_OPEN = 0,
	SND_PCM_STATE_SETUP,
	SND_PCM_STATE_PREPARED,
	SND_PCM_STATE_RUNNING,
	SND_PCM_STATE_XRUN,
	SND_PCM_STATE_DRAINING,
	SND_PCM_STATE_PAUSED,
	SND_PCM_STATE_SUSPENDED,
	SND_PCM_STATE_DISCONNECTED
} snd_pcm_state_t;

#define SND_PCM_NONBLOCK 0x00000001

typedef struct snd_pcm snd_pcm_t;
typedef struct snd_pcm_hw_params {
	snd_pcm_access_t access;
	snd_pcm_format_t format;
	unsigned int channels;
	unsigned int rate;
	snd_pcm_uframes_t period_size;
	snd_pcm_uframes_t buffer_size;
	unsigned int periods;
} snd_pcm_hw_params_t;

typedef struct snd_pcm_sw_params {
	snd_pcm_uframes_t avail_min;
	snd_pcm_uframes_t start_threshold;
} snd_pcm_sw_params_t;

typedef struct snd_config snd_config_t;
typedef struct snd_input snd_input_t;
typedef struct snd_output snd_output_t;

typedef struct snd_ctl snd_ctl_t;
typedef struct snd_ctl_elem_id {
	int iface;
	char name[64];
} snd_ctl_elem_id_t;

typedef struct snd_ctl_elem_info {
	int type;
	int writable;
	int tlv_readable;
	unsigned int count;
	long min_val;
	long max_val;
	char name[64];
} snd_ctl_elem_info_t;

typedef struct snd_ctl_elem_list {
	unsigned int count;
} snd_ctl_elem_list_t;

typedef struct snd_ctl_elem_value {
	long values[8];
} snd_ctl_elem_value_t;

#define SND_CTL_ELEM_IFACE_MIXER 2
#define SND_CTL_ELEM_TYPE_INTEGER 2
#define SND_CTL_TLV_DB_GAIN_MUTE (-9999999L)

#define snd_pcm_hw_params_alloca(ptr)                                                                                  \
	do {                                                                                                               \
		*(ptr) = (snd_pcm_hw_params_t *)alloca(sizeof(snd_pcm_hw_params_t));                                           \
		memset(*(ptr), 0, sizeof(snd_pcm_hw_params_t));                                                                \
	} while (0)

#define snd_pcm_sw_params_alloca(ptr)                                                                                  \
	do {                                                                                                               \
		*(ptr) = (snd_pcm_sw_params_t *)alloca(sizeof(snd_pcm_sw_params_t));                                           \
		memset(*(ptr), 0, sizeof(snd_pcm_sw_params_t));                                                                \
	} while (0)

#define snd_ctl_elem_id_alloca(ptr)                                                                                    \
	do {                                                                                                               \
		*(ptr) = (snd_ctl_elem_id_t *)alloca(sizeof(snd_ctl_elem_id_t));                                               \
		memset(*(ptr), 0, sizeof(snd_ctl_elem_id_t));                                                                  \
	} while (0)

#define snd_ctl_elem_info_alloca(ptr)                                                                                  \
	do {                                                                                                               \
		*(ptr) = (snd_ctl_elem_info_t *)alloca(sizeof(snd_ctl_elem_info_t));                                           \
		memset(*(ptr), 0, sizeof(snd_ctl_elem_info_t));                                                                \
	} while (0)

#define snd_ctl_elem_list_alloca(ptr)                                                                                  \
	do {                                                                                                               \
		*(ptr) = (snd_ctl_elem_list_t *)alloca(sizeof(snd_ctl_elem_list_t));                                           \
		memset(*(ptr), 0, sizeof(snd_ctl_elem_list_t));                                                                \
	} while (0)

#define snd_ctl_elem_value_alloca(ptr)                                                                                 \
	do {                                                                                                               \
		*(ptr) = (snd_ctl_elem_value_t *)alloca(sizeof(snd_ctl_elem_value_t));                                         \
		memset(*(ptr), 0, sizeof(snd_ctl_elem_value_t));                                                               \
	} while (0)

extern snd_config_t *snd_config;

const char *snd_strerror(int errnum);
const char *snd_pcm_format_name(snd_pcm_format_t format);
const char *snd_pcm_state_name(snd_pcm_state_t state);

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode);
int snd_pcm_open_lconf(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode, snd_config_t *lconf);
int snd_pcm_close(snd_pcm_t *pcm);
const char *snd_pcm_name(snd_pcm_t *pcm);
int snd_pcm_nonblock(snd_pcm_t *pcm, int nonblock);
int snd_pcm_prepare(snd_pcm_t *pcm);
int snd_pcm_start(snd_pcm_t *pcm);
int snd_pcm_drop(snd_pcm_t *pcm);
int snd_pcm_drain(snd_pcm_t *pcm);
int snd_pcm_pause(snd_pcm_t *pcm, int enable);
int snd_pcm_resume(snd_pcm_t *pcm);
int snd_pcm_recover(snd_pcm_t *pcm, int err, int silent);
int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delayp);
int snd_pcm_wait(snd_pcm_t *pcm, int timeout);
snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm);
int snd_pcm_dump(snd_pcm_t *pcm, snd_output_t *out);
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size);
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t size);
int snd_pcm_set_params(snd_pcm_t *pcm, snd_pcm_format_t format, snd_pcm_access_t access, unsigned int channels,
					   unsigned int rate, int soft_resample, unsigned int latency);

int snd_pcm_poll_descriptors_count(snd_pcm_t *pcm);
int snd_pcm_poll_descriptors(snd_pcm_t *pcm, struct pollfd *pfds, unsigned int space);
int snd_pcm_poll_descriptors_revents(snd_pcm_t *pcm, struct pollfd *pfds, unsigned int nfds, unsigned short *revents);

int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params);
int snd_pcm_hw_params_current(snd_pcm_t *pcm, snd_pcm_hw_params_t *params);
void snd_pcm_hw_params_copy(snd_pcm_hw_params_t *dst, const snd_pcm_hw_params_t *src);
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params);
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_access_t access);
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_format_t format);
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val);
int snd_pcm_hw_params_set_channels_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val);
int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir);
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir);
int snd_pcm_hw_params_set_buffer_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir);
int snd_pcm_hw_params_set_period_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir);
int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir);
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val,
										   int *dir);
int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *params, unsigned int *val, int *dir);
int snd_pcm_hw_params_get_format(const snd_pcm_hw_params_t *params, snd_pcm_format_t *val);
int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *params, unsigned int *val);
int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *params, snd_pcm_uframes_t *frames, int *dir);
int snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val);

int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params);
int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val);
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val);
int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params);

int snd_config_update(void);
int snd_config_copy(snd_config_t **dst, snd_config_t *src);
int snd_config_load(snd_config_t *config, snd_input_t *in);
int snd_config_delete(snd_config_t *config);
int snd_input_buffer_open(snd_input_t **inputp, const char *buffer, ssize_t size);
int snd_input_close(snd_input_t *input);
int snd_output_buffer_open(snd_output_t **outputp);
size_t snd_output_buffer_string(snd_output_t *output, char **buf);
int snd_output_close(snd_output_t *output);

int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode);
int snd_ctl_close(snd_ctl_t *ctl);
int snd_ctl_elem_list(snd_ctl_t *ctl, snd_ctl_elem_list_t *list);
int snd_ctl_elem_list_alloc_space(snd_ctl_elem_list_t *obj, unsigned int entries);
void snd_ctl_elem_list_free_space(snd_ctl_elem_list_t *obj);
unsigned int snd_ctl_elem_list_get_count(const snd_ctl_elem_list_t *obj);
void snd_ctl_elem_list_get_id(const snd_ctl_elem_list_t *obj, unsigned int idx, snd_ctl_elem_id_t *ptr);
int snd_ctl_elem_info(snd_ctl_t *ctl, snd_ctl_elem_info_t *info);
int snd_ctl_elem_info_get_type(const snd_ctl_elem_info_t *obj);
int snd_ctl_elem_info_is_writable(const snd_ctl_elem_info_t *obj);
int snd_ctl_elem_info_is_tlv_readable(const snd_ctl_elem_info_t *obj);
unsigned int snd_ctl_elem_info_get_count(const snd_ctl_elem_info_t *obj);
long snd_ctl_elem_info_get_min(const snd_ctl_elem_info_t *obj);
long snd_ctl_elem_info_get_max(const snd_ctl_elem_info_t *obj);
const char *snd_ctl_elem_info_get_name(const snd_ctl_elem_info_t *obj);
void snd_ctl_elem_info_set_id(snd_ctl_elem_info_t *obj, const snd_ctl_elem_id_t *ptr);
void snd_ctl_elem_id_set_interface(snd_ctl_elem_id_t *obj, int val);
void snd_ctl_elem_id_set_name(snd_ctl_elem_id_t *obj, const char *val);
int snd_ctl_elem_tlv_read(snd_ctl_t *ctl, const snd_ctl_elem_id_t *id, unsigned int *tlv, unsigned int tlv_size);
int snd_ctl_elem_read(snd_ctl_t *ctl, snd_ctl_elem_value_t *control);
int snd_ctl_elem_write(snd_ctl_t *ctl, snd_ctl_elem_value_t *control);
void snd_ctl_elem_value_set_id(snd_ctl_elem_value_t *obj, const snd_ctl_elem_id_t *ptr);
void snd_ctl_elem_value_set_integer(snd_ctl_elem_value_t *obj, unsigned int idx, long val);
long snd_ctl_elem_value_get_integer(const snd_ctl_elem_value_t *obj, unsigned int idx);
int snd_tlv_get_dB_range(unsigned int *tlv, long rangemin, long rangemax, long *min, long *max);
int snd_tlv_convert_from_dB(unsigned int *tlv, long rangemin, long rangemax, long db_gain, long *value, int xdir);
int snd_tlv_convert_to_dB(unsigned int *tlv, long rangemin, long rangemax, long volume, long *db_gain);

#ifdef __cplusplus
}
#endif

#endif /* SONIX_WEBEMU_ALSA_ASOUNDLIB_H */

#ifndef BT_RX_ALSA_TEST_H
#define BT_RX_ALSA_TEST_H
#include <poll.h>
typedef struct snd_pcm { int unused; } snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
typedef int snd_pcm_format_t;
#define SND_PCM_STREAM_CAPTURE 1
#define SND_PCM_FORMAT_S16_LE 2
#define SND_PCM_FORMAT_S32_LE 10
#define SND_PCM_ACCESS_RW_INTERLEAVED 3
int snd_pcm_open(snd_pcm_t **, const char *, int, int);
int snd_pcm_close(snd_pcm_t *);
int snd_pcm_set_params(snd_pcm_t *, snd_pcm_format_t, int, unsigned, unsigned, int, unsigned);
int snd_pcm_nonblock(snd_pcm_t *, int);
int snd_pcm_poll_descriptors_count(snd_pcm_t *);
int snd_pcm_poll_descriptors(snd_pcm_t *, struct pollfd *, unsigned);
int snd_pcm_poll_descriptors_revents(snd_pcm_t *, struct pollfd *, unsigned, unsigned short *);
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *, void *, snd_pcm_uframes_t);
int snd_pcm_recover(snd_pcm_t *, int, int);
const char *snd_strerror(int);
#endif

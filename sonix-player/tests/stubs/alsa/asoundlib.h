#ifndef SONIX_TEST_STUB_ALSA_H
#define SONIX_TEST_STUB_ALSA_H
/* Declarations only: USB arbitration tests discard all mixer code at link
 * time. Production compilation uses real ALSA headers, never this stub. */
typedef struct snd_ctl snd_ctl_t;
typedef struct snd_ctl_elem_id snd_ctl_elem_id_t;
typedef struct snd_ctl_elem_info snd_ctl_elem_info_t;
typedef struct snd_ctl_elem_list snd_ctl_elem_list_t;
typedef struct snd_ctl_elem_value snd_ctl_elem_value_t;
#define snd_ctl_elem_id_alloca(p) (*(p) = (snd_ctl_elem_id_t *)0)
#define snd_ctl_elem_info_alloca(p) (*(p) = (snd_ctl_elem_info_t *)0)
#define snd_ctl_elem_list_alloca(p) (*(p) = (snd_ctl_elem_list_t *)0)
#define snd_ctl_elem_value_alloca(p) (*(p) = (snd_ctl_elem_value_t *)0)
#define SND_CTL_ELEM_IFACE_MIXER 0
#define SND_CTL_ELEM_TYPE_INTEGER 1
#define SND_CTL_TLV_DB_GAIN_MUTE (-9999999L)
int snd_ctl_open();
int snd_ctl_close();
int snd_ctl_elem_list();
int snd_ctl_elem_list_alloc_space();
void snd_ctl_elem_list_free_space();
unsigned int snd_ctl_elem_list_get_count();
void snd_ctl_elem_list_get_id();
int snd_ctl_elem_info();
int snd_ctl_elem_info_get_type();
int snd_ctl_elem_info_is_writable();
int snd_ctl_elem_info_is_tlv_readable();
unsigned int snd_ctl_elem_info_get_count();
long snd_ctl_elem_info_get_min();
long snd_ctl_elem_info_get_max();
const char *snd_ctl_elem_info_get_name();
void snd_ctl_elem_info_set_id();
void snd_ctl_elem_id_set_interface();
void snd_ctl_elem_id_set_name();
int snd_ctl_elem_tlv_read();
int snd_ctl_elem_read();
int snd_ctl_elem_write();
void snd_ctl_elem_value_set_id();
void snd_ctl_elem_value_set_integer();
long snd_ctl_elem_value_get_integer();
int snd_tlv_get_dB_range();
int snd_tlv_convert_from_dB();
int snd_tlv_convert_to_dB();
#endif

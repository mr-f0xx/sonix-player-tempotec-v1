#ifndef WEBEMU_WASM_COMPAT_LINUX_INPUT_H
#define WEBEMU_WASM_COMPAT_LINUX_INPUT_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/time.h>

struct input_event {
	struct timeval time;
	uint16_t type;
	uint16_t code;
	int32_t value;
};

struct input_absinfo {
	int32_t value;
	int32_t minimum;
	int32_t maximum;
	int32_t fuzz;
	int32_t flat;
	int32_t resolution;
};

struct input_id {
	uint16_t bustype;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
};

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MAX 0x1f

#define SYN_REPORT 0
#define SYN_MT_REPORT 2

#define KEY_RESERVED 0
#define KEY_ESC 1
#define KEY_ENTER 28
#define KEY_UP 103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108
#define KEY_MUTE 113
#define KEY_VOLUMEDOWN 114
#define KEY_VOLUMEUP 115
#define KEY_POWER 116
#define KEY_PAUSE 119
#define KEY_STOP 128
#define KEY_BACK 158
#define KEY_NEXTSONG 163
#define KEY_PLAYPAUSE 164
#define KEY_PREVIOUSSONG 165
#define KEY_STOPCD 166
#define KEY_REWIND 168
#define KEY_PLAYCD 200
#define KEY_PAUSECD 201
#define KEY_PLAY 207
#define KEY_FASTFORWARD 208
#define KEY_MAX 0x2ff

#define BTN_TOUCH 0x14a
#define BTN_TOOL_FINGER 0x145

#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_PRESSURE 0x18
#define ABS_MT_SLOT 0x2f
#define ABS_MT_TOUCH_MAJOR 0x30
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39
#define ABS_MT_PRESSURE 0x3a
#define ABS_MAX 0x3f

#ifndef _IOC
#define _IOC(dir, type, nr, size) (((dir) << 30) | ((type) << 8) | (nr) | ((size) << 16))
#endif
#define EVIOCGVERSION _IOR('E', 0x01, int)
#define EVIOCGID _IOR('E', 0x02, struct input_id)
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, (len))
#define EVIOCGBIT(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), (len))
#define EVIOCGABS(abs) _IOR('E', 0x40 + (abs), struct input_absinfo)
#define EVIOCGKEY(len) _IOC(_IOC_READ, 'E', 0x18, (len))
#define EVIOCGRAB _IOW('E', 0x90, int)

#define BUS_PCI 0x01
#define BUS_ISAPNP 0x02
#define BUS_USB 0x03
#define BUS_HIL 0x04
#define BUS_BLUETOOTH 0x05
#define BUS_VIRTUAL 0x06

#endif /* WEBEMU_WASM_COMPAT_LINUX_INPUT_H */

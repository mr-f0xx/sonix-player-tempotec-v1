#ifndef SONIX_WEBEMU_H
#define SONIX_WEBEMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HOST_WEB_EMU

#include "lvgl/lvgl.h"

/* Initialize the Unix socket connection to the web emulator server (via
 * SONIX_WEB_EMU_SOCK). Safe to call even if SONIX_WEB_EMU_SOCK is unset. */
void webemu_init(void);
void webemu_shutdown(void);

/* Create the headless RGB565 framebuffer LVGL display and touch pointer. */
lv_display_t *webemu_init_display(int width, int height);
lv_indev_t *webemu_init_pointer(lv_display_t *disp);

/* Read back the current RGB565 framebuffer (for display_capture_frame). */
uint16_t *webemu_capture_frame(int *out_w, int *out_h);

/* 180-degree software rotation mirroring the V1 Rotate screen setting. */
void webemu_set_rotated(bool rotated);

/* Polled on the LVGL thread to execute pending UI-thread actions (e.g. shot). */
bool webemu_take_screenshot_request(void);
bool webemu_quit_requested(void);

/* Power button edge tracking for chargescreen.c (before system threads run). */
void webemu_take_power_edges(bool *pressed, bool *released);

/* Hardware telemetry pushed to the web UI. */
void webemu_notify_led(const char *mode, int r, int g, int b, int period, int duty, int fade);
void webemu_notify_backlight(long value, long max_val);
void webemu_notify_screen(bool on);
void webemu_notify_volume(int percent);
void webemu_notify_power(const char *action);

/* Audio PCM streaming to browser Web Audio API (interleaved S16LE). */
int webemu_get_volume_percent(void);
void webemu_send_audio_pcm(uint32_t sample_rate, uint16_t channels, const int16_t *samples, size_t frame_count);

#ifdef __EMSCRIPTEN__
/* Condition-variable backed key event queue for WASM pthreads (MEMFS pipes do
 * not support blocking poll()). */
void webemu_wasm_push_key_event(int code, bool down);
int webemu_wasm_wait_key_event(int *out_code, int *out_value, int timeout_ms);
#endif

#endif /* HOST_WEB_EMU */

#endif /* SONIX_WEBEMU_H */

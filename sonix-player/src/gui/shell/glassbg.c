#include "glassbg.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "src/gui/shell/glasswallpaper.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/image/jpeg_planes.h"
#include "src/system/image/jpeg_scaled.h"
#include "src/system/image/png_scaled.h"

#define DIR_NAME "Wallpaper"

// The same decode budget the boot picture uses: a camera-sized JPEG is read,
// but never beyond what the scaled decoders can fit beside the interface.
#define MAX_SOURCE_BYTES (12u * 1024u * 1024u)
#define PLANES_BUDGET_BYTES (12u * 1024u * 1024u)

static char card_root[512];
static char wallpaper_name[GLASSBG_NAME_MAX + 1];
static int dim_percent = GLASSBG_DIM_DEFAULT;

static uint8_t *custom_px;
static lv_image_dsc_t custom_dsc;
static bool custom_active;
static bool pending;

// The blurred-artwork background: the playing track's cover, blurred to the
// panel by the player and handed over here. It outranks the wallpaper while it
// is on hand; with none, the wallpaper (custom or supplied) shows instead.
static bool artwork_mode;
static uint8_t *artwork_px;
static lv_image_dsc_t artwork_dsc;
static bool artwork_active;

const lv_image_dsc_t *glassbg_source(void) {
	if (artwork_active) {
		return &artwork_dsc;
	}
	return custom_active ? &custom_dsc : &glass_wallpaper_dsc;
}

bool glassbg_artwork_enabled(void) {
	return artwork_mode;
}

static void set_artwork(uint8_t *px, int w, int h) {
	free(artwork_px);
	artwork_px = px;
	memset(&artwork_dsc, 0, sizeof(artwork_dsc));
	if (px) {
		artwork_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
		artwork_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
		artwork_dsc.header.w = (uint32_t)w;
		artwork_dsc.header.h = (uint32_t)h;
		artwork_dsc.header.stride = (uint32_t)(w * 2);
		artwork_dsc.data_size = (size_t)w * (size_t)h * 2;
		artwork_dsc.data = px;
		artwork_active = true;
	} else {
		artwork_active = false;
	}
}

void glassbg_set_artwork(bool on) {
	if (artwork_mode == on) {
		return;
	}
	artwork_mode = on;
	config_set_bool("ui", "glass_bg_artwork", on);
	config_save();
	if (!on) {
		set_artwork(NULL, 0, 0);
	}
	theme_repaint();
}

void glassbg_install_artwork(uint8_t *px, int panel_w, int panel_h) {
	if (!px) {
		return;
	}
	set_artwork(px, panel_w, panel_h);
	theme_repaint();
}

void glassbg_clear_artwork(void) {
	if (!artwork_active) {
		return;
	}
	set_artwork(NULL, 0, 0);
	theme_repaint();
}

lv_opa_t glassbg_dim_opa(void) {
	return (lv_opa_t)(dim_percent * 255 / 100);
}

int glassbg_dim_percent(void) {
	return dim_percent;
}

const char *glassbg_current_name(void) {
	return wallpaper_name;
}

const char *glassbg_dir(void) {
	static char dir[600];
	if (!card_root[0]) {
		dir[0] = '\0';
		return dir;
	}
	snprintf(dir, sizeof(dir), "%s/%s", card_root, DIR_NAME);
	return dir;
}

// ---------------------------------------------------------------------------
// The folder
// ---------------------------------------------------------------------------

static bool picture_name(const char *name) {
	if (!name || name[0] == '.') {
		return false; // a dot file is never a picture somebody put there
	}
	const char *dot = strrchr(name, '.');
	if (!dot) {
		return false;
	}
	return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 ||
		   strcasecmp(dot, ".png") == 0;
}

int glassbg_list(char names[][GLASSBG_NAME_MAX + 1], int max) {
	int count = 0;
	const char *dir = glassbg_dir();
	if (!dir[0]) {
		return 0;
	}
	DIR *d = opendir(dir);
	if (!d) {
		return 0;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL && count < max) {
		if (!picture_name(e->d_name)) {
			continue;
		}
		snprintf(names[count], GLASSBG_NAME_MAX + 1, "%s", e->d_name);
		count++;
	}
	closedir(d);
	// Alphabetical, so the list is the same order every visit.
	for (int i = 1; i < count; i++) {
		char key[GLASSBG_NAME_MAX + 1];
		snprintf(key, sizeof(key), "%s", names[i]);
		int j = i - 1;
		while (j >= 0 && strcasecmp(names[j], key) > 0) {
			snprintf(names[j + 1], GLASSBG_NAME_MAX + 1, "%s", names[j]);
			j--;
		}
		snprintf(names[j + 1], GLASSBG_NAME_MAX + 1, "%s", key);
	}
	return count;
}

// ---------------------------------------------------------------------------
// The decode (no LVGL state; safe off the interface thread)
// ---------------------------------------------------------------------------

static uint8_t *read_whole(const char *path, size_t *out_size) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long sz = ftell(f);
	if (sz <= 0 || (unsigned long)sz > MAX_SOURCE_BYTES || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	uint8_t *data = malloc((size_t)sz);
	if (!data) {
		fclose(f);
		return NULL;
	}
	if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
		free(data);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*out_size = (size_t)sz;
	return data;
}

// Decoded small: never at more resolution than the panel can use. The same
// reduction the boot picture takes (see bootpic.c).
static uint8_t *decode_small(const uint8_t *data, size_t size, int panel_w, int panel_h, int *out_w,
							 int *out_h) {
	int shortest = panel_w < panel_h ? panel_w : panel_h;
	int longest = panel_w > panel_h ? panel_w : panel_h;
	if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
		uint8_t *rgb = jpeg_scaled_decode(data, size, shortest / 2, out_w, out_h, NULL, NULL);
		if (rgb) {
			return rgb;
		}
		return jpeg_planes_decode(data, size, longest * 2, PLANES_BUDGET_BYTES, out_w, out_h, NULL);
	}
	if (size >= 8 && data[0] == 0x89 && data[1] == 'P') {
		return png_scaled_decode(data, size, shortest, out_w, out_h);
	}
	return NULL;
}

// The rectangle of the source that covers the panel, centred, the rest cropped.
static void crop_rect(int sw, int sh, int dw, int dh, int *x0, int *y0, int *cw, int *ch) {
	if ((int64_t)sw * dh >= (int64_t)sh * dw) {
		*cw = (int)((int64_t)sh * dw / dh);
		*ch = sh;
	} else {
		*cw = sw;
		*ch = (int)((int64_t)sw * dh / dw);
	}
	*cw = *cw < 1 ? 1 : *cw;
	*ch = *ch < 1 ? 1 : *ch;
	*x0 = (sw - *cw) / 2;
	*y0 = (sh - *ch) / 2;
}

// A box filter straight into RGB565: every destination pixel the mean of the
// source pixels it stands for, packed the way the panel scans it out. Works
// whether the crop is reduced or enlarged (see bootpic.c's resample_rgb).
static uint8_t *resample_rgb565(const uint8_t *src, int sw, int x0, int y0, int cw, int ch, int dw,
								int dh) {
	uint8_t *dst = malloc((size_t)dw * (size_t)dh * 2);
	if (!dst) {
		return NULL;
	}
	uint16_t *out = (uint16_t *)dst;
	for (int dy = 0; dy < dh; dy++) {
		int ys = dy * ch / dh;
		int ye = (dy + 1) * ch / dh;
		if (ye <= ys) {
			ye = ys + 1;
		}
		if (ye > ch) {
			ye = ch;
		}
		if (ys >= ch) {
			ys = ch - 1;
		}
		for (int dx = 0; dx < dw; dx++) {
			int xs = dx * cw / dw;
			int xe = (dx + 1) * cw / dw;
			if (xe <= xs) {
				xe = xs + 1;
			}
			if (xe > cw) {
				xe = cw;
			}
			if (xs >= cw) {
				xs = cw - 1;
			}
			uint32_t sum[3] = {0, 0, 0};
			uint32_t n = 0;
			for (int y = ys; y < ye; y++) {
				const uint8_t *line = src + ((size_t)(y0 + y) * sw + x0 + xs) * 3;
				for (int x = 0; x < xe - xs; x++) {
					sum[0] += line[x * 3];
					sum[1] += line[x * 3 + 1];
					sum[2] += line[x * 3 + 2];
					n++;
				}
			}
			uint32_t r = sum[0] / n, g = sum[1] / n, b = sum[2] / n;
			out[(size_t)dy * dw + dx] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		}
	}
	return dst;
}

uint8_t *glassbg_decode_file(const char *path, int panel_w, int panel_h) {
	if (!path || !path[0] || panel_w <= 0 || panel_h <= 0) {
		return NULL;
	}
	size_t size = 0;
	uint8_t *data = read_whole(path, &size);
	if (!data) {
		return NULL;
	}
	int sw = 0, sh = 0;
	uint8_t *rgb = decode_small(data, size, panel_w, panel_h, &sw, &sh);
	free(data);
	if (!rgb || sw <= 0 || sh <= 0) {
		free(rgb);
		return NULL;
	}
	int x0, y0, cw, ch;
	crop_rect(sw, sh, panel_w, panel_h, &x0, &y0, &cw, &ch);
	uint8_t *px = resample_rgb565(rgb, sw, x0, y0, cw, ch, panel_w, panel_h);
	free(rgb);
	return px;
}

// ---------------------------------------------------------------------------
// The active picture (interface thread)
// ---------------------------------------------------------------------------

static void set_custom(uint8_t *px, int w, int h) {
	free(custom_px);
	custom_px = px;
	memset(&custom_dsc, 0, sizeof(custom_dsc));
	if (px) {
		custom_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
		custom_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
		custom_dsc.header.w = (uint32_t)w;
		custom_dsc.header.h = (uint32_t)h;
		custom_dsc.header.stride = (uint32_t)(w * 2);
		custom_dsc.data_size = (size_t)w * (size_t)h * 2;
		custom_dsc.data = px;
		custom_active = true;
	} else {
		custom_active = false;
	}
}

void glassbg_install(uint8_t *px, int panel_w, int panel_h, const char *name) {
	if (!px) {
		return;
	}
	set_custom(px, panel_w, panel_h);
	snprintf(wallpaper_name, sizeof(wallpaper_name), "%s", name ? name : "");
	config_set("ui", "glass_wallpaper", wallpaper_name);
	config_save();
	pending = false;
	theme_repaint();
}

void glassbg_reset(void) {
	set_custom(NULL, 0, 0);
	wallpaper_name[0] = '\0';
	config_set("ui", "glass_wallpaper", "");
	config_save();
	pending = false;
	theme_repaint();
}

static int dim_clamp(int percent) {
	if (percent < 0) {
		percent = 0;
	}
	if (percent > GLASSBG_DIM_MAX) {
		percent = GLASSBG_DIM_MAX;
	}
	return percent;
}

void glassbg_set_dim(int percent) {
	dim_percent = dim_clamp(percent);
	config_set_int("ui", "glass_wallpaper_dim", dim_percent);
	config_save();
	theme_repaint();
}

void glassbg_preview_dim(int percent) {
	dim_percent = dim_clamp(percent);
	theme_repaint();
}

void glassbg_sync(void) {
	if (!pending) {
		return;
	}
	// Only Liquid Glass shows a wallpaper; on any other theme leave the choice
	// pending so the decode happens when glass is next selected, not now.
	if (!theme_is_liquid_glass()) {
		return;
	}
	pending = false;
	if (!wallpaper_name[0] || !card_root[0]) {
		set_custom(NULL, 0, 0);
		return;
	}
	char path[800];
	snprintf(path, sizeof(path), "%s/%s/%s", card_root, DIR_NAME, wallpaper_name);
	lv_display_t *disp = lv_display_get_default();
	int w = (int)lv_display_get_horizontal_resolution(disp);
	int h = (int)lv_display_get_vertical_resolution(disp);
	uint8_t *px = glassbg_decode_file(path, w, h);
	set_custom(px, w, h);
}

static void reload_posted(void *unused) {
	(void)unused;
	glassbg_sync();
	theme_repaint();
}

void glassbg_set_root(const char *root) {
	snprintf(card_root, sizeof(card_root), "%s", root ? root : "");
	pending = true;
	gui_post(reload_posted, NULL);
}

void glassbg_init(void) {
	dim_percent = (int)config_get_int("ui", "glass_wallpaper_dim", GLASSBG_DIM_DEFAULT);
	if (dim_percent < 0) {
		dim_percent = 0;
	}
	if (dim_percent > GLASSBG_DIM_MAX) {
		dim_percent = GLASSBG_DIM_MAX;
	}
	snprintf(wallpaper_name, sizeof(wallpaper_name), "%s", config_get("ui", "glass_wallpaper", ""));
	artwork_mode = config_get_bool("ui", "glass_bg_artwork", false);
	pending = true;
}

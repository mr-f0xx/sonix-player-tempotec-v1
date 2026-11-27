#include "glassicons.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// One cached badge. The descriptor's data points at `buf`, an ARGB8888 bitmap
// (LVGL's byte order is B, G, R, A) rendered once and kept until the theme
// leaves Liquid Glass. Nothing is ever evicted while a widget may still draw
// it; the whole cache is dropped together by glass_icons_clear().
typedef struct {
	const lv_image_dsc_t *glyph;
	int size;
	uint32_t color; // 0xRRGGBB
	lv_image_dsc_t dsc;
	uint8_t *buf;
	bool used;
} badge_t;

// Large enough for every page's badges plus a full font-size sweep of the
// icons-only home grid (each footer height yields a fresh badge size). The
// whole pool is freed together when the theme leaves Liquid Glass.
#define BADGE_POOL 512
static badge_t badges[BADGE_POOL];

static uint32_t pack(lv_color_t c) { return ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue; }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The glyph's coverage at a source pixel, as 0..255. The menu glyphs are A8
// masks; a true-colour glyph contributes its alpha. Anything else is opaque.
static int glyph_alpha_at(const lv_image_dsc_t *g, int x, int y) {
	if (x < 0) {
		x = 0;
	}
	if (y < 0) {
		y = 0;
	}
	if (x >= (int)g->header.w) {
		x = (int)g->header.w - 1;
	}
	if (y >= (int)g->header.h) {
		y = (int)g->header.h - 1;
	}
	const uint8_t *row = g->data + (size_t)y * g->header.stride;
	if (g->header.cf == LV_COLOR_FORMAT_A8) {
		return row[x];
	}
	if (g->header.cf == LV_COLOR_FORMAT_ARGB8888 || g->header.cf == LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED) {
		return row[x * 4 + 3];
	}
	return 255;
}

static float glyph_alpha_bilinear(const lv_image_dsc_t *g, float sx, float sy) {
	int x0 = (int)floorf(sx);
	int y0 = (int)floorf(sy);
	float fx = sx - x0;
	float fy = sy - y0;
	float a00 = (float)glyph_alpha_at(g, x0, y0);
	float a10 = (float)glyph_alpha_at(g, x0 + 1, y0);
	float a01 = (float)glyph_alpha_at(g, x0, y0 + 1);
	float a11 = (float)glyph_alpha_at(g, x0 + 1, y0 + 1);
	float top = a00 + (a10 - a00) * fx;
	float bot = a01 + (a11 - a01) * fx;
	return top + (bot - top) * fy;
}

// Composites the white glyph, bilinearly scaled to ~56% of the badge, centred
// over the badge pixel already in `buf` (straight alpha, B G R A order).
static void stamp_glyph(uint8_t *buf, int size, const lv_image_dsc_t *g) {
	int gs = (int)((float)size * 0.56f + 0.5f);
	if (gs < 1) {
		gs = 1;
	}
	int ox = (size - gs) / 2;
	int oy = (size - gs) / 2;
	int sw = (int)g->header.w;
	int sh = (int)g->header.h;
	if (sw < 1 || sh < 1) {
		return;
	}
	for (int gy = 0; gy < gs; gy++) {
		float sy = ((float)gy + 0.5f) * (float)sh / (float)gs - 0.5f;
		for (int gx = 0; gx < gs; gx++) {
			float sx = ((float)gx + 0.5f) * (float)sw / (float)gs - 0.5f;
			float ga = clampf(glyph_alpha_bilinear(g, sx, sy) / 255.0f, 0.0f, 1.0f);
			if (ga <= 0.0f) {
				continue;
			}
			uint8_t *px = buf + ((size_t)(oy + gy) * size + (ox + gx)) * 4;
			float ba = (float)px[3] / 255.0f;
			float out_a = ga + ba * (1.0f - ga);
			if (out_a <= 0.0f) {
				continue;
			}
			for (int c = 0; c < 3; c++) {
				float over = (255.0f * ga + (float)px[c] * ba * (1.0f - ga)) / out_a;
				px[c] = (uint8_t)clampf(over + 0.5f, 0.0f, 255.0f);
			}
			px[3] = (uint8_t)clampf(out_a * 255.0f + 0.5f, 0.0f, 255.0f);
		}
	}
}

// Apple's Liquid Glass "clear" app icon: a translucent frosted squircle that
// lets the wallpaper show through, a bright specular rim (stronger along the
// top edge where light catches the glass), and the glyph in solid white. The
// badge is mostly partial-alpha, so it composites over whatever is behind it
// like a real pane of glass rather than a painted tile.
static void render_badge(uint8_t *buf, int size, lv_color_t color, const lv_image_dsc_t *glyph) {
	(void)color; // clear glass is neutral; the hue lives in the wallpaper behind
	float half = (float)size * 0.5f;
	float radius = (float)size * 0.225f; // the iOS corner roundness
	if (radius < 1.0f) {
		radius = 1.0f;
	}
	for (int y = 0; y < size; y++) {
		float t = size > 1 ? (float)y / (float)(size - 1) : 0.0f;
		// Clear glass body: a whisper of milk, lighter at the top, so the
		// wallpaper shows through like iOS 26's clear icons.
		float body = 0.16f + 0.16f * (1.0f - t);
		for (int x = 0; x < size; x++) {
			// Signed distance to a rounded square, for a clean anti-aliased rim.
			float px = (float)x + 0.5f;
			float py = (float)y + 0.5f;
			float qx = fabsf(px - half) - (half - radius);
			float qy = fabsf(py - half) - (half - radius);
			float ax = qx > 0.0f ? qx : 0.0f;
			float ay = qy > 0.0f ? qy : 0.0f;
			float inner = fmaxf(qx, qy);
			if (inner > 0.0f) {
				inner = 0.0f;
			}
			float dist = sqrtf(ax * ax + ay * ay) + inner - radius;
			float cov = clampf(0.5f - dist, 0.0f, 1.0f);
			// Specular rim: a bright line just inside the edge, catching more
			// light along the top than the foot.
			float rim = clampf(1.0f - fabsf(dist) / 1.5f, 0.0f, 1.0f);
			float rim_a = rim * (0.95f - 0.35f * t);
			float a = body;
			if (rim_a > a) {
				a = rim_a;
			}
			a *= cov;
			uint8_t *pxb = buf + ((size_t)y * size + x) * 4;
			pxb[0] = 255; // B
			pxb[1] = 255; // G
			pxb[2] = 255; // R
			pxb[3] = (uint8_t)clampf(a * 255.0f + 0.5f, 0.0f, 255.0f);
		}
	}
	if (glyph && glyph->data) {
		stamp_glyph(buf, size, glyph);
	}
}

static const lv_image_dsc_t *badge_lookup(const lv_image_dsc_t *glyph, int size, uint32_t key) {
	for (int i = 0; i < BADGE_POOL; i++) {
		if (badges[i].used && badges[i].glyph == glyph && badges[i].size == size && badges[i].color == key) {
			return &badges[i].dsc;
		}
	}
	return NULL;
}

const lv_image_dsc_t *glass_icon_badge_find(const lv_image_dsc_t *glyph, int size, lv_color_t color) {
	if (!glyph || size < 3) {
		return NULL;
	}
	return badge_lookup(glyph, size, pack(color));
}

const lv_image_dsc_t *glass_icon_badge(const lv_image_dsc_t *glyph, int size, lv_color_t color) {
	if (!glyph || size < 3) {
		return glyph;
	}
	uint32_t key = pack(color);
	const lv_image_dsc_t *cached = badge_lookup(glyph, size, key);
	if (cached) {
		return cached;
	}
	int slot = -1;
	for (int i = 0; i < BADGE_POOL; i++) {
		if (!badges[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		return glyph; // pool exhausted: fall back to the plain glyph
	}
	size_t bytes = (size_t)size * size * 4;
	uint8_t *buf = malloc(bytes);
	if (!buf) {
		return glyph;
	}
	render_badge(buf, size, color, glyph);

	badge_t *b = &badges[slot];
	b->glyph = glyph;
	b->size = size;
	b->color = key;
	b->buf = buf;
	memset(&b->dsc, 0, sizeof(b->dsc));
	b->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	b->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
	b->dsc.header.w = size;
	b->dsc.header.h = size;
	b->dsc.header.stride = size * 4;
	b->dsc.data = buf;
	b->dsc.data_size = bytes;
	b->used = true;
	return &b->dsc;
}

const lv_image_dsc_t *glass_icon_badge_resized(const lv_image_dsc_t *maybe_badge, int size) {
	if (!maybe_badge || size < 3) {
		return NULL;
	}
	for (int i = 0; i < BADGE_POOL; i++) {
		if (badges[i].used && &badges[i].dsc == maybe_badge) {
			lv_color_t c = {(uint8_t)(badges[i].color & 0xFF),
							(uint8_t)((badges[i].color >> 8) & 0xFF),
							(uint8_t)((badges[i].color >> 16) & 0xFF)};
			return glass_icon_badge(badges[i].glyph, size, c);
		}
	}
	return NULL;
}

void glass_icons_clear(void) {
	for (int i = 0; i < BADGE_POOL; i++) {
		if (badges[i].used) {
			free(badges[i].buf);
			memset(&badges[i], 0, sizeof(badges[i]));
		}
	}
}

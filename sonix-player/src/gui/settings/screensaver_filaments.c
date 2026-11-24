// SPDX-License-Identifier: GPL-3.0-only
// Ribbon, tunnel and spark effects adapted from Joseph Malam's Sonix Visualiser
// (sonix-visualiser, 052863c6ef4f549bc21d2a12354739aa179ccb6f).
// Modified 2026-11-23: screensaver lifecycle, shared PCM analysis and V1 draw budget.
#include "screensaver_filaments.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/audio/visualizer.h"

#define TICK_MS 50
#define MOTION_STEP_MS 33.0f
#define MAX_WIDTH 120
#define MAX_HEIGHT 160
#define STEPS 24
#define STRANDS 6
#define GLOW_RIBBONS 3
#define GLOW_LEVELS 5
#define SPARK_COUNT 40
#define TUNNEL_RINGS 5
#define TUNNEL_STEPS 24
#define SUBPIXEL_BITS 8
#define SUBPIXEL (1 << SUBPIXEL_BITS)
#define SINE_SIZE 256
#define TWO_PI 6.28318530718f

static lv_obj_t *image;
static lv_image_dsc_t picture;
static uint16_t *pixels;
static int width, height, view_w, view_h;
static uint32_t frame_at;
static float sine_table[SINE_SIZE];
static float reach;
static float phase, drift, twist, hue, beat, burst_angle, bass_floor, previous_low;
static float shape_phase, starting_hue = -1;
static float tunnel_travel;
static float bass, mid, treble;
static float target_bass, target_mid, target_treble, pulse;
static unsigned beat_cooldown;
static uint32_t random_state;

typedef struct {
	float distance, angle, speed, glow, age;
} spark_t;

static spark_t sparks[SPARK_COUNT];
static unsigned next_spark;
static float spark_budget;

// A table keeps trigonometry out of the inner drawing loop on the V1. Angles
// are measured in turns so a quarter turn also gives cosine.
static float wave(float turns) {
	float index = turns * SINE_SIZE;
	int base = (int)index;
	if (index < base) {
		base--;
	}
	unsigned slot = (unsigned)base & (SINE_SIZE - 1);
	float fraction = index - base;
	return sine_table[slot] + (sine_table[(slot + 1) & (SINE_SIZE - 1)] - sine_table[slot]) * fraction;
}

static float random_unit(void) {
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return (random_state >> 8) * (1.0f / 16777216.0f);
}

static float next_starting_hue(void) {
	float sample = random_unit();
	if (starting_hue < 0) {
		starting_hue = sample;
	} else {
		// Leave a gap around the last opening colour so a new palette is
		// apparent even when the next random sample would be very close.
		starting_hue += .125f + .75f * sample;
		if (starting_hue >= 1) starting_hue -= 1;
	}
	return starting_hue;
}

static float follow(float current, float target, float elapsed, float rise, float fall) {
	float smoothing = elapsed / (elapsed + (target > current ? rise : fall));
	return current + (target - current) * smoothing;
}

static void read_levels(void) {
	if (beat_cooldown > 0) beat_cooldown--;
	float levels[3];
	visualizer_motion_levels(levels);
	float low = levels[0], middle = levels[1], high = levels[2];
	target_bass = low;
	target_mid = middle;
	target_treble = high;
	bass_floor += (low - bass_floor) * .075f;

	// React to the leading edge of a bass hit. A held note feeds a ribbon
	// without repeatedly firing a burst.
	float attack = low - previous_low;
	if (!beat_cooldown && low > .095f && attack > .055f && low > bass_floor * 1.18f) {
		beat = fminf(.30f + low * .52f + attack, 1.0f);
		burst_angle = random_unit();
		beat_cooldown = 4;
	}
	previous_low = low;
}

static void advance_motion(float elapsed) {
	// PCM arrives in batches. Ease toward the latest levels on every drawing
	// tick so a new batch doesn't suddenly bend or stretch the whole picture.
	bass = follow(bass, target_bass, elapsed, 60, 145);
	mid = follow(mid, target_mid, elapsed, 95, 180);
	treble = follow(treble, target_treble, elapsed, 75, 150);
	pulse = follow(pulse, beat, elapsed, 52, 100);
	beat *= 125.0f / (125.0f + elapsed);

	float step = elapsed / MOTION_STEP_MS;
	// Keep the overall rotation gentle while bends and forward travel retain
	// their pace and music response.
	phase += (.00075f + .0011f * mid + .00075f * pulse) * step;
	drift += (.0012f + .0011f * bass) * step;
	twist += (.0019f + .0014f * treble) * step;
	shape_phase += (.0032f + .0013f * mid) * step;
	tunnel_travel += (.017f + .009f * bass + .008f * pulse) * step;
	hue += elapsed / 48000.0f;
	if (phase >= 1) phase -= 1;
	if (drift >= 1) drift -= 1;
	if (twist >= 1) twist -= 1;
	if (shape_phase >= 1) shape_phase -= 1;
	if (hue >= 1) hue -= 1;
	if (tunnel_travel >= 1) tunnel_travel -= 1;
}

static unsigned rgb565(unsigned r, unsigned g, unsigned b) {
	return (r << 11) | (g << 5) | b;
}

static float palette_hue(float tone) {
	// Give red, orange, yellow, green, blue, purple and pink equal time.
	// A linear HSV sweep otherwise gives the broad green/cyan region much
	// more time than orange and pink, and green is brighter to the eye.
	static const float hues[] = {0, 30, 60, 120, 240, 275, 325, 360};
	tone -= floorf(tone);
	float position = tone * 7;
	int segment = (int)position;
	return hues[segment] + (hues[segment + 1] - hues[segment]) * (position - segment);
}

static uint16_t filament_color(float tone, float saturation, float level) {
	lv_color_t rgb = lv_color_hsv_to_rgb((uint16_t)palette_hue(tone), (uint8_t)(saturation * 100),
										 (uint8_t)(level * 100));
	return lv_color_to_u16(rgb);
}

static void blend_pixel(uint16_t *dst, uint16_t ink) {
	unsigned old = *dst;
	unsigned r = (old >> 11) + (ink >> 11);
	unsigned g = ((old >> 5) & 63) + ((ink >> 5) & 63);
	unsigned b = (old & 31) + (ink & 31);
	*dst = (uint16_t)rgb565(r > 31 ? 31 : r, g > 63 ? 63 : g, b > 31 ? 31 : b);
}

static void point(int x, int y, uint16_t ink) {
	if ((unsigned)x < (unsigned)width && (unsigned)y < (unsigned)height) {
		blend_pixel(&pixels[y * width + x], ink);
	}
}

static void line(int x, int y, int end_x, int end_y, uint16_t ink) {
	if ((x < 0 && end_x < 0) || (x >= width && end_x >= width) ||
		(y < 0 && end_y < 0) || (y >= height && end_y >= height)) {
		return;
	}
	int dx = abs(end_x - x), dy = -abs(end_y - y);
	int sx = x < end_x ? 1 : -1;
	int sy = y < end_y ? 1 : -1;
	int error = dx + dy;
	while (x != end_x || y != end_y) {
		point(x, y, ink);
		int twice = 2 * error;
		if (twice >= dy) {
			error += dy;
			x += sx;
		}
		if (twice <= dx) {
			error += dx;
			y += sy;
		}
	}
}

typedef struct {
	int x, y; // framebuffer coordinates with 8 fractional bits
} subpixel_point_t;

static void point_coverage(bool steep, int axis, int across, uint16_t ink, unsigned coverage) {
	unsigned color = rgb565(((ink >> 11) * coverage + SUBPIXEL / 2) >> SUBPIXEL_BITS,
							(((ink >> 5) & 63) * coverage + SUBPIXEL / 2) >> SUBPIXEL_BITS,
							((ink & 31) * coverage + SUBPIXEL / 2) >> SUBPIXEL_BITS);
	if (color) {
		point(steep ? across : axis, steep ? axis : across, (uint16_t)color);
	}
}

// Walk the dominant axis and divide the line's light between its two nearest
// pixels. Fractional endpoints keep adjoining segments from drawing a full
// pixel twice. Everything blends into the existing RGB565 trails.
static void line_aa(subpixel_point_t start, subpixel_point_t end, uint16_t ink) {
	bool steep = abs(end.y - start.y) > abs(end.x - start.x);
	int a = steep ? start.y : start.x, b = steep ? end.y : end.x;
	int u = steep ? start.x : start.y, v = steep ? end.x : end.y;
	int limit = steep ? height : width;
	int across_limit = steep ? width : height;
	if (!ink || a == b || (u <= -SUBPIXEL && v <= -SUBPIXEL) ||
		(u >= across_limit * SUBPIXEL && v >= across_limit * SUBPIXEL)) {
		return;
	}
	if (a > b) {
		int tmp = a;
		a = b;
		b = tmp;
		tmp = u;
		u = v;
		v = tmp;
	}
	int first = (a + SUBPIXEL / 2) >> SUBPIXEL_BITS;
	int last = (b + SUBPIXEL / 2) >> SUBPIXEL_BITS;
	if (last < 0 || first >= limit) {
		return;
	}
	if (first < 0) first = 0;
	if (last >= limit) last = limit - 1;

	int slope = (v - u) * SUBPIXEL / (b - a);
	int minor = u * SUBPIXEL + slope * (first * SUBPIXEL - a);
	for (int i = first; i <= last; i++, minor += slope * SUBPIXEL) {
		int center = i * SUBPIXEL;
		int left = center - SUBPIXEL / 2;
		int right = center + SUBPIXEL / 2;
		if (left < a) left = a;
		if (right > b) right = b;
		unsigned weight = (unsigned)(right - left);
		int sample = (minor + slope * ((right + left) / 2 - center)) >> SUBPIXEL_BITS;
		int cross = sample >> SUBPIXEL_BITS;
		unsigned fraction = (unsigned)sample & (SUBPIXEL - 1);
		unsigned upper = (fraction * weight + SUBPIXEL / 2) >> SUBPIXEL_BITS;
		point_coverage(steep, i, cross, ink, weight - upper);
		point_coverage(steep, i, cross + 1, ink, upper);
	}
}

static void fade_trails(void) {
	for (int i = 0; i < width * height; i++) {
		unsigned p = pixels[i];
		pixels[i] = (uint16_t)rgb565((p >> 11) * 15 >> 4, ((p >> 5) & 63) * 15 >> 4, (p & 31) * 15 >> 4);
	}
}

static subpixel_point_t polar_point(float distance, float angle) {
	float radius = distance * reach * SUBPIXEL;
	return (subpixel_point_t){
		.x = width / 2 * SUBPIXEL + (int)(radius * wave(angle + .25f)),
		.y = height / 2 * SUBPIXEL + (int)(radius * wave(angle)),
	};
}

static void draw_tunnel(void) {
	// Equally spaced cross-sections in depth grow faster as they approach
	// the viewer. Both ends fade so recycling a section never flashes.
	for (int i = TUNNEL_RINGS; i > 0; i--) {
		float depth = i - tunnel_travel;
		// Depth decreases as we move forward. Nearby sections expand beyond
		// the edges, with a shared vanishing point for the distant sections.
		float distance = .68f / (.30f + depth);
		float fade = fminf(1, fminf(depth * 3.4f, (TUNNEL_RINGS - depth) * 1.6f));
		float turn = phase + .04f * wave(twist) - .02f * depth;
		int offset_x = (int)(reach * SUBPIXEL * distance * .105f * wave(drift));
		int offset_y = (int)(reach * SUBPIXEL * distance * .076f * wave(drift + .25f));
		uint16_t ink[3];
		for (int j = 0; j < 3; j++) {
			float level = fade * (.17f + .06f * bass) * (.58f + .21f * j);
			ink[j] = filament_color(hue + .07f + .044f * depth, .64f, level);
		}
		subpixel_point_t previous = {0};
		for (int j = 0; j <= TUNNEL_STEPS; j++) {
			float angle = (float)j / TUNNEL_STEPS;
			float shape = 1 + .17f * wave(2 * angle + twist) + .065f * wave(3 * angle + drift);
			subpixel_point_t p = polar_point(distance * shape, angle + turn);
			p.x += offset_x;
			p.y += offset_y;
			if (j > 0) {
				// Uneven illumination lets pieces of each cross-section
				// surface through the ribbons without dominating them.
				float light = wave(angle + drift + .19f * depth);
				int shade = light > .15f ? 2 : (light < -.5f ? 0 : 1);
				line_aa(previous, p, ink[shade]);
			}
			previous = p;
		}
	}
}

// Stamp a feathered brush directly into the RGB565 trails. This gives the
// broad layer a soft edge without a second framebuffer or a full-screen blur.
static void glow_dab(int x, int y, int radius, const uint16_t ink[GLOW_LEVELS]) {
	if (x + radius < 0 || x - radius >= width || y + radius < 0 || y - radius >= height) {
		return;
	}
	int r2 = radius * radius;
	unsigned scale = GLOW_LEVELS * 1024 / r2;
	int left = x - radius < 0 ? -x : -radius;
	int right = x + radius >= width ? width - 1 - x : radius;
	int top = y - radius < 0 ? -y : -radius;
	int bottom = y + radius >= height ? height - 1 - y : radius;
	for (int dy = top; dy <= bottom; dy++) {
		uint16_t *dst = &pixels[(y + dy) * width + x + left];
		for (int dx = left; dx <= right; dx++, dst++) {
			int d2 = dx * dx + dy * dy;
			if (d2 < r2) {
				uint16_t color = ink[(d2 * scale) >> 10];
				if (color) {
					blend_pixel(dst, color);
				}
			}
		}
	}
}

static void glow_segment(int x, int y, int nx, int ny, int radius, const uint16_t ink[GLOW_LEVELS]) {
	int dx = nx - x, dy = ny - y;
	int length = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
	int stamps = length / (radius + 1) + 1;
	for (int i = 0; i < stamps; i++) {
		glow_dab(x + dx * i / stamps, y + dy * i / stamps, radius, ink);
	}
}

static float zigzag(float position, float peak, float rise, float fall) {
	float fraction = position - (int)position;
	return (fraction < peak ? fraction * rise : (1 - fraction) * fall) - 1;
}

static void draw_ribbons(bool soft) {
	// Each layer has independently drifting bends and unevenly spaced lanes.
	// Paths can reverse their curl while the whole field keeps moving outwards.
	float bend[STEPS + 1], flare[STEPS + 1], ripple[STEPS + 1];
	for (int i = 0; i <= STEPS; i++) {
		float t = (float)i / STEPS;
		float curl = (.31f + .48f * wave(drift) + .19f * mid) * t;
		bend[i] = curl + (.16f + .17f * mid) * t * wave(t * 1.23f + twist);
		flare[i] = wave(t * .5f);
		ripple[i] = t * wave(t * 2.45f - twist * 2) * (.011f + treble * .024f);
	}

	int count = soft ? GLOW_RIBBONS : STRANDS;
	for (int strand = 0; strand < count; strand++) {
		float lane = ((float)strand + (soft ? .58f : 0)) / count;
		float base = lane + phase + .16f * wave(lane * 2 + drift);
		float fold = wave(lane * 4 + twist);
		float lean = wave(lane * 3 - drift);
		// Each ribbon eases into a different zigzag. The corners travel
		// outward with the ripple, and the uneven teeth drift in shape.
		float contour = fmaxf(0, wave(shape_phase + .56f * lane + (soft ? .12f : 0)));
		float zigzag_mix = contour * contour;
		float teeth = 2.65f + .75f * lean;
		float tooth_phase = 3 + lane - 2 * twist;
		float peak = .48f + .18f * fold;
		float rise = 2 / peak, fall = 2 / (1 - peak);
		float zigzag_depth = (soft ? .039f : .052f) + .010f * treble;
		float reach_scale = 1.04f + .17f * lean + .10f * pulse;
		float tone = hue + .27f * lane + (soft ? 0 : .07f);
		uint16_t halo[GLOW_LEVELS];
		uint16_t core = 0;
		if (soft) {
			float glow = .45f + .20f * bass + .22f * pulse;
			for (int level = 0; level < GLOW_LEVELS; level++) {
				float opacity = 1 - (float)level / GLOW_LEVELS;
				halo[level] = filament_color(tone, .85f, glow * opacity * opacity * .42f);
			}
		} else {
			core = filament_color(tone, .72f, .30f + .24f * mid + .17f * pulse);
		}

		// Stagger the starts so the crossing paths never draw a fixed centre
		// emblem. Wider strokes open up farther from the centre.
		float start = (soft ? .065f : .013f) + (soft ? .15f : .095f) * (.5f + .5f * fold);
		subpixel_point_t previous = polar_point(start, base);
		for (int i = 1; i <= STEPS; i++) {
			float t = (float)i / STEPS;
			float distance = start + t * (reach_scale - start);
			float detail = ripple[i];
			if (zigzag_mix > 0) {
				float angular = t * zigzag_depth * zigzag(t * teeth + tooth_phase, peak, rise, fall);
				detail += zigzag_mix * (angular - detail);
			}
			float angle = base + bend[i] * (.72f + .52f * lean) +
						  fold * .17f * t * flare[i] + detail;
			subpixel_point_t p = polar_point(distance, angle);
			if (soft) {
				float spread = .5f + .5f * wave(lane * 2 + drift + t * .87f);
				int radius = 2 + (int)(flare[i] * (2 + 4 * spread + 3 * bass + 2 * pulse));
				glow_segment(previous.x >> SUBPIXEL_BITS, previous.y >> SUBPIXEL_BITS,
							 p.x >> SUBPIXEL_BITS, p.y >> SUBPIXEL_BITS, radius, halo);
			} else {
				line_aa(previous, p, core);
			}
			previous = p;
		}
	}
}

static void draw_sparks(float step) {
	spark_budget += (4 * treble + 5 * pulse) * step;
	int count = (int)spark_budget;
	spark_budget -= count;
	for (int i = 0; i < count; i++) {
		float angle = random_unit();
		if (pulse > .16f && (i & 1)) {
			angle = burst_angle + (angle - .5f) * .30f;
		}
		sparks[next_spark] = (spark_t){
			.distance = .022f + random_unit() * .13f,
			.angle = angle,
			.speed = .012f + random_unit() * .023f + .020f * pulse,
			.glow = .46f + random_unit() * .24f + .28f * treble,
		};
		next_spark = (next_spark + 1) % SPARK_COUNT;
	}
	for (unsigned i = 0; i < SPARK_COUNT; i++) {
		spark_t *spark = &sparks[i];
		if (spark->glow < .035f || spark->distance >= 1.13f) {
			continue;
		}
		subpixel_point_t start = polar_point(spark->distance, spark->angle);
		spark->distance += spark->speed * step;
		spark->angle += (.007f + .018f * mid * wave(spark->angle * 2 + phase)) * step;
		spark->age += step;
		subpixel_point_t end = polar_point(spark->distance, spark->angle);
		float intensity = spark->glow * fminf(spark->age * .33f, 1.0f);
		uint16_t ink = filament_color(hue + .18f + .15f * spark->angle, .60f, intensity);
		line(start.x >> SUBPIXEL_BITS, start.y >> SUBPIXEL_BITS,
			 end.x >> SUBPIXEL_BITS, end.y >> SUBPIXEL_BITS, ink);
		spark->glow *= 1.0f / (1.0f + .075f * step);
	}
}

void filaments_release(void) {
	if (pixels) {
		lv_image_set_src(image, NULL);
		lv_obj_set_hidden(image, true);
		lv_image_cache_drop(&picture);
		free(pixels);
		pixels = NULL;
	}
	view_w = view_h = 0;
}

static void begin_frame(lv_obj_t *parent, int area_w, int area_h) {
	filaments_release();
	view_w = area_w;
	view_h = area_h;
	// At most 37.5 KiB: render at half the V1's panel resolution and cap
	// the work even on bigger panels. LVGL stretches this one opaque image.
	int scale = LV_MAX(2, LV_MAX((area_w + MAX_WIDTH - 1) / MAX_WIDTH,
								(area_h + MAX_HEIGHT - 1) / MAX_HEIGHT));
	width = LV_MAX(1, area_w / scale);
	height = LV_MAX(1, area_h / scale);
	pixels = calloc((size_t)width * height, sizeof(*pixels));
	if (!pixels) return;

	if (!image) {
		image = lv_image_create(parent);
		lv_obj_set_clickable(image, false);
		lv_obj_set_scrollable(image, false);
		lv_image_set_inner_align(image, LV_IMAGE_ALIGN_STRETCH);
		// The thin paths already have coverage AA; avoid a second filter
		// over the full upscaled framebuffer on every draw.
		lv_image_set_antialias(image, false);
		for (int i = 0; i < SINE_SIZE; i++) sine_table[i] = sinf(i * (TWO_PI / SINE_SIZE));
		random_state = (0xcf624a18u ^ lv_tick_get()) | 1u;
	}
	picture = (lv_image_dsc_t){
		.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
				   .w = width, .h = height, .stride = width * sizeof(*pixels)},
		.data_size = (uint32_t)(width * height * sizeof(*pixels)),
		.data = (const uint8_t *)pixels,
	};
	lv_image_set_src(image, &picture);
	lv_obj_set_size(image, area_w, area_h);
	lv_obj_set_hidden(image, false);

	reach = .54f * sqrtf((float)width * width + (float)height * height);
	phase = random_unit();
	drift = random_unit();
	twist = random_unit();
	shape_phase = random_unit();
	hue = next_starting_hue();
	tunnel_travel = 0;
	bass = mid = treble = target_bass = target_mid = target_treble = 0;
	beat = pulse = bass_floor = previous_low = spark_budget = 0;
	beat_cooldown = next_spark = 0;
	memset(sparks, 0, sizeof(sparks));
	frame_at = lv_tick_get() - TICK_MS;
}

void filaments_draw(lv_obj_t *parent, int area_w, int area_h, unsigned layers) {
	if (view_w != area_w || view_h != area_h) begin_frame(parent, area_w, area_h);
	if (!pixels) return;
	uint32_t now = lv_tick_get();
	uint32_t elapsed = now - frame_at;
	frame_at = now;
	if (!elapsed || elapsed > 2 * TICK_MS) elapsed = TICK_MS;

	read_levels();
	advance_motion(elapsed);
	fade_trails();
	if (layers & FILAMENTS_TUNNEL) draw_tunnel();
	if (layers & FILAMENTS_SOFT) draw_ribbons(true);
	if (layers & FILAMENTS_SHARP) draw_ribbons(false);
	if (layers & FILAMENTS_SPARKS) draw_sparks(elapsed / MOTION_STEP_MS);
	lv_obj_invalidate(image);
}

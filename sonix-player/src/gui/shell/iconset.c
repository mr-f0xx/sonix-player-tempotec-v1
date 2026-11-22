/*
 * SD icon sets -- whole-interface artwork chosen from Appearance.
 *
 * A set is a folder under Icons/ on the card: <id>.png files named after the
 * built-in glyph ids in icons.h, plus an optional palette.ini giving the seven
 * function roles their colours. Loading one replaces the pixels behind those
 * descriptors in place, so every screen that already points at them changes
 * with it -- no call sites, no recompilation, nothing baked into the firmware.
 *
 * What the UI paints on top still applies: the recolor that tints idle icons
 * to the text colour is dropped for swapped artwork only (see
 * sonix_iconset_recolor_bypass, called from LVGL's draw descriptor init via
 * patches/lvgl-iconset-recolor.patch), so a set's own colours show through,
 * while accent and state recolors keep painting their states as before.
 *
 * Files are decoded once at load with the same stb_image the covers use, and
 * downscaled to each descriptor's size; the originals are kept so unloading
 * (or a failed load) puts the built-in bitmaps back exactly.
 */

#include "iconset.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/shell/icons.h"
#include "src/system/core/config.h"
#include "src/system/image/stb_image_decl.h"

// For lv_image_t's src field: the widget struct itself is private to LVGL,
// and src/ includes private headers like this one elsewhere (see gui.c).
#include "lvgl/src/widgets/image/lv_image_private.h"

#define ICONSET_FOLDER "Icons"
#define ICONSET_SETS_MAX 8	   // folders offered in Appearance
#define ICONSET_NAME_MAX 64
#define ICONSET_PATH_MAX 320

// palette.ini's seven keys, in theme_semantic_t order (theme.h).
static const char *const ROLE_KEY[7] = {
	"blue", "cyan", "purple", "green", "amber", "red", "muted",
};

typedef struct {
	lv_color_t role[7];
	bool has_role[7];
} palette_t;

typedef struct {
	const void *orig_data; // the built-in bitmap, restored on unload
	uint32_t orig_size;
	void *own;	// this set's pixels; NULL when the set didn't ship the icon
	uint32_t own_size;
} slot_t;

static struct {
	char card_root[ICONSET_PATH_MAX];
	char name[ICONSET_NAME_MAX];
	bool active;
	palette_t palette;
	slot_t *slots; // iconset_entry_count entries while a set is loaded
	// Folder names for iconset_choices(); pointers stay valid until the
	// next call, so the settings row copies what it needs.
	char names[ICONSET_SETS_MAX][ICONSET_NAME_MAX];
} live;

void iconset_set_card_root(const char *root) {
	live.card_root[0] = '\0';
	if (root && root[0]) {
		snprintf(live.card_root, sizeof(live.card_root), "%s", root);
	}
}

bool iconset_active(void) { return live.active; }

const char *iconset_active_name(void) { return live.active ? live.name : ""; }

/* ---------------------------------------------------------------------- */
/* palette.ini                                                            */
/* ---------------------------------------------------------------------- */

static bool parse_hex(const char *s, lv_color_t *out) {
	if (!s || s[0] != '#' || strlen(s) != 7) {
		return false;
	}
	char *end = NULL;
	long v = strtol(s + 1, &end, 16);
	if (!end || *end != '\0' || v < 0) {
		return false;
	}
	*out = lv_color_make((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
	return true;
}

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
		s++;
	}
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = '\0';
	return s;
}

// key = #rrggbb lines; ';' comments. A missing file is fine (every role then
// falls back to the theme's own colour); a malformed line is skipped.
static void load_palette(const char *path, palette_t *pal) {
	memset(pal, 0, sizeof(*pal));
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (*s == '\0' || *s == ';' || *s == '#') {
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char *key = trim(s);
		char *value = trim(eq + 1);
		for (int r = 0; r < 7; r++) {
			if (strcmp(key, ROLE_KEY[r]) == 0 && parse_hex(value, &pal->role[r])) {
				pal->has_role[r] = true;
			}
		}
	}
	fclose(f);
}

/* ---------------------------------------------------------------------- */
/* decoding and scaling                                                   */
/* ---------------------------------------------------------------------- */

static unsigned char *read_file(const char *path, size_t *size_out) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long size = ftell(f);
	if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	unsigned char *buf = malloc((size_t)size);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size) {
		free(buf);
		return NULL;
	}
	*size_out = (size_t)size;
	return buf;
}

// The set's RGBA pixels to LVGL's BGRA at the descriptor's size. Sets are
// rendered at 4x, so this is normally a box average -- premultiplied, so the
// soft edges of the artwork don't average colour in from nothing. An exact
// size is copied, and anything smaller is taken nearest-neighbour so a
// hand-made file still loads. The result is what the dsc keeps; free it with
// free().
static void *scale_to(const unsigned char *px, int w, int h, int tw, int th, uint32_t *out_size) {
	if (tw <= 0 || th <= 0) {
		return NULL;
	}
	size_t out_n = (size_t)tw * (size_t)th * 4;
	unsigned char *out = malloc(out_n);
	if (!out) {
		return NULL;
	}

	if (w == tw && h == th) {
		for (size_t i = 0; i < (size_t)tw * (size_t)th; i++) {
			out[i * 4 + 0] = px[i * 4 + 2];
			out[i * 4 + 1] = px[i * 4 + 1];
			out[i * 4 + 2] = px[i * 4 + 0];
			out[i * 4 + 3] = px[i * 4 + 3];
		}
	} else if (w >= tw && h >= th) {
		for (int oy = 0; oy < th; oy++) {
			int y0 = oy * h / th;
			int y1 = (oy + 1) * h / th;
			if (y1 <= y0) {
				y1 = y0 + 1;
			}
			for (int ox = 0; ox < tw; ox++) {
				int x0 = ox * w / tw;
				int x1 = (ox + 1) * w / tw;
				if (x1 <= x0) {
					x1 = x0 + 1;
				}
				uint32_t sr = 0, sg = 0, sb = 0, sa = 0;
				uint32_t n = 0;
				for (int y = y0; y < y1; y++) {
					for (int x = x0; x < x1; x++) {
						const unsigned char *p = px + ((size_t)y * w + x) * 4;
						uint32_t a = p[3];
						sr += p[0] * a;
						sg += p[1] * a;
						sb += p[2] * a;
						sa += a;
						n++;
					}
				}
				unsigned char *q = out + ((size_t)oy * tw + ox) * 4;
				if (sa == 0) {
					q[0] = q[1] = q[2] = q[3] = 0;
				} else {
					q[0] = (unsigned char)(sr / sa); // red, unpremultiplied
					q[1] = (unsigned char)(sg / sa);
					q[2] = (unsigned char)(sb / sa);
					q[3] = (unsigned char)(sa / n);
				}
				// to BGRA
				unsigned char t = q[0];
				q[0] = q[2];
				q[2] = t;
			}
		}
	} else {
		for (int oy = 0; oy < th; oy++) {
			int sy = oy * h / th;
			if (sy >= h) {
				sy = h - 1;
			}
			for (int ox = 0; ox < tw; ox++) {
				int sx = ox * w / tw;
				if (sx >= w) {
					sx = w - 1;
				}
				const unsigned char *p = px + ((size_t)sy * w + sx) * 4;
				unsigned char *q = out + ((size_t)oy * tw + ox) * 4;
				q[0] = p[2];
				q[1] = p[1];
				q[2] = p[0];
				q[3] = p[3];
			}
		}
	}

	*out_size = (uint32_t)out_n;
	return out;
}

/* ---------------------------------------------------------------------- */
/* load / unload                                                          */
/* ---------------------------------------------------------------------- */

// Reads one set into `slots` and swaps it in, keeping the built-ins in the
// slots so unload puts them back. Failure changes nothing: the previous set
// (or the built-ins) stay showing.
static bool set_load(const char *name) {
	// Worst case: a full card root, /Icons/, a full set name.
	char dir[ICONSET_PATH_MAX + ICONSET_NAME_MAX + 16];
	snprintf(dir, sizeof(dir), "%s/%s/%s", live.card_root, ICONSET_FOLDER, name);
	DIR *d = opendir(dir);
	if (!d) {
		return false;
	}
	closedir(d);

	palette_t pal;
	char palette_path[sizeof(dir) + 16];
	snprintf(palette_path, sizeof(palette_path), "%s/palette.ini", dir);
	load_palette(palette_path, &pal);

	int count = iconset_entry_count;
	slot_t *slots = calloc((size_t)count, sizeof(slot_t));
	if (!slots) {
		return false;
	}

	// The built-ins are wherever they are now: swapped out by the previous
	// set (its slots remember them) or still in place.
	int shipped = 0;
	for (int i = 0; i < count; i++) {
		const iconset_entry_t *e = &iconset_entries[i];
		if (live.active && live.slots[i].own) {
			slots[i].orig_data = live.slots[i].orig_data;
			slots[i].orig_size = live.slots[i].orig_size;
		} else {
			slots[i].orig_data = e->dsc->data;
			slots[i].orig_size = e->dsc->data_size;
		}

		char path[sizeof(dir) + 80];
		// %.64s: the ids are short names from icons.h; the precision also
		// keeps the path builder provably inside the buffer.
		snprintf(path, sizeof(path), "%s/%.64s.png", dir, e->id);
		size_t file_size = 0;
		unsigned char *file = read_file(path, &file_size);
		if (!file) {
			continue; // not shipped: this icon keeps its built-in bitmap
		}
		int w = 0, h = 0, n = 0;
		unsigned char *px = stbi_load_from_memory(file, (int)file_size, &w, &h, &n, 4);
		free(file);
		if (!px || w <= 0 || h <= 0) {
			stbi_image_free(px);
			continue;
		}
		uint32_t out_size = 0;
		void *buf = scale_to(px, w, h, (int)e->dsc->header.w, (int)e->dsc->header.h, &out_size);
		stbi_image_free(px);
		if (!buf) {
			continue;
		}
		slots[i].own = buf;
		slots[i].own_size = out_size;
		shipped++;
	}

	if (shipped == 0) {
		for (int i = 0; i < count; i++) {
			free(slots[i].own);
		}
		free(slots);
		return false;
	}

	// Commit: the new pixels go in (or the built-ins come back for icons this
	// set didn't ship), the previous set's buffers are freed.
	for (int i = 0; i < count; i++) {
		const iconset_entry_t *e = &iconset_entries[i];
		if (slots[i].own) {
			e->dsc->data = slots[i].own;
			e->dsc->data_size = slots[i].own_size;
		} else {
			e->dsc->data = slots[i].orig_data;
			e->dsc->data_size = slots[i].orig_size;
		}
	}
	if (live.slots) {
		for (int i = 0; i < count; i++) {
			free(live.slots[i].own);
		}
		free(live.slots);
	}
	live.slots = slots;
	live.palette = pal;
	live.active = true;
	snprintf(live.name, sizeof(live.name), "%s", name);
	return true;
}

static void set_unload(void) {
	if (live.slots) {
		for (int i = 0; i < iconset_entry_count; i++) {
			if (live.slots[i].own) {
				iconset_entries[i].dsc->data = live.slots[i].orig_data;
				iconset_entries[i].dsc->data_size = live.slots[i].orig_size;
				free(live.slots[i].own);
			}
		}
		free(live.slots);
		live.slots = NULL;
	}
	live.active = false;
	live.name[0] = '\0';
	memset(&live.palette, 0, sizeof(live.palette));
}

bool iconset_select(const char *name) {
	if (!name || name[0] == '\0') {
		set_unload();
		return true;
	}
	// A folder name goes into a path: no separators, no climbing out.
	if (strchr(name, '/') || strchr(name, '\\') || strcmp(name, "..") == 0) {
		return false;
	}
	if (!live.card_root[0]) {
		return false;
	}
	if (live.active && strcmp(live.name, name) == 0) {
		return true;
	}
	return set_load(name);
}

int iconset_choices(const char **names, int max) {
	int n = 0;
	if (!live.card_root[0] || max <= 0) {
		return 0;
	}
	char dir[ICONSET_PATH_MAX + 16];
	snprintf(dir, sizeof(dir), "%s/%s", live.card_root, ICONSET_FOLDER);
	DIR *d = opendir(dir);
	if (!d) {
		return 0;
	}
	struct dirent *ent;
	while (n < max && n < ICONSET_SETS_MAX && (ent = readdir(d)) != NULL) {
		size_t len = strlen(ent->d_name);
		// No hidden folders, nothing with a path in it, and nothing longer
		// than the name buffer can hold -- a name that does not fit cannot
		// be offered as a choice.
		if (len == 0 || len >= ICONSET_NAME_MAX || ent->d_name[0] == '.' ||
			strchr(ent->d_name, '/') || strchr(ent->d_name, '\\')) {
			continue;
		}
		char sub[sizeof(dir) + 272];
		snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name);
		DIR *check = opendir(sub);
		if (!check) {
			continue; // files in Icons/ are not sets
		}
		closedir(check);
		memcpy(live.names[n], ent->d_name, len + 1);
		names[n] = live.names[n];
		n++;
	}
	closedir(d);
	return n;
}

bool iconset_semantic_color(theme_semantic_t role, lv_color_t *out) {
	if (!live.active || (int)role < 0 || (int)role > THEME_SEMANTIC_MUTED) {
		return false;
	}
	if (!live.palette.has_role[role]) {
		return false;
	}
	*out = live.palette.role[role];
	return true;
}

/* ---------------------------------------------------------------------- */
/* the draw hook                                                          */
/* ---------------------------------------------------------------------- */

// Called from the patched lv_obj_init_draw_image_dsc() for every image that
// carries a recolor. For artwork this module swapped in, the idle tint (the
// flat text colour the whole UI paints its icons with) gives way to the
// pixels; every other recolor -- accent, a semantic state, an active tab --
// passes through untouched so states keep working.
lv_opa_t sonix_iconset_recolor_bypass(lv_obj_t *obj, lv_color_t recolor, lv_opa_t opa) {
	if (!live.active || opa <= LV_OPA_MIN) {
		return opa;
	}
	// Light themes keep their flat tint: a set's palette is drawn for a dark
	// surface, and the UI's light-theme icons are dark-on-light.
	if (!theme_is_dark()) {
		return opa;
	}
	if (!lv_color_eq(recolor, theme()->text_primary)) {
		return opa;
	}
	// lv_image_t lays src right after the base object; anything else that
	// reaches here reads a different field, which matches no descriptor and
	// simply keeps its recolor.
	const void *src = ((lv_image_t *)obj)->src;
	if (!src) {
		return opa;
	}
	for (int i = 0; i < iconset_entry_count; i++) {
		if (live.slots[i].own && (const void *)iconset_entries[i].dsc == src) {
			return LV_OPA_TRANSP;
		}
	}
	return opa;
}

/* ---------------------------------------------------------------------- */
/* startup                                                                */
/* ---------------------------------------------------------------------- */

void iconset_init(void) {
	// Publish the set to the theme before anything asks about glyph modes.
	theme_set_icon_sources(iconset_active, iconset_semantic_color);

	const char *saved = config_get("ui", "icon_set", "");
	if (saved && saved[0]) {
		// A set that has gone missing from the card is not an error at boot:
		// the built-ins show, the saved name stays, and the folder loading
		// again brings the choice back on its own.
		(void)iconset_select(saved);
	}
}

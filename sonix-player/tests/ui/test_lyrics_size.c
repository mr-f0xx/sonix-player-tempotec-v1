// Drive the lyrics appearance and timing menus on a 240x320 LVGL display. Only
// the playback shell and FreeType faces are replaced by test fixtures.
#include <assert.h>
#include <unistd.h>

#include "../../src/gui/nowplaying/player.c"

lv_font_t font_ui_14, font_ui_24;
lv_style_t theme_style_icon, theme_style_card, theme_style_card_pressed, theme_style_text;
static const theme_palette_t palette;

const theme_palette_t *theme(void) { return &palette; }
bool theme_is_monochrome(void) { return false; }
bool theme_is_tokyo_night(void) { return false; }
lv_color_t theme_player_chrome_color(theme_player_chrome_t role) {
	(void)role;
	return palette.accent;
}
int back_btn_centre_y(void) { return 42; }

const lv_font_t *fonts_lyrics(int size) {
	static const lv_font_t *const fonts[] = {
		[FONTS_TEXT_SMALL] = &lv_font_montserrat_14,
		[FONTS_TEXT_NORMAL] = &lv_font_montserrat_20,
		[FONTS_TEXT_LARGE] = &lv_font_montserrat_28,
	};
	return fonts[size];
}

static device_state_t state;
static int saved_ms, writes;
void device_state_get(device_state_t *out) { *out = state; }
audio_status_t audio_get_status(void) { return AUDIO_STATUS_PLAYING; }
void toast_error(const char *text) { (void)text; assert(false); }
bool library_set_lyrics_offset(const char *path, int ms) {
	assert(strcmp(path, "/card/test.flac") == 0);
	writes++;
	saved_ms = ms;
	return true;
}

static void ignore_search(lv_event_t *e) { (void)e; }

static void check_header(void) {
	lv_area_t search, size, offset;
	lyrics_header_sync(false);
	assert(lv_obj_is_hidden(lyrics_find_btn) && lv_obj_is_hidden(lyrics_size_btn));
	assert(lv_obj_is_hidden(lyrics_offset_btn));
	lyrics_header_sync(true);
	assert(!lv_obj_is_hidden(lyrics_find_btn) && !lv_obj_is_hidden(lyrics_size_btn));
	lv_obj_update_layout(lyrics_pane);
	lv_obj_get_coords(lyrics_find_btn, &search);
	lv_obj_get_coords(lyrics_size_btn, &size);
	assert(search.y1 == size.y1 && search.y2 == size.y2);
	lv_obj_get_coords(lyrics_offset_btn, &offset);
	assert(offset.x2 + 2 * LYRICS_HEADER_HIT_SLOP < size.x1);
	assert(size.x2 + 2 * LYRICS_HEADER_HIT_SLOP < search.x1);
	layout_sticker_now = true;
	lyrics_header_sync(true);
	lv_obj_update_layout(lyrics_pane);
	lv_obj_get_coords(lyrics_find_btn, &search);
	lv_obj_get_coords(lyrics_size_btn, &size);
	lv_obj_get_coords(lyrics_offset_btn, &offset);
	assert(size.x2 + 2 * LYRICS_HEADER_HIT_SLOP < offset.x1);
	assert(search.x2 + 2 * LYRICS_HEADER_HIT_SLOP < size.x1);
	layout_sticker_now = false;
	lyrics_header_sync(true);
	lv_obj_update_layout(lyrics_pane);
}

static void choose_size(int row, int checked_row) {
	lv_obj_send_event(lyrics_size_btn, LV_EVENT_CLICKED, NULL);
	lv_obj_t *veil = lv_obj_get_child(lv_layer_top(), -1);
	assert(!lv_obj_is_hidden(veil));
	lv_obj_t *card = lv_obj_get_child(veil, 0);
	const char *const names[] = {"Small", "Normal", "Large"};
	for (int i = 0; i < 3; i++) {
		// The card alternates rows and separators, with a tick on the active row.
		lv_obj_t *item = lv_obj_get_child(card, 2 * i);
		assert(strcmp(lv_label_get_text(lv_obj_get_child(item, 0)), names[i]) == 0);
		assert(lv_obj_get_child_count(item) == (i == checked_row ? 2u : 1u));
	}
	lv_obj_send_event(lv_obj_get_child(card, 2 * row), LV_EVENT_CLICKED, NULL);
	assert(lv_obj_is_hidden(veil));
}

static void check_offset(void) {
	strcpy(state.current_file, "/card/test.flac");
	strcpy(lyrics_path, state.current_file);
	state.progress_current_secs = 0.95;
	lyrics_header_sync(true);
	assert(!lv_obj_is_hidden(lyrics_offset_btn));
	lv_obj_send_event(lyrics_offset_btn, LV_EVENT_CLICKED, NULL);
	lv_obj_t *veil = lv_obj_get_child(lv_layer_top(), -1);
	lv_obj_t *card = lv_obj_get_child(veil, 0);
	lv_obj_send_event(lv_obj_get_child(card, 2), LV_EVENT_CLICKED, NULL);
	assert(!lv_obj_is_hidden(veil) && writes == 0 && lyrics_lit == 1);
	assert(lyrics_seek_ms(1000) == 900 && lyrics_seek_ms(0) == 0);
	lv_obj_send_event(lv_obj_get_child(card, 0), LV_EVENT_CLICKED, NULL);
	lv_obj_send_event(lv_obj_get_child(card, 0), LV_EVENT_CLICKED, NULL);
	assert(writes == 0 && lyrics_lit == 0 && lyrics_seek_ms(1000) == 1100);
	assert(strcmp(lv_label_get_text(lv_obj_get_child(lv_obj_get_child(card, 4), 0)), "Reset (-0.1 s)") == 0);
	lv_obj_send_event(veil, LV_EVENT_CLICKED, NULL);
	assert(lv_obj_is_hidden(veil) && writes == 1 && saved_ms == -100);

	// Closing untouched, or undoing the changes before dismissal, writes nothing.
	lv_obj_send_event(lyrics_offset_btn, LV_EVENT_CLICKED, NULL);
	popover_close();
	lv_obj_send_event(lyrics_offset_btn, LV_EVENT_CLICKED, NULL);
	lv_obj_send_event(lv_obj_get_child(card, 2), LV_EVENT_CLICKED, NULL);
	lv_obj_send_event(lv_obj_get_child(card, 0), LV_EVENT_CLICKED, NULL);
	popover_close();
	assert(writes == 1);

	lv_obj_send_event(lyrics_offset_btn, LV_EVENT_CLICKED, NULL);
	lv_obj_send_event(lv_obj_get_child(card, 4), LV_EVENT_CLICKED, NULL);
	assert(lv_obj_is_hidden(veil) && writes == 2 && saved_ms == 0 && lyrics_seek_ms(1000) == 1000);

	// An open menu cannot change the next song when playback advances.
	lv_obj_send_event(lyrics_offset_btn, LV_EVENT_CLICKED, NULL);
	lv_obj_send_event(lv_obj_get_child(card, 2), LV_EVENT_CLICKED, NULL);
	strcpy(state.current_file, "/card/next.flac");
	lv_obj_send_event(lv_obj_get_child(card, 2), LV_EVENT_CLICKED, NULL);
	assert(lv_obj_is_hidden(veil) && writes == 3 && saved_ms == 100);
	popover_close();
	assert(writes == 3);
}

static int label_y(lv_obj_t *label) {
	lv_area_t area;
	lv_obj_get_coords(label, &area);
	return area.y1;
}

int main(int argc, char **argv) {
	assert(argc == 2);
	char dir[] = "/tmp/sonix-lyrics-size-XXXXXX";
	assert(mkdtemp(dir));
	char path[128];
	snprintf(path, sizeof(path), "%s/config.ini", dir);
	(void)config_init(path); // The file does not exist until the first save.
	config_set_int("ui", "text_size", FONTS_TEXT_SMALL);
	lyrics_size_load();
	assert(lyrics_text_size == FONTS_TEXT_NORMAL);

	lv_init();
	lv_display_create(240, 320);
	lang_init(argv[1]);
	assert(lang_set("English"));
	font_ui_14 = lv_font_montserrat_14;
	font_ui_24 = lv_font_montserrat_20;
	lv_style_init(&theme_style_icon);
	lv_style_init(&theme_style_card);
	lv_style_init(&theme_style_card_pressed);
	lv_style_init(&theme_style_text);
	popover_init();

	player_screen = lv_screen_active();
	lv_obj_remove_style_all(player_screen);
	lyrics_pane = lv_obj_create(player_screen);
	lv_obj_remove_style_all(lyrics_pane);
	lv_obj_set_size(lyrics_pane, 240, 320);
	lyrics_view = lv_obj_create(lyrics_pane);
	lv_obj_remove_style_all(lyrics_view);
	lv_obj_set_pos(lyrics_view, 8, 64);
	lv_obj_set_size(lyrics_view, 224, 256);
	lv_obj_set_flex_flow(lyrics_view, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(lyrics_view, LYRICS_LINE_GAP, 0);
	lyrics_view_h = 256;
	lyrics_find_btn = lyrics_header_button(&icon_lyrics_search, 192, ignore_search, &lyrics_find_icon);
	lyrics_size_btn = lyrics_header_button(&icon_ebook_font, 140, lyrics_size_cb, &lyrics_size_icon);
	lyrics_offset_btn = lyrics_header_button(&icon_radio_recent, 180, lyrics_offset_cb, &lyrics_offset_icon);
	lyrics_now = true;

	lyrics_line_t lines[30];
	for (int i = 0; i < 30; i++) {
		lines[i] = (lyrics_line_t){.text = "The words of a song that need to wrap", .ms = i * 1000};
	}
	lyrics_cur = (lyrics_t){.lines = lines, .count = 30, .synced = true};
	lyrics_loaded = true;
	check_header();
	lyrics_fill();
	check_offset();
	lyrics_lit = 9;
	lv_obj_t *line = lv_obj_get_child(lyrics_view, lyrics_lit);
	lyrics_paint_line(lyrics_lit, true);
	lyrics_centre(line, false);
	int normal_height = lv_obj_get_height(line);
	choose_size(2, 1);
	assert(lyrics_text_size == FONTS_TEXT_LARGE);
	assert(lv_obj_get_child(lyrics_view, lyrics_lit) == line);
	assert(lv_obj_get_style_text_font(line, 0) == fonts_lyrics(FONTS_TEXT_LARGE));
	assert(lv_obj_get_style_text_opa(line, 0) == LV_OPA_COVER);
	assert(lv_obj_get_height(line) > normal_height);
	assert(abs(label_y(line) + lv_obj_get_height(line) / 2 - (64 + 128)) <= 1);

	assert(config_init(path));
	lyrics_size_load();
	assert(lyrics_text_size == FONTS_TEXT_LARGE);
	assert(config_get_int("ui", "text_size", -1) == FONTS_TEXT_SMALL);

	// A new untimed track uses the saved size. Resizing does not jump back to
	// its start when the reader has scrolled down.
	lyrics_cur.synced = false;
	lyrics_header_sync(true);
	assert(lv_obj_is_hidden(lyrics_offset_btn));
	lyrics_fill();
	line = lv_obj_get_child(lyrics_view, 9);
	lv_obj_update_layout(lyrics_view);
	assert(lv_obj_get_style_text_font(line, 0) == fonts_lyrics(FONTS_TEXT_LARGE));
	lv_obj_scroll_to_y(lyrics_view, label_y(line) - 64, LV_ANIM_OFF);
	assert(label_y(line) == 64);
	choose_size(0, 2);
	assert(lv_obj_get_style_text_font(line, 0) == fonts_lyrics(FONTS_TEXT_SMALL));
	assert(label_y(line) == 64);
	assert(lv_obj_get_height(line) < normal_height);

	config_set_int("ui", "lyrics_text_size", -12);
	lyrics_size_load();
	assert(lyrics_text_size == FONTS_TEXT_NORMAL);
	assert(unlink(path) == 0);
	assert(rmdir(dir) == 0);
	puts("lyrics-size-ui: ok");
	return 0;
}

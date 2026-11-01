// Exercise the production Control Centre layout parser without the radios,
// streaming services and display behind the sheet. Section GC removes those
// unused handlers; the list migration and accessors are the real ones.
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/gui/shell/quickpanel.c"

// LVGL's configured default font; no widgets are created in this suite.
lv_font_t font_ui_14;

static void reset_layout(void) {
	order_loaded = false;
	hidden_n = 0;
	memset(slots, QP_BTN_NONE, sizeof(slots));
	memset(hidden, QP_BTN_NONE, sizeof(hidden));
}
static bool contains(const uint8_t *list, int n, quickpanel_button_t button) {
	for (int i = 0; i < n; i++) {
		if (list[i] == button) { return true; }
	}
	return false;
}
static void check_partition(void) {
	bool seen[QP_BTN_COUNT] = {false};
	int in_n = quickpanel_in_use_count();
	int out_n = quickpanel_hidden_count();
	assert(in_n <= QP_SLOT_COUNT && in_n + out_n == QP_BTN_COUNT);
	for (int i = 0; i < in_n; i++) {
		int b = quickpanel_in_use_at(i);
		assert(b >= 0 && b < QP_BTN_COUNT && !seen[b]);
		seen[b] = true;
	}
	for (int i = 0; i < out_n; i++) {
		int b = quickpanel_hidden_at(i);
		assert(b >= 0 && b < QP_BTN_COUNT && !seen[b]);
		seen[b] = true;
	}
}
int main(void) {
	lv_init();
	char dir[] = "/tmp/sonix-qp-layout-XXXXXX";
	assert(mkdtemp(dir));
	char path[512];
	snprintf(path, sizeof(path), "%s/config.ini", dir);
	config_init(path);

	// A fresh config still uses the old eight-place migration. Appending a
	// control must not push an existing default out of those eight places;
	// the new control waits in Settings until there is room for it.
	assert(quickpanel_in_use_count() == QP_SLOT_VISIBLE);
	assert(contains(hidden, hidden_n, QP_BTN_PEQ_PROFILE));
	assert(strcmp(button_key[QP_BTN_PEQ_PROFILE], "peq_profile") == 0);
	assert(strcmp(quickpanel_button_tag(QP_BTN_PEQ_PROFILE), "peq_profile") == 0);
	check_partition();

	// An existing v4 layout keeps every chosen position. The new button takes
	// an empty place; existing empty places remain empty.
	config_set("quickpanel", "order", "peq,eq,wifi,bluetooth,-,-,-,-");
	config_set("quickpanel", "order_more", "");
	config_set("quickpanel", "hidden", "mseb,airplay,gain,lineout,sonixlink,dlna,sleep_music,sleep_audiobook");
	config_set("quickpanel", "hidden_more", "sleep_podcast,wifi_transfer,gapless,fade");
	config_set_int("quickpanel", "layout", 4);
	reset_layout();
	assert(quickpanel_in_use_count() == 5);
	assert(quickpanel_in_use_at(0) == QP_BTN_PEQ);
	assert(quickpanel_in_use_at(3) == QP_BTN_BLUETOOTH);
	assert(quickpanel_in_use_at(4) == QP_BTN_PEQ_PROFILE);
	assert(slots[5] == QP_BTN_NONE);
	check_partition();

	// A full saved grid is not rearranged: the new control waits in the
	// settings list, where the user can explicitly make room for it.
	config_set("quickpanel", "order", "wifi,bluetooth,airplay,mseb,eq,gain,lineout,peq");
	config_set("quickpanel", "order_more", "sonixlink,dlna,sleep_music,sleep_audiobook,sleep_podcast,wifi_transfer,gapless,fade");
	config_set("quickpanel", "hidden", "");
	config_set("quickpanel", "hidden_more", "");
	reset_layout();
	assert(quickpanel_in_use_count() == QP_SLOT_COUNT);
	assert(quickpanel_in_use_at(15) == QP_BTN_FADE);
	assert(quickpanel_hidden_count() == 1);
	assert(quickpanel_hidden_at(0) == QP_BTN_PEQ_PROFILE);
	check_partition();

	// Moving it into the first eight makes it visible without expanding the
	// sheet. PEQ shifts into the extension, the last control waits in Settings,
	// and no chosen control is lost. The choice survives boot.
	quickpanel_move_in_use(QP_BTN_PEQ_PROFILE, 7);
	assert(quickpanel_in_use_at(7) == QP_BTN_PEQ_PROFILE);
	assert(quickpanel_in_use_at(8) == QP_BTN_PEQ);
	assert(contains(hidden, hidden_n, QP_BTN_FADE));
	check_partition();
	assert(config_save());
	config_init(path);
	reset_layout();
	assert(quickpanel_in_use_at(7) == QP_BTN_PEQ_PROFILE);
	assert(quickpanel_in_use_count() == QP_SLOT_COUNT);
	check_partition();
	printf("quickpanel: new PEQ profile control, saved layouts, full grid and visible-slot migration passed\n");
	return 0;
}

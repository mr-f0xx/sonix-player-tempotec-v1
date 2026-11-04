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

// Mode answers used by quickpanel_blocked(). DAC mode deliberately does not
// block the Control Centre any more; the other device-owning modes still do.
static bool dac_active;
static bool transfer_enabled;
static bool receiver_active;

bool usbdac_is_active(void) { return dac_active; }
bool wifitransfer_get_enabled(void) { return transfer_enabled; }
bool btreceiver_is_active(void) { return receiver_active; }

// The palette the page dots are coloured from. Nothing is drawn here.
const theme_palette_t *theme(void) {
	static theme_palette_t palette;
	return &palette;
}

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

static void test_quickpanel_mode_gating(void) {
	dac_active = true;
	transfer_enabled = false;
	receiver_active = false;
	assert(usbdac_is_active());
	assert(!quickpanel_blocked());

	dac_active = false;
	transfer_enabled = true;
	assert(quickpanel_blocked());

	transfer_enabled = false;
	receiver_active = true;
	assert(quickpanel_blocked());

	receiver_active = false;
	assert(!quickpanel_blocked());
}

int main(void) {
	lv_init();
	char dir[] = "/tmp/sonix-qp-layout-XXXXXX";
	assert(mkdtemp(dir));
	char path[512];
	snprintf(path, sizeof(path), "%s/config.ini", dir);
	config_init(path);
	test_quickpanel_mode_gating();

	// A config that has never carried a layout gets the panel as it ships: the
	// eight buttons the first page has always had, in their old order, and a
	// second page of the new toggles. Everything else waits in Settings.
	static const uint8_t page_one[QP_SLOT_VISIBLE] = {QP_BTN_WIFI, QP_BTN_BLUETOOTH, QP_BTN_AIRPLAY, QP_BTN_MSEB,
													  QP_BTN_EQ,   QP_BTN_GAIN,	  QP_BTN_LINEOUT, QP_BTN_SONIXLINK};
	static const uint8_t page_two[QP_SLOT_VISIBLE] = {QP_BTN_PEQ,		QP_BTN_AIRPLANE, QP_BTN_CROSSFEED, QP_BTN_REPLAYGAIN,
													  QP_BTN_MONO, QP_BTN_KEYLOCK,  QP_BTN_SCREEN_OFF, QP_BTN_POWER};
	assert(QP_PAGE_COUNT == 2 && QP_SLOT_COUNT == 2 * QP_SLOT_VISIBLE);
	assert(quickpanel_in_use_count() == QP_SLOT_COUNT);
	for (int i = 0; i < QP_SLOT_VISIBLE; i++) {
		assert(quickpanel_in_use_at(i) == page_one[i]);
		assert(quickpanel_in_use_at(QP_SLOT_VISIBLE + i) == page_two[i]);
	}
	assert(quickpanel_hidden_count() == QP_BTN_COUNT - QP_SLOT_COUNT);
	assert(contains(hidden, hidden_n, QP_BTN_PEQ_PROFILE));
	assert(contains(hidden, hidden_n, QP_BTN_SOUNDFIELD));
	assert(contains(hidden, hidden_n, QP_BTN_LED));
	assert(contains(hidden, hidden_n, QP_BTN_ROTATE));
	assert(strcmp(button_key[QP_BTN_PEQ_PROFILE], "peq_profile") == 0);
	assert(strcmp(quickpanel_button_tag(QP_BTN_PEQ_PROFILE), "peq_profile") == 0);
	check_partition();

	// Every button has a key and a tag, and the keys are all different: they
	// are what the config file stores.
	for (int i = 0; i < QP_BTN_COUNT; i++) {
		assert(button_key[i] && button_key[i][0] && button_tag[i] && button_tag[i][0]);
		assert(strlen(button_key[i]) <= 15);
		for (int j = 0; j < i; j++) {
			assert(strcmp(button_key[i], button_key[j]) != 0);
		}
	}
	assert(strcmp(button_key[QP_BTN_AIRPLANE], "airplane") == 0);
	assert(strcmp(button_key[QP_BTN_SCREEN_OFF], "screen_off") == 0);

	// The setting that is not a layout does not make one: a player whose owner
	// only moved the slider to the volume is still on the shipped panel.
	config_set_bool("quickpanel", "volume_slider", true);
	reset_layout();
	assert(quickpanel_in_use_at(QP_SLOT_VISIBLE + 1) == QP_BTN_AIRPLANE);
	check_partition();
	config_set_bool("quickpanel", "volume_slider", false);

	// A layout saved from the shipped one reads back the same.
	order_save();
	reset_layout();
	assert(quickpanel_in_use_count() == QP_SLOT_COUNT);
	assert(quickpanel_in_use_at(QP_SLOT_VISIBLE + 7) == QP_BTN_POWER);
	assert(quickpanel_hidden_count() == QP_BTN_COUNT - QP_SLOT_COUNT);
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
	assert(quickpanel_hidden_count() == QP_BTN_COUNT - QP_SLOT_COUNT);
	assert(contains(hidden, hidden_n, QP_BTN_PEQ_PROFILE));
	assert(contains(hidden, hidden_n, QP_BTN_AIRPLANE) && contains(hidden, hidden_n, QP_BTN_POWER));
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

	// Edit mode: a tap picks a tile up, a tap on another swaps the two (here
	// across the pages), a tap on the picked-up one or on nothing puts it down,
	// and a swap is saved. The first run of this section starts from the shipped
	// panel again.
	remove(path);
	config_init(path);
	reset_layout();
	assert(quickpanel_in_use_at(0) == QP_BTN_WIFI && quickpanel_in_use_at(15) == QP_BTN_POWER);
	edit_mode = true;
	edit_sel = QP_BTN_NONE;
	edit_tap(QP_BTN_WIFI);
	assert(edit_sel == QP_BTN_WIFI);
	edit_tap(QP_BTN_WIFI);
	assert(edit_sel == QP_BTN_NONE);
	edit_tap(QP_BTN_WIFI);
	edit_tap(QP_BTN_NONE);
	assert(edit_sel == QP_BTN_NONE && quickpanel_in_use_at(0) == QP_BTN_WIFI);
	edit_tap(QP_BTN_WIFI);
	edit_tap(QP_BTN_POWER);
	assert(edit_sel == QP_BTN_NONE);
	assert(quickpanel_in_use_at(0) == QP_BTN_POWER && quickpanel_in_use_at(15) == QP_BTN_WIFI);
	assert(quickpanel_in_use_count() == QP_SLOT_COUNT);
	check_partition();
	assert(config_save());
	config_init(path);
	reset_layout();
	assert(quickpanel_in_use_at(0) == QP_BTN_POWER && quickpanel_in_use_at(15) == QP_BTN_WIFI);
	check_partition();
	edit_mode = false;

	// More buttons left out than two keys can hold: they are carried by the
	// further hidden keys and none is lost over a save and a boot.
	config_set("quickpanel", "order", "wifi,-,-,-,-,-,-,-");
	config_set("quickpanel", "order_more", "-,-,-,-,-,-,-,-");
	config_set("quickpanel", "hidden", "");
	config_set("quickpanel", "hidden_more", "");
	config_set("quickpanel", "hidden_more2", "");
	config_set("quickpanel", "hidden_more3", "");
	config_set_int("quickpanel", "layout", 4);
	reset_layout();
	assert(quickpanel_in_use_count() == 1);
	assert(quickpanel_hidden_count() == QP_BTN_COUNT - 1);
	check_partition();
	order_save();
	assert(config_get("quickpanel", "hidden_more2", NULL) && config_get("quickpanel", "hidden_more2", NULL)[0]);
	assert(config_save());
	config_init(path);
	reset_layout();
	assert(quickpanel_in_use_count() == 1);
	assert(quickpanel_hidden_count() == QP_BTN_COUNT - 1);
	check_partition();

	// Every key written stays inside what a setting can hold.
	for (int k = 0; k < QP_HIDDEN_KEYS; k++) {
		const char *v = config_get("quickpanel", hidden_key_name[k], "");
		assert(strlen(v) <= 127);
	}

	printf("quickpanel: DAC-mode access, shipped two-page panel, saved layouts and hidden-list capacity passed\n");
	return 0;
}

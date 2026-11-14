#include "gestures.h"

#include "src/system/core/config.h"

// Read once and kept, like coverflow_enabled(): the answer is asked for on
// every press that lands on the status bar or the bottom edge of the screen, and
// going back to the config file for it -- a set of strings compared by name --
// is work a touch handler has no business doing.
static bool loaded;
static bool stock;

static bool swipe_up_home_loaded;
static bool swipe_up_home;

bool gestures_stock(void) {
	if (!loaded) {
		loaded = true;
		stock = config_get_bool("ui", "stock_gestures", false);
	}
	return stock;
}

void gestures_set_stock(bool on) {
	loaded = true;
	stock = on;
	config_set_bool("ui", "stock_gestures", on);
	config_save();
}

bool gestures_swipe_up_home(void) {
	if (!swipe_up_home_loaded) {
		swipe_up_home_loaded = true;
		swipe_up_home = config_get_bool("ui", "swipe_up_home", true);
	}
	return swipe_up_home;
}

void gestures_set_swipe_up_home(bool on) {
	swipe_up_home_loaded = true;
	swipe_up_home = on;
	config_set_bool("ui", "swipe_up_home", on);
	config_save();
}

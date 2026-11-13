#include "addons.h"

#include <stddef.h>
#include <string.h>

#include "src/system/core/config.h"

// The list, and nothing else in this file knows what any of them do: the gate
// is asked by the page that would build the module, and by the menu that would
// offer it. Adding a module is a row here, a switch that never leaves the
// player, and the two questions -- "may I build?" -- at the places that build.
//
// Order is the Plugins page's: the services first, because they are the ones
// people actually turn off, then what belongs beside the music, then the tools
// and the games. The section tag is on every row so the page never has to know
// which id is what.
static const addon_t ADDONS[] = {
	{"tidal", "tidal", "addon_tidal_note", "plugins_section_services"},
	{"qobuz", "qobuz", "addon_qobuz_note", "plugins_section_services"},
	{"podcasts", "podcasts", "addon_podcasts_note", "plugins_section_services"},
	{"radio", "radio", "addon_radio_note", "plugins_section_services"},
	{"subsonic", "subsonic", "addon_subsonic_note", "plugins_section_services"},
	{"soundcloud", "soundcloud", "addon_soundcloud_note", "plugins_section_services"},
	{"webdav", "webdav", "addon_webdav_note", "plugins_section_services"},
	// What talks to a pair of earbuds over the Bluetooth link: the AirPods'
	// battery and noise control through Apple's own protocol. Off means the
	// worker that speaks it is never started and its page is not built.
	{"airpods", "airpods", "addon_airpods_note", "plugins_section_earbuds"},
	// A book is not a music feature, and nobody would look for it under one.
	{"ebook", "books", "addon_ebook_note", "plugins_section_library"},
	{"coverflow", "cover_flow", "addon_coverflow_note", "plugins_section_player"},
	// What the player does WITH the music: words on screen, the sound shaped,
	// a line to two scrobblers.
	{"lyrics", "addon_lyrics_label", "addon_lyrics_note", "plugins_section_features"},
	// The three ways this player shapes the sound, each a module of its own.
	// Switched off, their pages are never built, their rows leave Music
	// settings and the filters leave the chain.
	{"eq", "equaliser", "addon_eq_note", "plugins_section_features"},
	{"peq", "peq", "addon_peq_note", "plugins_section_features"},
	{"mseb", "mseb", "addon_mseb_note", "plugins_section_features"},
	{"autoeq", "addon_autoeq_label", "addon_autoeq_note", "plugins_section_features"},
	{"mono", "musicsettings_mono_audio", "addon_mono_note", "plugins_section_features"},
	{"soundfield", "musicsettings_soundfield", "addon_soundfield_note", "plugins_section_features"},
	{"crossfeed", "musicsettings_crossfeed", "addon_crossfeed_note", "plugins_section_features"},
	{"channel_balance", "musicsettings_channel_balance", "addon_channel_balance_note", "plugins_section_features"},
	{"volume_limit", "musicsettings_volume_limit", "addon_volume_limit_note", "plugins_section_features"},
	{"lastfm", "lastfm", "addon_lastfm_note", "plugins_section_features"},
	{"listenbrainz", "listenbrainz", "addon_listenbrainz_note", "plugins_section_features"},
	{"calculator", "calculator", "addon_calculator_note", "plugins_section_tools"},
	{"notes", "notes", "addon_notes_note", "plugins_section_tools"},
	{"tagmanager", "addon_tagmanager_label", "addon_tagmanager_note", "plugins_section_tools"},
	{"metronome", "metronome", "addon_metronome_note", "plugins_section_tools"},
#if GB_CORE
	{"gearboy", "gearboy", "addon_gearboy_note", "plugins_section_games"},
#endif
	{"flappy", "flappy_bird", "addon_flappy_note", "plugins_section_games"},
	{"game2048", "2048", "addon_2048_note", "plugins_section_games"},
	{"snake", "snake", "addon_snake_note", "plugins_section_games"},
	{"dino", "dino", "addon_dino_note", "plugins_section_games"},
	{"pet", "pet", "addon_pet_note", "plugins_section_games"},
};

int addons_count(void) { return (int)(sizeof(ADDONS) / sizeof(ADDONS[0])); }

const addon_t *addons_at(int index) {
	if (index < 0 || index >= addons_count()) {
		return NULL;
	}
	return &ADDONS[index];
}

const addon_t *addons_find(const char *id) {
	if (!id) {
		return NULL;
	}
	for (int i = 0; i < addons_count(); i++) {
		if (strcmp(ADDONS[i].id, id) == 0) {
			return &ADDONS[i];
		}
	}
	return NULL;
}

// Fresh install defaults: a lean set. Everything not named here starts off.
// The DSP modules that the player has always had on stay on, so the sound a
// new owner hears out of the box is the sound this firmware has always made.
static bool addon_is_default_on(const char *id) {
	if (!id) {
		return false;
	}
	if (strcmp(id, "subsonic") == 0) return true;
	if (strcmp(id, "soundcloud") == 0) return true;
	if (strcmp(id, "webdav") == 0) return true;
	if (strcmp(id, "eq") == 0) return true;
	if (strcmp(id, "peq") == 0) return true;
	if (strcmp(id, "mseb") == 0) return true;
	if (strcmp(id, "mono") == 0) return true;
	if (strcmp(id, "soundfield") == 0) return true;
	if (strcmp(id, "crossfeed") == 0) return true;
	if (strcmp(id, "channel_balance") == 0) return true;
	if (strcmp(id, "volume_limit") == 0) return true;
	return false;
}

bool addons_enabled(const char *id) {
	if (!id) {
		return true;
	}
	return config_get_bool("addons", id, addon_is_default_on(id));
}

bool addons_set(const char *id, bool on) {
	if (!id || !addons_find(id)) {
		return false;
	}
	bool previous = addons_enabled(id);
	config_set_bool("addons", id, on);
	if (addons_enabled(id) != on || !config_save()) {
		config_set_bool("addons", id, previous);
		config_save();
		return false;
	}
	return true;
}

void addons_seed(bool fresh_install) {
	if (config_get_bool("addons", "seeded", false)) {
		return;
	}
	for (int i = 0; i < addons_count(); i++) {
		bool on = fresh_install ? addon_is_default_on(ADDONS[i].id) : true;
		config_set_bool("addons", ADDONS[i].id, on);
	}
	config_set_bool("addons", "seeded", true);
	// The first-run hint explains the lean defaults, so it is for a fresh
	// install only. An existing device has nothing new to be told.
	config_set_bool("system", "plugins_notice_shown", !fresh_install);
	config_save();
}

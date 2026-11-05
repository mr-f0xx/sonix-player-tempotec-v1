#include "region.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "src/system/core/config.h"
#include "src/system/core/lang.h"

// The standard list. Worldwide first because it is the answer before anyone
// has chosen: no preference, the catalogue as it is indexed, "any country" for
// the radio directory. After it, one entry per language the firmware ships, in
// the order the language files are kept, each in its own locale -- the region
// picker therefore offers exactly what the player itself speaks, and nothing
// whose name nobody here could read.
//
// The labels of those are the languages' own names rather than translation
// tags: "Polski" is what the row says in every interface language, as the
// podcast chart's list already had it. Only "Worldwide" is a tag, because it
// is a word of the interface rather than the name of a language.
static const region_t REGIONS[] = {
	{"worldwide", "", "", "region_worldwide", true},
	{"gb", "GB", "en", "English", false},
	{"it", "IT", "it", "Italiano", false},
	{"de", "DE", "de", "Deutsch", false},
	{"es", "ES", "es", "Espa\u00f1ol", false},
	{"fr", "FR", "fr", "Fran\u00e7ais", false},
	{"nl", "NL", "nl", "Nederlands", false},
	{"pl", "PL", "pl", "Polski", false},
	{"br", "BR", "pt", "Portugu\u00eas", false},
	{"ua", "UA", "uk", "\u0423\u043a\u0440\u0430\u0457\u043d\u0441\u044c\u043a\u0430", false},
	{"ru", "RU", "ru", "\u0420\u0443\u0441\u0441\u043a\u0438\u0439", false},
	{"jp", "JP", "ja", "\u65e5\u672c\u8a9e", false},
};

#define REGION_COUNT ((int)(sizeof(REGIONS) / sizeof(REGIONS[0])))

static int index_of_id(const char *id) {
	if (!id || !id[0]) {
		return 0;
	}
	for (int i = 0; i < REGION_COUNT; i++) {
		if (strcmp(REGIONS[i].id, id) == 0) {
			return i;
		}
	}
	return -1;
}

// The choice in force. An id no build ever wrote answers Worldwide rather than
// nothing: a hand-edited config must not leave every list unfiltered.
static const char *stored_id(void) {
	const char *value = config_get("region", "value", "");
	return value && value[0] ? value : REGION_DEFAULT_ID;
}

// Older builds kept this in three places -- a market pair for YouTube Music, a
// country for the radio, a language for the podcast chart -- and each service
// wrote its own. Read once, the first of the three that still names an entry
// seeds the shared value, so an update does not send anybody back to
// Worldwide, and after that only this file writes it.
static bool seeded;

static void seed_from_old_keys(void) {
	if (seeded) {
		return;
	}
	seeded = true;
	if (config_get("region", "value", "")[0]) {
		return;
	}
	const char *pairs[2][2];
	pairs[0][0] = config_get("ytmusic", "region", "");
	pairs[0][1] = config_get("ytmusic", "language", "");
	pairs[1][0] = config_get("radio", "region", "");
	pairs[1][1] = "";

	for (int p = 0; p < 2; p++) {
		const char *cc = pairs[p][0];
		const char *lang = pairs[p][1];
		if (!cc || !cc[0]) {
			continue;
		}
		// The exact pair first, then the country alone: an old market pair
		// whose language has no entry of its own still names its country.
		for (int pass = 0; pass < 2; pass++) {
			for (int i = 1; i < REGION_COUNT; i++) {
				bool country_ok = strcmp(REGIONS[i].country, cc) == 0;
				bool language_ok = !lang || !lang[0] || strcmp(REGIONS[i].language, lang) == 0;
				if (country_ok && (pass == 0 ? language_ok : true)) {
					config_set("region", "value", REGIONS[i].id);
					config_save();
					return;
				}
			}
		}
	}

	const char *podcast_lang = config_get("podcast", "trending_lang", "");
	if (podcast_lang && podcast_lang[0]) {
		for (int i = 1; i < REGION_COUNT; i++) {
			if (strcmp(REGIONS[i].language, podcast_lang) == 0) {
				config_set("region", "value", REGIONS[i].id);
				config_save();
				return;
			}
		}
	}

	// The radio kept the directory's own name for the country -- "Poland",
	// not "pl" -- so those are mapped too, for the eleven this list offers.
	// Any other name the old directory offered answers Worldwide, which is
	// where a country this list does not carry belongs anyway.
	static const struct {
		const char *name;
		const char *id;
	} RADIO_NAMES[] = {
		{"Poland", "pl"},	  {"United Kingdom", "gb"}, {"Italy", "it"},
		{"Germany", "de"},	  {"Spain", "es"},	  {"France", "fr"},
		{"Netherlands", "nl"}, {"Brazil", "br"},	  {"Ukraine", "ua"},
		{"Russia", "ru"},	  {"Japan", "jp"},
	};
	const char *radio_name = config_get("radio", "region", "");
	if (radio_name && radio_name[0]) {
		for (size_t n = 0; n < sizeof(RADIO_NAMES) / sizeof(RADIO_NAMES[0]); n++) {
			if (strcasecmp(RADIO_NAMES[n].name, radio_name) == 0) {
				config_set("region", "value", RADIO_NAMES[n].id);
				config_save();
				return;
			}
		}
		// Or a code set by hand: the same thing region_select_id would have
		// done, asked of the country rather than of the id.
		for (int i = 1; i < REGION_COUNT; i++) {
			if (strcasecmp(REGIONS[i].country, radio_name) == 0) {
				config_set("region", "value", REGIONS[i].id);
				config_save();
				return;
			}
		}
	}
}

int region_list(region_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	int count = REGION_COUNT < max ? REGION_COUNT : max;
	for (int i = 0; i < count; i++) {
		out[i] = REGIONS[i];
	}
	return count;
}

const region_t *region_current(void) {
	seed_from_old_keys();
	int index = index_of_id(stored_id());
	return index < 0 ? &REGIONS[0] : &REGIONS[index];
}

int region_index(void) { return (int)(region_current() - REGIONS); }

bool region_select_id(const char *id) {
	seed_from_old_keys();
	if (!id || index_of_id(id) < 0) {
		return false;
	}
	config_set("region", "value", id);
	config_save();
	return true;
}

bool region_select(int index) {
	if (index < 0 || index >= REGION_COUNT) {
		return false;
	}
	return region_select_id(REGIONS[index].id);
}

bool region_select_language(const char *language) {
	if (language && language[0]) {
		for (int i = 1; i < REGION_COUNT; i++) {
			if (strcmp(REGIONS[i].language, language) == 0) {
				return region_select(i);
			}
		}
	}
	// No language, or one this list does not speak: Worldwide, which is what
	// an empty choice has always meant.
	return region_select(0);
}

bool region_select_country(const char *country) {
	if (country && country[0]) {
		for (int i = 1; i < REGION_COUNT; i++) {
			if (strcasecmp(REGIONS[i].country, country) == 0) {
				return region_select(i);
			}
		}
	}
	// No country, or one this list does not carry: Worldwide, which is the
	// radio's own "any country" and what an empty choice has always meant.
	return region_select(0);
}

void region_entry_label(const region_t *region, char *out, size_t size) {
	if (!out || !size) {
		return;
	}
	if (!region) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%s", region->translated ? tr(region->label) : region->label);
}

void region_label(char *out, size_t size) { region_entry_label(region_current(), out, size); }

void region_market(char *country, size_t country_size, char *language, size_t language_size) {
	const region_t *region = region_current();
	if (country && country_size) {
		snprintf(country, country_size, "%s", region->country[0] ? region->country : "US");
	}
	if (language && language_size) {
		snprintf(language, language_size, "%s", region->language[0] ? region->language : "en");
	}
}

const char *region_language(void) { return region_current()->language; }

const char *region_country(void) { return region_current()->country; }

#ifndef REGION_H
#define REGION_H

#include <stdbool.h>
#include <stddef.h>

// One region choice, shared by every service that has one.
//
// Tidal, Podcasts, Radio and YouTube Music each had their own table, their own
// page and their own config key, so the same player offered a country list
// here, a language list there and a fetched directory of two hundred countries
// somewhere else -- and "where am I listening from" had four answers instead of
// one. This is the single list they all draw now, the single value they all
// read, and region_select() is the single way it changes.
//
// The options follow what the player itself speaks: Worldwide first, then one
// entry per language built into the firmware, each in its own locale. Nothing
// here is fetched, so the picker is the same on every device and works with no
// network at all.
typedef struct {
	const char *id;	   // what the config stores: "worldwide", "pl", "fr" ...
	const char *country; // ISO 3166-1 alpha-2, "" for worldwide
	const char *language; // ISO 639-1, "" for worldwide
	const char *label;   // a tag, or a language's own name (never translated)
	bool translated;	   // true when `label` is a tag for tr()
} region_t;

// The choice a player that has never been asked answers with.
#define REGION_DEFAULT_ID "worldwide"

// The list is short and fixed. A ceiling for a caller that needs somewhere on
// the stack to put it -- the entries themselves are never copied out.
#define REGION_LIST_MAX 16

// The standard list: Worldwide first, then every language the firmware ships,
// in the order the language files are kept. `out` receives `max` entries at
// most; the count is what is returned.
int region_list(region_t *out, int max);

// The entry in force. Never NULL: an unknown stored id answers Worldwide.
const region_t *region_current(void);

// The index in region_list() of the choice in force.
int region_index(void);

// Selects by index or by id, saves the choice and answers whether it took.
// The one function every service's picker calls.
bool region_select(int index);
bool region_select_id(const char *id);

// The entry whose language code this is -- "pl", "fr" -- or Worldwide when
// there is none and when the code is empty: how a stored code is turned back
// into the choice, which is all the podcast chart needs of this.
bool region_select_language(const char *language);

// The entry with this ISO 3166 country, or Worldwide when there is none and
// when it is empty: how the radio's stored country becomes the choice.
bool region_select_country(const char *country);

// The row's text for the choice in force: tr()'d for a tag, the name itself
// otherwise. Never NULL.
void region_label(char *out, size_t size);

// The row's text for one entry of the list, the same way.
void region_entry_label(const region_t *region, char *out, size_t size);

// For a service keyed on the market rather than on the pair: the country and
// the language a request should carry. Worldwide answers the defaults the
// endpoints take with no preference (US / en), never empty buffers.
void region_market(char *country, size_t country_size, char *language, size_t language_size);

// ISO 639-1 for the choice in force, "" for worldwide (the catalogue's own
// default rather than somebody's language).
const char *region_language(void);

// ISO 3166-1 for the choice in force, "" for worldwide ("any country").
const char *region_country(void);

#endif /* REGION_H */

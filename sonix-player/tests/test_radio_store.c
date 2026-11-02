// Regression coverage for radio favourites and Recently Played when a station
// has no radio-browser UUID (custom and built-in stations only have a URL).
// The real radio store is included so the private recent_note() path can be
// tested too; SQLite and the filesystem are real, while file identity helpers
// are tiny host implementations.

#include "../src/system/streaming/radio.c"

#include <dirent.h>
#include <sys/stat.h>

static int failures;

#define CHECK(cond)                                                                            \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			failures++;                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
		}                                                                                       \
	} while (0)

void file_identity_read(const char *path, file_identity_t *out) {
	struct stat st;
	out->exists = path && path[0] && stat(path, &st) == 0;
	out->dev = out->exists ? st.st_dev : 0;
	out->ino = out->exists ? st.st_ino : 0;
}

bool file_identity_changed(const char *path, const file_identity_t *was) {
	file_identity_t now;
	file_identity_read(path, &now);
	return now.exists != was->exists || now.dev != was->dev || now.ino != was->ino;
}

static void make_station(radio_station_t *station, const char *name, const char *url) {
	memset(station, 0, sizeof(*station));
	snprintf(station->name, sizeof(station->name), "%s", name);
	snprintf(station->url, sizeof(station->url), "%s", url);
}

static void set_recent_played(const char *url, sqlite3_int64 played) {
	char key[RADIO_URL_MAX + sizeof(RADIO_STORE_URL_KEY_PREFIX)];
	snprintf(key, sizeof(key), "%s%s", RADIO_STORE_URL_KEY_PREFIX, url);

	sqlite3_stmt *stmt = NULL;
	CHECK(sqlite3_prepare_v2(store, "UPDATE RECENT SET played=? WHERE uuid=?", -1, &stmt, NULL) == SQLITE_OK);
	if (!stmt) {
		return;
	}
	sqlite3_bind_int64(stmt, 1, played);
	sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
	CHECK(sqlite3_step(stmt) == SQLITE_DONE);
	sqlite3_finalize(stmt);
}

static void check_url_favourite(const char *root) {
	radio_station_t station;
	make_station(&station, "Curated Stream", "https://radio.example/stream.mp3");

	CHECK(!radio_fav_contains_station(&station));
	radio_fav_add(&station);
	CHECK(radio_fav_count() == 1);
	CHECK(radio_fav_contains_station(&station));

	// The synthetic database key must not leak into the public station UUID.
	radio_station_t loaded;
	CHECK(radio_fav_get(0, &loaded));
	CHECK(loaded.uuid[0] == '\0');
	CHECK(strcmp(loaded.url, station.url) == 0);
	CHECK(strcmp(loaded.name, station.name) == 0);

	// Re-adding the same URL updates the row instead of creating a duplicate.
	snprintf(station.name, sizeof(station.name), "%s", "Renamed Stream");
	radio_fav_add(&station);
	CHECK(radio_fav_count() == 1);
	CHECK(radio_fav_get(0, &loaded));
	CHECK(strcmp(loaded.name, "Renamed Stream") == 0);

	// URL-backed favourites survive closing and reopening the database.
	radio_store_close();
	CHECK(radio_store_open(root));
	CHECK(radio_fav_contains_station(&station));
	CHECK(radio_fav_count() == 1);

	radio_fav_remove_station(&station);
	CHECK(radio_fav_count() == 0);
}

static void check_uuid_favourite(void) {
	radio_station_t station;
	make_station(&station, "Directory Stream", "https://radio.example/directory.mp3");
	snprintf(station.uuid, sizeof(station.uuid), "%s", "123e4567-e89b-12d3-a456-426614174000");

	radio_fav_add(&station);
	CHECK(radio_fav_contains_station(&station));
	CHECK(radio_fav_contains(station.uuid));
	CHECK(radio_fav_get(0, &station));
	CHECK(strcmp(station.uuid, "123e4567-e89b-12d3-a456-426614174000") == 0);
	radio_fav_remove_station(&station);
	CHECK(radio_fav_count() == 0);
}

static void check_url_recents_and_limit(void) {
	CHECK(store_exec("DELETE FROM RECENT"));

	// A repeated URL moves to one updated row, even with no UUID to key on.
	radio_station_t station;
	make_station(&station, "First Name", "https://radio.example/recent.mp3");
	recent_note(&station);
	snprintf(station.name, sizeof(station.name), "%s", "Updated Name");
	recent_note(&station);
	CHECK(radio_recent_count() == 1);
	radio_station_t loaded;
	CHECK(radio_recent_get(0, &loaded));
	CHECK(loaded.uuid[0] == '\0');
	CHECK(strcmp(loaded.url, station.url) == 0);
	CHECK(strcmp(loaded.name, "Updated Name") == 0);

	CHECK(store_exec("DELETE FROM RECENT"));
	for (int i = 0; i < RADIO_RECENT_MAX + 2; i++) {
		char name[32];
		char url[RADIO_URL_MAX];
		snprintf(name, sizeof(name), "Station %d", i);
		snprintf(url, sizeof(url), "https://radio.example/%d.mp3", i);
		make_station(&station, name, url);
		recent_note(&station);
		// Give each test row a deterministic order; recent_note() itself uses
		// wall-clock seconds, which can be identical across a fast test run.
		set_recent_played(url, i + 1);
	}

	CHECK(radio_recent_count() == RADIO_RECENT_MAX);
	CHECK(radio_recent_get(0, &loaded));
	CHECK(strcmp(loaded.url, "https://radio.example/11.mp3") == 0);
	CHECK(strcmp(loaded.name, "Station 11") == 0);
	CHECK(radio_recent_get(RADIO_RECENT_MAX - 1, &loaded));
	CHECK(strcmp(loaded.url, "https://radio.example/2.mp3") == 0);
	CHECK(!radio_recent_get(RADIO_RECENT_MAX, &loaded));
}

static void check_reopen_after_database_replacement(const char *root) {
	char local[512];
	char database[512];
	char backup[512];
	snprintf(local, sizeof(local), "%s/.local", root);
	snprintf(database, sizeof(database), "%s/.local/radio.db", root);
	snprintf(backup, sizeof(backup), "%s/.local/radio.db.old", root);

	// Keep the old inode alive while making a replacement at the same path, so
	// the store's identity check cannot mistake inode reuse for the same file.
	CHECK(rename(database, backup) == 0);
	FILE *replacement = fopen(database, "wb");
	CHECK(replacement != NULL);
	if (replacement) {
		CHECK(fclose(replacement) == 0);
	}

	radio_store_reopen_if_replaced();
	CHECK(radio_fav_count() == 0);
	CHECK(radio_recent_count() == 0);

	radio_store_close();
	DIR *dir = opendir(local);
	if (dir) {
		struct dirent *entry;
		while ((entry = readdir(dir)) != NULL) {
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
				continue;
			}
			char path[768];
			snprintf(path, sizeof(path), "%s/%s", local, entry->d_name);
			unlink(path);
		}
		closedir(dir);
	}
	rmdir(local);
	rmdir(root);
}

int main(void) {
	char root_template[] = "/tmp/radio-store-test-XXXXXX";
	char *root = mkdtemp(root_template);
	CHECK(root != NULL);
	if (!root) {
		return 1;
	}

	// Open on a clean card: the store creates .local itself.
	CHECK(radio_store_open(root));
	char local[512];
	snprintf(local, sizeof(local), "%s/.local", root);
	struct stat st;
	CHECK(stat(local, &st) == 0 && S_ISDIR(st.st_mode));

	check_url_favourite(root);
	check_uuid_favourite();
	check_url_recents_and_limit();
	radio_store_close();
	CHECK(radio_store_open(root));
	CHECK(radio_recent_count() == RADIO_RECENT_MAX);
	radio_station_t loaded;
	CHECK(radio_recent_get(0, &loaded));
	CHECK(strcmp(loaded.url, "https://radio.example/11.mp3") == 0);
	check_reopen_after_database_replacement(root);

	if (failures) {
		fprintf(stderr, "%d radio store check(s) failed\n", failures);
		return 1;
	}
	puts("ok  radio favourites and recents support URL-only stations, persist and reopen cleanly");
	return 0;
}

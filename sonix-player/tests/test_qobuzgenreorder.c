#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/gui/streaming/qobuzgenreorder.h"

static qobuz_genre_t source[80];
static int source_count, fail_root, reggae_status;
static qobuz_genre_t reggae = {.id = 777, .name = "Reggae", .slug = "reggae"};
int qobuz_genres(int offset, qobuz_genre_t *out, int max) {
    if (fail_root) return -1;
    int n = source_count - offset;
    if (n < 0) n = 0;
    if (n > max) n = max;
    memcpy(out, source + offset, (size_t)n * sizeof(*out));
    return n;
}
int qobuz_reggae_genre(qobuz_genre_t *out) {
    if (reggae_status == 1) *out = reggae;
    return reggae_status;
}
int main(void) {
    // Put every favourite on the second API page, with localized names.
    source_count = 56;
    for (int i = 0; i < source_count; i++) {
        source[i].id = 1000 + i;
        snprintf(source[i].name, sizeof(source[i].name), "Unknown %d", i);
    }
    source[50].id = 3; source[51].id = 64; source[52].id = 80;
    source[53].id = 127; source[54].id = 133; source[55].id = 112;
    strcpy(source[54].name, "Rap localise");
    reggae_status = 1;
    qobuz_genre_t out[100]; long reggae_id = 0;
    int n = qobuzgenre_load_ordered(out, 100, &reggae_id);
    const long expected[] = {133, 127, 80, 64, 3, 777, 112, 1000, 1001};
    assert(n == 57 && reggae_id == 777);
    for (unsigned i = 0; i < sizeof(expected)/sizeof(expected[0]); i++) assert(out[i].id == expected[i]);
    assert(!strcmp(out[0].name, "Rap localise"));
    assert(out[49].id != 133); // preferred entries were promoted across the page boundary

    // No duplicate if the root list already contained the resolved child.
    source[0] = reggae;
    n = qobuzgenre_load_ordered(out, 100, &reggae_id);
    assert(n == 56 && out[5].id == 777);
    int matches = 0;
    for (int i = 0; i < n; i++) if (out[i].id == 777) matches++;
    assert(matches == 1);
    reggae_status = -1;
    n = qobuzgenre_load_ordered(out, 100, &reggae_id);
    assert(n == 56 && reggae_id == 777 && out[5].id == 777); // root fallback
    source[0].id = 1000;
    strcpy(source[0].name, "Unknown 0");
    n = qobuzgenre_load_ordered(out, 100, &reggae_id);
    assert(n == 56 && reggae_id == 0); // keep browsing if the child lookup fails
    // Children is filtered by its stable slug even when the name is localized.
    // Blues is promoted even when Qobuz lists it only beneath a parent.
    source[50].id = 3000;
    source[1].id = 3001;
    strcpy(source[1].name, "Pour les petits");
    strcpy(source[1].slug, "diction-litterature/enfants");
    reggae_status = 1;
    n = qobuzgenre_load_ordered(out, 100, &reggae_id);
    assert(n == 57 && out[4].id == 3 && out[5].id == 777);
    for (int i = 0; i < n; i++) assert(out[i].id != 3001);
    // Localized root categories are removed without hiding their individual
    // children, or changing the ordering of the remaining genres.
    source_count = 6;
    memset(source, 0, sizeof(source));
    source[0] = (qobuz_genre_t){.id = 91, .name = "Films", .slug = "localized"};
    source[1] = (qobuz_genre_t){.id = 901, .name = "French songs", .slug = "chanson-francaise"};
    source[2] = (qobuz_genre_t){.id = 902, .name = "Blues/Country/Folk"};
    source[3] = (qobuz_genre_t){.id = 4, .name = "Country"};
    source[4] = (qobuz_genre_t){.id = 5, .name = "Folk"};
    source[5] = (qobuz_genre_t){.id = 112, .name = "Pop/Rock"};
    reggae_status = 0;
    n = qobuzgenre_load_ordered(out, 100, &reggae_id);
    assert(n == 4 && out[0].id == 3 && out[1].id == 112 && out[2].id == 5 && out[3].id == 4);
    fail_root = 1;
    assert(qobuzgenre_load_ordered(out, 100, &reggae_id) == -1);
    puts("PASS Qobuz genre ordering, cross-page promotion and fallback");
    return 0;
}

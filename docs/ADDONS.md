# Plugins (optional modules)

The player has a list of optional modules. Each one has a switch on
**Settings → Plugins**. A module that is switched off is not built at start-up:
its pages are never created, its rows leave the menus, and the memory those
pages would have held stays free for the music.

The change takes effect after a restart. The Plugins page says so, because a
page that has already been built cannot give its memory back until the next
start.

## The list

The source of truth is `sonix-player/src/system/addons/addons.c`. Each entry has
four fields: the id (also the config key), the row label, the one-line note and
the section heading on the Plugins page.

| Section | Module (id) | What switching it off removes |
| --- | --- | --- |
| Streaming services | `tidal`, `qobuz`, `podcasts`, `radio`, `subsonic` | The service's pages and its Streaming tile or list row. |
| Earbuds | `airpods` | The AirPods worker and its page. |
| Library | `ebook` | The Books shelf and the EPUB reader (home Books tile). |
| Player | `coverflow` | The album carousel and its larger cover cache. |
| Music features | `lyrics` | Synced lyrics on the player and the LRCLIB search. |
| Music features | `eq`, `peq`, `mseb`, `autoeq` | The equaliser, parametric EQ, MSEB tuning and AutoEq pages and their quick-panel buttons. |
| Music features | `mono`, `soundfield`, `crossfeed`, `channel_balance`, `volume_limit` | The filter in the audio chain and its row in Music settings. |
| Music features | `lastfm`, `listenbrainz` | Scrobbling to that service. |
| Tools | `calculator`, `notes`, `tagmanager`, `metronome` | The tile on the More page. |
| Games | `gearboy`, `flappy`, `game2048`, `snake`, `dino`, `pet` | The tile on the More page. `gearboy` is only listed in builds with the Game Boy core. |

Not every feature is a module. Cover Flow's own layout switch, Audiobooks, the
sleep timer and artwork are not gated. Modules that the fork does not ship are
not listed.

## Fresh installs and existing devices

Seeding happens once, at start-up, in `addons_seed()`:

- **Fresh install** (no language chosen yet): the lean set applies. The DSP
  modules the player has always run (`eq`, `peq`, `mseb`, `mono`, `soundfield`,
  `crossfeed`, `channel_balance`, `volume_limit`) and `subsonic` start on.
  Everything else starts off. The first-run hint is shown once.
- **Existing device** (updated firmware): every module stays on, so nothing
  disappears after an update. No hint is shown.

After seeding, the stored choice is only changed from the Plugins page.

## The first-run hint

On a fresh install, a notice card titled **Plugins** appears about a second
after the main menu opens. It explains the lean defaults and where to switch
modules on. The flag `[system] plugins_notice_shown` is written before the card
is shown, so it never returns.

## Configuration

Switches live in the `[addons]` section of `device_config.ini`:

```ini
[addons]
seeded = true
tidal = false
calculator = true
```

A missing key means off, except for the defaults above. An unknown id is always
treated as on, so a misspelt name cannot quietly remove a feature.

## Adding a module

1. Add a row to `ADDONS[]` in `addons.c`. Pick the section and give it a note key.
2. Add the default in `addon_is_default_on()` only if it should start on for a
   fresh install.
3. Add the note and label keys to every `sonix-packer/assets/V1/usr/resource/sonix/language/*.ini`
   file. The Plugins page will show the raw key until they exist.
4. At each place that builds the module, ask `addons_enabled("<id>")` first.
   The page that hosts a row or tile does the check, so the module's own code
   never needs to know about the list.
5. Test with the main menu and Plugins test binaries (see below).

## Files

- `sonix-player/src/system/addons/addons.h`, `addons.c`: the list, the switches and seeding.
- `sonix-player/src/gui/settings/plugins.c`, `plugins.h`: the Settings → Plugins page.
- `sonix-player/src/gui/shell/gui.c`: seeding at the start of `gui_init`, and the first-run hint.
- `sonix-player/tests/ui/test_main_menu.c`, `test_tools_pages.c`: seed as an existing device before checking gated pages.

## Limits

- Switching a module off does not free memory already in use until the restart.
- If every streaming service is off, the Streaming tile still opens an empty page.
  The main menu keeps its six tiles, so the tile is not hidden.
- Layouts (the `layout_*` switches of upstream) are not part of this system.

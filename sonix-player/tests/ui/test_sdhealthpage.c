// The production SD-card health page on the V1's 240x320 LVGL panel. The
// benchmark worker is made synchronous and its result is injected, so the
// test can exercise running, completion, every verdict, cancellation, USB and
// card guards, the stall watchdog, all eleven languages and larger fonts.
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/sdhealthpage.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/sdbench.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"

// ---------------------------------------------------------------------------
// stand-ins for the shell
// ---------------------------------------------------------------------------

lv_font_t font_ui_14, font_ui_16, font_ui_20;
lv_style_t theme_style_card, theme_style_text, theme_style_text_dim;
static theme_palette_t palette;
static theme_refresh_cb_t refresh_cb;

// Injected card state and counters, shared by the shell stubs and the wrapped
// worker. Declared before the stubs that read them.
static int scenario;
static bool skip_worker, fail_thread;
static bool card_available = true, usb_active;
static unsigned run_calls, worker_calls, background_calls, no_card_calls, progress_calls;
static char last_popup[64];
// A skipped worker stands in for a read that has not returned yet. Keep its
// entry point so the test can inject cancel/USB/card changes while it is
// running, then let it return and prove state and generation handling.
static void *(*pending_worker)(void *);
static void *pending_arg;
static bool allow_cancelled_worker;
static bool last_run_cancelled;

const theme_palette_t *theme(void) { return &palette; }
void theme_register_refresh(theme_refresh_cb_t cb) { refresh_cb = cb; }

static lv_obj_t *page_title;
lv_obj_t *settingsrow_page_title(lv_obj_t *screen) {
	(void)screen;
	return page_title;
}
void settingsrow_title_corner_slots(lv_obj_t *title, gui_config_t *cfg, int buttons) {
	(void)title;
	(void)cfg;
	assert(buttons == 0);
}
lv_obj_t *settingsrow_page(lv_obj_t *screen, gui_config_t *cfg, const char *title) {
	(void)cfg;
	lv_obj_t *container = lv_obj_create(screen);
	lv_obj_remove_style_all(container);
	lv_obj_set_size(container, lv_pct(100), lv_pct(100));
	lv_obj_set_style_pad_all(container, 6, 0);
	lv_obj_set_style_pad_gap(container, 6, 0);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(container, true);
	page_title = lv_label_create(container);
	lv_label_set_text(page_title, tr(title));
	lv_obj_set_style_text_font(page_title, &font_ui_20, 0);
	return container;
}
lv_obj_t *settingsrow_action(lv_obj_t *parent, const char *name, lv_event_cb_t cb, void *user_data) {
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, 48);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, tr(name));
	lv_obj_set_style_text_font(label, &font_ui_16, 0);
	lv_obj_center(label);
	lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
	return row;
}
void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }

bool gui_card_available(void) { return card_available && !usb_active; }
void gui_notify_no_card(void) { no_card_calls++; }
void gui_notify_popup(const char *text) { snprintf(last_popup, sizeof(last_popup), "%s", text); }
bool usb_storage_active(void) { return usb_active; }
bool storage_card_attached(void) { return card_available; }
const char *storage_sd_device(void) { return card_available ? "/dev/fake-card" : NULL; }
void thread_be_background(const char *name) {
	(void)name;
	background_calls++;
}
void sysinfo_format_size(uint64_t bytes, char *out, size_t size) {
	if (bytes == 64ull * 1024ull * 1024ull) {
		snprintf(out, size, "64 MB");
	} else {
		snprintf(out, size, "%llu B", (unsigned long long)bytes);
	}
}

static char language[64] = "English";
const char *config_get(const char *section, const char *key, const char *fallback) {
	if (section && key && strcmp(section, "ui") == 0 && strcmp(key, "language") == 0) {
		return language[0] ? language : fallback;
	}
	return fallback;
}
void config_set(const char *section, const char *key, const char *value) {
	if (section && key && strcmp(section, "ui") == 0 && strcmp(key, "language") == 0) {
		snprintf(language, sizeof(language), "%s", value ? value : "");
	}
}
bool config_save(void) { return true; }

// ---------------------------------------------------------------------------
// the injected benchmark
// ---------------------------------------------------------------------------

enum {
	RUN_GOOD,
	RUN_BELOW_REFERENCE,
	RUN_READ_ERRORS,
	RUN_SLOW_BLOCKS,
	RUN_REPEATED_DATA,
	RUN_CANCELLED,
	RUN_NO_CARD,
	RUN_OPEN_FAILED,
};

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
						  void *(*start_routine)(void *), void *arg) {
	(void)attr;
	if (fail_thread) {
		return -1;
	}
	if (thread) {
		*thread = (pthread_t)0;
	}
	if (!skip_worker) {
		pending_worker = NULL;
		pending_arg = NULL;
		worker_calls++;
		start_routine(arg);
	} else {
		pending_worker = start_routine;
		pending_arg = arg;
	}
	return 0;
}
int __wrap_pthread_detach(pthread_t thread) {
	(void)thread;
	return 0;
}

static void finish_pending_worker(void) {
	assert(pending_worker);
	allow_cancelled_worker = true;
	worker_calls++;
	pending_worker(pending_arg);
	pending_worker = NULL;
	pending_arg = NULL;
	allow_cancelled_worker = false;
}

static void fill_result(sdbench_result_t *r) {
	memset(r, 0, sizeof(*r));
	r->status = SDBENCH_OK;
	r->phase = SDBENCH_PHASE_PROBE;
	r->verdict = SDBENCH_VERDICT_INCOMPLETE;
	r->card_bytes = 64ull * 1024ull * 1024ull;
	r->direct_io = true;
	r->probe_reads = 5;
	r->seq_reads = 1234;
	r->random_reads = 1234;
	r->seq_bytes = (uint64_t)1234 * SDBENCH_BLOCK_BYTES;
	r->random_bytes = (uint64_t)1234 * SDBENCH_BLOCK_BYTES;
	r->seq_bps = 20000000ull;
	r->random_bps = 8000000ull;
	r->random_iops = 1953;
	r->max_read_ms = 4;
	r->elapsed_ms = SDBENCH_DURATION_MS;
	r->first_error_offset = SDBENCH_NO_OFFSET;
	r->first_slow_offset = SDBENCH_NO_OFFSET;
	r->first_repeat_offset = SDBENCH_NO_OFFSET;
}

static void finish_result(sdbench_result_t *r) {
	r->phase = SDBENCH_PHASE_DONE;
	if (r->status == SDBENCH_OK) {
		r->verdict = sdbench_verdict(r);
	} else {
		r->verdict = SDBENCH_VERDICT_INCOMPLETE;
	}
}

bool __wrap_sdbench_run(const char *device, bool *cancel, uint32_t duration_ms,
						sdbench_progress_cb progress, void *user, sdbench_result_t *out) {
	run_calls++;
	assert(device && strcmp(device, "/dev/fake-card") == 0);
	assert(duration_ms == SDBENCH_DURATION_MS);
	assert(progress && cancel);
	bool was_cancelled = __atomic_load_n(cancel, __ATOMIC_RELAXED);
	last_run_cancelled = was_cancelled;
	assert(!was_cancelled || allow_cancelled_worker);
	if (was_cancelled) {
		// A cancel tap, USB handover, pulled card or the watchdog set this
		// run's flag. A real runner notices between reads and leaves; a stale
		// worker's progress must not overwrite a newer watchdog result.
		memset(out, 0, sizeof(*out));
		out->status = SDBENCH_CANCELLED;
		out->phase = SDBENCH_PHASE_DONE;
		out->verdict = SDBENCH_VERDICT_INCOMPLETE;
		out->progress = 42;
		out->first_error_offset = SDBENCH_NO_OFFSET;
		out->first_slow_offset = SDBENCH_NO_OFFSET;
		out->first_repeat_offset = SDBENCH_NO_OFFSET;
		progress(out, user);
		progress_calls++;
		return false;
	}
	fill_result(out);

	progress(out, user);
	progress_calls++;
	out->phase = SDBENCH_PHASE_SEQUENTIAL;
	out->progress = 50;
	out->elapsed_ms = SDBENCH_DURATION_MS / 2;
	progress(out, user);
	progress_calls++;

	switch (scenario) {
	case RUN_BELOW_REFERENCE:
		out->seq_bps = 5000000ull;
		out->random_bps = 2000000ull;
		out->random_iops = 700;
		out->max_read_ms = 4;
		break;
	case RUN_READ_ERRORS:
		out->read_errors = 2;
		out->first_error_offset = 12345;
		break;
	case RUN_SLOW_BLOCKS:
		out->slow_reads = 1;
		out->max_read_ms = 312;
		out->first_slow_offset = 54321;
		break;
	case RUN_REPEATED_DATA:
		out->repeated_probes = 1;
		out->first_repeat_offset = 99999;
		break;
	case RUN_CANCELLED:
		out->status = SDBENCH_CANCELLED;
		out->progress = 42;
		__atomic_store_n(cancel, true, __ATOMIC_RELAXED);
		break;
	case RUN_NO_CARD:
	case RUN_OPEN_FAILED:
		fill_result(out);
		out->card_bytes = 0;
		out->probe_reads = out->seq_reads = out->random_reads = 0;
		out->seq_bytes = out->random_bytes = 0;
		out->seq_bps = out->random_bps = 0;
		out->random_iops = 0;
		out->max_read_ms = 0;
		out->status = scenario == RUN_NO_CARD ? SDBENCH_NO_CARD : SDBENCH_OPEN_FAILED;
		out->progress = 0;
		break;
	default:
		break;
	}
	if (scenario != RUN_NO_CARD && scenario != RUN_OPEN_FAILED && scenario != RUN_CANCELLED) {
		out->progress = 100;
	}
	finish_result(out);
	progress(out, user);
	progress_calls++;
	return out->status == SDBENCH_OK;
}

// ---------------------------------------------------------------------------
// LVGL helpers
// ---------------------------------------------------------------------------

static lv_obj_t *page;

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
		return obj;
	}
	uint32_t count = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *found = find_label(lv_obj_get_child(obj, (int32_t)i), text);
		if (found) {
			return found;
		}
	}
	return NULL;
}

static lv_obj_t *find_bar(lv_obj_t *obj) {
	if (lv_obj_check_type(obj, &lv_bar_class)) {
		return obj;
	}
	uint32_t count = lv_obj_get_child_count(obj);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *found = find_bar(lv_obj_get_child(obj, (int32_t)i));
		if (found) {
			return found;
		}
	}
	return NULL;
}

static lv_obj_t *named(lv_obj_t *root, const char *tag) {
	lv_obj_t *found = find_label(root, tr(tag));
	assert(found);
	return found;
}

static void check_metric(lv_obj_t *root, const char *tag, const char *value) {
	lv_obj_t *name = named(root, tag);
	lv_obj_t *row = lv_obj_get_parent(name);
	lv_obj_t *shown = find_label(row, value);
	assert(shown);
	lv_obj_update_layout(root);
	lv_area_t a, b;
	lv_obj_get_coords(name, &a);
	lv_obj_get_coords(shown, &b);
	assert(a.x2 <= b.x1 + 2); // the name dots out before the value begins
	assert(a.x1 >= 0 && b.x2 <= 240);
}

static void advance(unsigned ticks) {
	for (unsigned i = 0; i < ticks; i++) {
		lv_tick_inc(100);
		lv_timer_handler();
	}
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
	(void)area;
	(void)pixels;
	lv_display_flush_ready(display);
}

static lv_obj_t *run_row(void) { return lv_obj_get_parent(named(page, "sd_health_run")); }
static lv_obj_t *cancel_row(void) { return lv_obj_get_parent(named(page, "sd_health_cancel")); }

static void start_test(int which) {
	scenario = which;
	skip_worker = false;
	fail_thread = false;
	card_available = true;
	usb_active = false;
	lv_obj_send_event(run_row(), LV_EVENT_CLICKED, NULL);
}

static void start_and_settle(int which) {
	start_test(which);
	advance(2);
}

// Start a run whose worker is deliberately held back, so the UI can act on a
// still-running test. finish_pending_worker() lets it return afterwards.
static void start_pending_test(int which) {
	scenario = which;
	skip_worker = true;
	fail_thread = false;
	card_available = true;
	usb_active = false;
	last_run_cancelled = false;
	lv_obj_send_event(run_row(), LV_EVENT_CLICKED, NULL);
	assert(pending_worker);
}

static void finish_cancelled_pending(void) {
	finish_pending_worker();
	assert(last_run_cancelled);
	advance(2);
	named(page, "sd_health_status_cancelled");
	assert(lv_obj_is_hidden(find_bar(page)));
	assert(!lv_obj_has_state(run_row(), LV_STATE_DISABLED));
	assert(lv_obj_is_hidden(cancel_row()));
	skip_worker = false;
}

// ---------------------------------------------------------------------------
// the checks
// ---------------------------------------------------------------------------

static const char *const translation_keys[] = {
	"sd_health_title", "sd_health_run", "sd_health_cancel", "sd_health_not_run",
	"sd_health_running", "sd_health_idle_note", "sd_health_running_note",
	"sd_health_good", "sd_health_good_note", "sd_health_below_reference",
	"sd_health_below_reference_note", "sd_health_slow_blocks", "sd_health_slow_blocks_note",
	"sd_health_read_errors", "sd_health_read_errors_note", "sd_health_repeated_data",
	"sd_health_repeated_data_note", "sd_health_incomplete", "sd_health_incomplete_note",
	"sd_health_status_no_card", "sd_health_status_usb_busy", "sd_health_status_open_failed",
	"sd_health_status_too_small", "sd_health_status_no_memory", "sd_health_status_cancelled",
	"sd_health_status_start_failed", "sd_health_status_stalled", "sd_health_status_incomplete",
	"sd_health_seq", "sd_health_random", "sd_health_iops", "sd_health_latency",
	"sd_health_errors", "sd_health_slow", "sd_health_repeats", "sd_health_capacity",
	"sd_health_blocks", "sd_health_first_error", "sd_health_first_slow",
	"sd_health_first_repeat", "sd_health_help", "sd_health_limits_note",
};

int main(int argc, char **argv) {
	assert(argc == 2);
	lv_init();
	font_ui_14 = lv_font_montserrat_14;
	font_ui_16 = lv_font_montserrat_16;
	font_ui_20 = lv_font_montserrat_20;
	palette.accent = lv_color_make(80, 160, 240);
	palette.text_primary = lv_color_make(240, 240, 240);
	palette.text_secondary = lv_color_make(150, 150, 150);
	palette.surface = lv_color_make(30, 30, 40);
	lv_style_init(&theme_style_card);
	lv_style_set_bg_opa(&theme_style_card, LV_OPA_COVER);
	lv_style_set_bg_color(&theme_style_card, palette.surface);
	lv_style_set_text_color(&theme_style_card, palette.text_primary);
	lv_style_init(&theme_style_text);
	lv_style_set_text_color(&theme_style_text, palette.text_primary);
	lv_style_init(&theme_style_text_dim);
	lv_style_set_text_color(&theme_style_text_dim, palette.text_secondary);

	lv_display_t *display = lv_display_create(240, 320);
	static uint8_t framebuffer[240 * 320 * 4];
	lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(display, flush_cb);

	lang_init(argv[1]);
	assert(strcmp(lang_current(), "English") == 0);
	gui_config_t cfg = {.screen_width = 240, .screen_height = 320, .padding = 6, .top_bar_height = 24};
	page = sdhealthpage_init(&cfg);
	lv_screen_load(page);
	advance(2);

	named(page, "sd_health_title");
	named(page, "sd_health_not_run");
	named(page, "sd_health_idle_note");
	named(page, "sd_health_help");
	named(page, "sd_health_limits_note");
	check_metric(page, "sd_health_seq", "—");
	check_metric(page, "sd_health_random", "—");
	check_metric(page, "sd_health_iops", "—");
	check_metric(page, "sd_health_latency", "—");
	check_metric(page, "sd_health_errors", "—");
	check_metric(page, "sd_health_slow", "—");
	check_metric(page, "sd_health_repeats", "—");
	check_metric(page, "sd_health_capacity", "—");
	check_metric(page, "sd_health_blocks", "—");
	check_metric(page, "sd_health_first_error", "—");
	check_metric(page, "sd_health_first_slow", "—");
	check_metric(page, "sd_health_first_repeat", "—");
	lv_obj_t *bar = find_bar(page);
	assert(bar && lv_obj_is_hidden(bar));
	assert(!lv_obj_has_state(run_row(), LV_STATE_DISABLED));
	assert(lv_obj_is_hidden(cancel_row()));
	assert(refresh_cb);

	// USB mass storage and a missing card never start a worker.
	unsigned before = run_calls;
	usb_active = true;
	lv_obj_send_event(run_row(), LV_EVENT_CLICKED, NULL);
	assert(run_calls == before && strcmp(last_popup, "usb_storage_shared") == 0);
	usb_active = false;
	card_available = false;
	lv_obj_send_event(run_row(), LV_EVENT_CLICKED, NULL);
	assert(run_calls == before && no_card_calls == 1);
	card_available = true;
	advance(2);

	// A healthy run: the running state is painted before the worker, the final
	// numbers after the poll timer sees the worker's result.
	progress_calls = 0;
	run_calls = worker_calls = background_calls = 0;
	start_test(RUN_GOOD);
	assert(run_calls == 1 && worker_calls == 1 && background_calls == 1);
	assert(progress_calls == 3);
	named(page, "sd_health_running");
	assert(!lv_obj_is_hidden(bar));
	assert(lv_obj_has_state(run_row(), LV_STATE_DISABLED));
	assert(!lv_obj_is_hidden(cancel_row()));
	advance(2);
	named(page, "sd_health_good");
	named(page, "sd_health_good_note");
	check_metric(page, "sd_health_seq", "20.0 MB/s");
	check_metric(page, "sd_health_random", "8.0 MB/s");
	check_metric(page, "sd_health_iops", "1953 IOPS");
	check_metric(page, "sd_health_latency", "4 ms");
	check_metric(page, "sd_health_errors", "0");
	check_metric(page, "sd_health_slow", "0");
	check_metric(page, "sd_health_repeats", "0");
	check_metric(page, "sd_health_capacity", "64 MB");
	check_metric(page, "sd_health_blocks", "2473");
	check_metric(page, "sd_health_first_error", "—");
	check_metric(page, "sd_health_first_slow", "—");
	check_metric(page, "sd_health_first_repeat", "—");
	assert(lv_obj_is_hidden(bar));
	assert(!lv_obj_has_state(run_row(), LV_STATE_DISABLED));
	assert(lv_obj_is_hidden(cancel_row()));

	start_and_settle(RUN_BELOW_REFERENCE);
	named(page, "sd_health_below_reference");
	named(page, "sd_health_below_reference_note");
	check_metric(page, "sd_health_seq", "5.0 MB/s");
	check_metric(page, "sd_health_iops", "700 IOPS");

	start_and_settle(RUN_READ_ERRORS);
	named(page, "sd_health_read_errors");
	named(page, "sd_health_read_errors_note");
	check_metric(page, "sd_health_errors", "2");
	check_metric(page, "sd_health_first_error", "12345 B");

	start_and_settle(RUN_SLOW_BLOCKS);
	named(page, "sd_health_slow_blocks");
	named(page, "sd_health_slow_blocks_note");
	check_metric(page, "sd_health_latency", "312 ms");
	check_metric(page, "sd_health_slow", "1");
	check_metric(page, "sd_health_first_slow", "54321 B");

	start_and_settle(RUN_REPEATED_DATA);
	named(page, "sd_health_repeated_data");
	named(page, "sd_health_repeated_data_note");
	check_metric(page, "sd_health_repeats", "1");
	check_metric(page, "sd_health_first_repeat", "99999 B");

	start_and_settle(RUN_CANCELLED);
	named(page, "sd_health_status_cancelled");
	named(page, "sd_health_incomplete_note");

	start_and_settle(RUN_NO_CARD);
	named(page, "sd_health_status_no_card");
	check_metric(page, "sd_health_seq", "—");
	check_metric(page, "sd_health_capacity", "—");

	start_and_settle(RUN_OPEN_FAILED);
	named(page, "sd_health_status_open_failed");

	// The cancel row flips this run's own flag between reads; the held-back
	// worker then sees it and the page returns to its idle, re-armed state.
	start_pending_test(RUN_GOOD);
	named(page, "sd_health_running");
	lv_obj_send_event(cancel_row(), LV_EVENT_CLICKED, NULL);
	named(page, "sd_health_status_cancelled");
	assert(lv_obj_has_state(cancel_row(), LV_STATE_DISABLED));
	finish_cancelled_pending();

	// USB handover and a pulled card cancel a run already in progress, even
	// though both were checked before it started.
	start_pending_test(RUN_GOOD);
	usb_active = true;
	advance(1);
	usb_active = false;
	finish_cancelled_pending();

	start_pending_test(RUN_GOOD);
	card_available = false;
	advance(1);
	card_available = true;
	finish_cancelled_pending();

	// A thread that cannot be created is a result, not a silently dead row.
	fail_thread = true;
	scenario = RUN_GOOD;
	lv_obj_send_event(run_row(), LV_EVENT_CLICKED, NULL);
	advance(2);
	named(page, "sd_health_status_start_failed");
	fail_thread = false;

	// A read that never returns: the watchdog invalidates the worker's
	// generation after 15 s, so the page stays usable and says what happened.
	before = run_calls;
	unsigned workers = worker_calls;
	start_pending_test(RUN_GOOD);
	assert(run_calls == before && worker_calls == workers);
	named(page, "sd_health_running");
	assert(!lv_obj_is_hidden(bar));
	advance(160);
	named(page, "sd_health_status_stalled");
	assert(lv_obj_is_hidden(bar));
	assert(lv_obj_is_hidden(cancel_row()));
	assert(!lv_obj_has_state(run_row(), LV_STATE_DISABLED));
	// The stuck read eventually returns with its own cancellation. Its result
	// belongs to an invalidated generation and must not replace the watchdog's.
	finish_pending_worker();
	assert(last_run_cancelled);
	assert(run_calls == before + 1 && worker_calls == workers + 1);
	advance(1);
	named(page, "sd_health_status_stalled");
	assert(lv_obj_is_hidden(bar));
	skip_worker = false;

	// Theme refresh must repaint hand-set colors without losing the result.
	palette.accent = lv_color_make(240, 80, 80);
	refresh_cb();
	advance(1);
	start_and_settle(RUN_GOOD);
	named(page, "sd_health_good");

	// Larger text and every shipped language: all new keys must exist, the
	// metric rows must still share their width, and the verdict must relabel.
	const char *languages[16];
	int count = lang_list(languages, 16);
	assert(count == 11);
	font_ui_14 = font_ui_16 = lv_font_montserrat_20;
	font_ui_20 = lv_font_montserrat_20;
	lv_obj_report_style_change(NULL);
	for (int i = 0; i < count; i++) {
		assert(lang_set(languages[i]));
		advance(2);
		for (size_t k = 0; k < sizeof(translation_keys) / sizeof(translation_keys[0]); k++) {
			assert(strcmp(tr(translation_keys[k]), translation_keys[k]) != 0);
		}
		named(page, "sd_health_good");
		named(page, "sd_health_good_note");
		named(page, "sd_health_run");
		named(page, "sd_health_cancel");
		check_metric(page, "sd_health_seq", "20.0 MB/s");
		check_metric(page, "sd_health_random", "8.0 MB/s");
		check_metric(page, "sd_health_iops", "1953 IOPS");
		check_metric(page, "sd_health_latency", "4 ms");
		check_metric(page, "sd_health_errors", "0");
		check_metric(page, "sd_health_slow", "0");
		check_metric(page, "sd_health_repeats", "0");
		check_metric(page, "sd_health_capacity", "64 MB");
		check_metric(page, "sd_health_blocks", "2473");
		check_metric(page, "sd_health_first_error", "—");
		check_metric(page, "sd_health_first_slow", "—");
		check_metric(page, "sd_health_first_repeat", "—");
		lv_obj_t *note = named(page, "sd_health_good_note");
		lv_obj_update_layout(page);
		lv_area_t a;
		lv_obj_get_coords(note, &a);
		assert(a.x1 >= 0 && a.x2 <= 240);
	}

	puts("PASS SD card health UI (240x320, worker, verdicts, start/mid-run guards, cancel, stall watchdog, theme, 11 languages, larger text)");
	return 0;
}

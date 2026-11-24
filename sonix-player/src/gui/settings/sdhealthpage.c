#include "sdhealthpage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/sdbench.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"

// The card is read on a detached SCHED_IDLE worker. This page never reads a
// block: it copies a mutex-protected snapshot on its own timer. That is the
// whole point of the feature -- a card that stalls on one 4K read must not
// stall the interface that is measuring it.
#define POLL_MS 100
#define STALL_EXTRA_MS 10000 // 5 s of reads + 10 s before giving up on a hung read
#define NO_VALUE "\xE2\x80\x94" // em dash
#define WARN_COLOR lv_color_make(224, 27, 36)
#define GOOD_GREEN lv_color_make(46, 194, 126)

typedef struct {
	bool busy;
	bool have_result;
	bool cancelling;
	sdbench_result_t result;
} view_t;

// One allocation per run. A hung read may outlive the page's 15-second
// watchdog; giving that worker its own cancel flag and keeping `current`
// under the mutex means a later run cannot clear the old worker's cancellation
// or publish its result. The worker frees its state when the kernel call
// eventually returns (the page never frees a state that may still be in use).
typedef struct {
	bool cancel_requested;
	unsigned generation;
} run_state_t;

static lv_obj_t *screen;
static lv_obj_t *verdict_value, *verdict_note, *progress_bar;
static lv_obj_t *seq_value, *random_value, *iops_value, *latency_value;
static lv_obj_t *errors_value, *slow_value, *repeats_value;
static lv_obj_t *capacity_value, *blocks_value;
static lv_obj_t *first_error_value, *first_slow_value, *first_repeat_value;
static lv_obj_t *run_row, *cancel_row;
static lv_timer_t *poll;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool busy;            // under lock
static bool have_result;     // under lock
static sdbench_result_t live; // under lock, written by the worker/progress callback
static unsigned generation;  // under lock; stale workers are ignored
static run_state_t *current; // under lock; NULL once a worker is orphaned
static uint32_t started_tick; // interface thread only

// ---------------------------------------------------------------------------
// the page's shapes
// ---------------------------------------------------------------------------

static void set_text(lv_obj_t *label, const char *text) {
	if (strcmp(lv_label_get_text(label), text) != 0) {
		lv_label_set_text(label, text);
	}
}

static lv_obj_t *label_text(lv_obj_t *parent, const char *text, bool dim) {
	lv_obj_t *obj = lv_label_create(parent);
	lv_obj_add_style(obj, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(obj, dim ? &font_ui_14 : &font_ui_16, 0);
	lv_label_set_text(obj, text);
	return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *key, bool dim) {
	return label_text(parent, tr(key), dim);
}

static lv_obj_t *paragraph(lv_obj_t *parent, const char *key, bool dim) {
	lv_obj_t *obj = label(parent, key, dim);
	lv_obj_set_width(obj, lv_pct(100));
	return obj;
}

static lv_obj_t *card(lv_obj_t *parent) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_add_style(obj, &theme_style_card, 0);
	lv_obj_set_size(obj, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_radius(obj, 10, 0);
	lv_obj_set_style_border_width(obj, 0, 0);
	lv_obj_set_style_shadow_width(obj, 0, 0);
	lv_obj_set_style_pad_all(obj, 8, 0);
	lv_obj_set_style_pad_gap(obj, 4, 0);
	lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_clickable(obj, false);
	return obj;
}

static lv_obj_t *metric(lv_obj_t *parent, const char *name) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_gap(row, 6, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_clickable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *name_label = label(row, name, true);
	lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name_label, 1);
	lv_obj_set_flex_grow(name_label, 1);

	lv_obj_t *value = label_text(row, NO_VALUE, false);
	lv_obj_set_width(value, lv_pct(55));
	lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
	return value;
}

// ---------------------------------------------------------------------------
// what the answer says
// ---------------------------------------------------------------------------

static const char *status_title(sdbench_status_t status) {
	switch (status) {
	case SDBENCH_NO_CARD:
		return "sd_health_status_no_card";
	case SDBENCH_USB_BUSY:
		return "sd_health_status_usb_busy";
	case SDBENCH_OPEN_FAILED:
		return "sd_health_status_open_failed";
	case SDBENCH_TOO_SMALL:
		return "sd_health_status_too_small";
	case SDBENCH_NO_MEMORY:
		return "sd_health_status_no_memory";
	case SDBENCH_CANCELLED:
		return "sd_health_status_cancelled";
	case SDBENCH_START_FAILED:
		return "sd_health_status_start_failed";
	case SDBENCH_STALLED:
		return "sd_health_status_stalled";
	default:
		return "sd_health_status_incomplete";
	}
}

static const char *verdict_title(sdbench_verdict_t verdict) {
	switch (verdict) {
	case SDBENCH_VERDICT_GOOD:
		return "sd_health_good";
	case SDBENCH_VERDICT_BELOW_REFERENCE:
		return "sd_health_below_reference";
	case SDBENCH_VERDICT_SLOW_BLOCKS:
		return "sd_health_slow_blocks";
	case SDBENCH_VERDICT_READ_ERRORS:
		return "sd_health_read_errors";
	case SDBENCH_VERDICT_REPEATED_DATA:
		return "sd_health_repeated_data";
	default:
		return "sd_health_incomplete";
	}
}

static const char *verdict_note_key(sdbench_verdict_t verdict) {
	switch (verdict) {
	case SDBENCH_VERDICT_GOOD:
		return "sd_health_good_note";
	case SDBENCH_VERDICT_BELOW_REFERENCE:
		return "sd_health_below_reference_note";
	case SDBENCH_VERDICT_SLOW_BLOCKS:
		return "sd_health_slow_blocks_note";
	case SDBENCH_VERDICT_READ_ERRORS:
		return "sd_health_read_errors_note";
	case SDBENCH_VERDICT_REPEATED_DATA:
		return "sd_health_repeated_data_note";
	default:
		return "sd_health_incomplete_note";
	}
}

static lv_color_t verdict_color(const view_t *v) {
	if (v->busy) {
		return theme()->accent;
	}
	if (!v->have_result || v->result.status != SDBENCH_OK) {
		return theme()->text_primary;
	}
	switch (v->result.verdict) {
	case SDBENCH_VERDICT_GOOD:
		return GOOD_GREEN;
	case SDBENCH_VERDICT_BELOW_REFERENCE:
	case SDBENCH_VERDICT_SLOW_BLOCKS:
	case SDBENCH_VERDICT_READ_ERRORS:
	case SDBENCH_VERDICT_REPEATED_DATA:
		return WARN_COLOR;
	default:
		return theme()->text_primary;
	}
}

static void set_metric(lv_obj_t *label, const char *text, bool warning) {
	set_text(label, text);
	lv_obj_set_style_text_color(label, warning ? WARN_COLOR : theme()->text_primary, 0);
}

static void show_rate(lv_obj_t *label, uint32_t reads, uint64_t bytes_per_s, bool warning) {
	char text[32];
	if (!reads) {
		set_metric(label, NO_VALUE, false);
		return;
	}
	sdbench_format_rate(bytes_per_s, text, sizeof(text));
	set_metric(label, text, warning);
}

static void show_count(lv_obj_t *label, uint32_t count) {
	char text[24];
	snprintf(text, sizeof(text), "%u", count);
	set_metric(label, text, count > 0);
}

static void show_offset(lv_obj_t *label, uint64_t offset) {
	char text[64];
	if (offset == SDBENCH_NO_OFFSET) {
		set_metric(label, NO_VALUE, false);
		return;
	}
	sysinfo_format_size(offset, text, sizeof(text));
	set_metric(label, text, true);
}

static void paint(const view_t *v) {
	const sdbench_result_t *r = &v->result;
	bool cancelling = v->cancelling;

	const char *title, *note;
	if (v->busy) {
		title = tr(cancelling ? "sd_health_status_cancelled" : "sd_health_running");
		note = tr(cancelling ? "sd_health_incomplete_note" : "sd_health_running_note");
	} else if (!v->have_result) {
		title = tr("sd_health_not_run");
		note = tr("sd_health_idle_note");
	} else if (r->status != SDBENCH_OK) {
		title = tr(status_title(r->status));
		note = tr("sd_health_incomplete_note");
	} else {
		title = tr(verdict_title(r->verdict));
		note = tr(verdict_note_key(r->verdict));
	}
	set_text(verdict_value, title);
	set_text(verdict_note, note);
	lv_obj_set_style_text_color(verdict_value, verdict_color(v), 0);
	lv_obj_set_style_text_font(verdict_value, &font_ui_20, 0);

	lv_obj_set_hidden(progress_bar, !v->busy);
	lv_bar_set_value(progress_bar, v->busy ? r->progress : (v->have_result ? 100 : 0), LV_ANIM_OFF);
	lv_obj_set_state(run_row, LV_STATE_DISABLED, v->busy);
	lv_obj_set_hidden(cancel_row, !v->busy);
	lv_obj_set_state(cancel_row, LV_STATE_DISABLED, cancelling || !v->busy);

	bool done = !v->busy && v->have_result && r->status == SDBENCH_OK;
	bool slow_rate = done && (r->seq_bps < SDBENCH_MIN_SEQ_BPS || r->random_iops < SDBENCH_MIN_RANDOM_IOPS);
	if (!v->have_result) {
		set_metric(seq_value, NO_VALUE, false);
		set_metric(random_value, NO_VALUE, false);
		set_metric(iops_value, NO_VALUE, false);
		set_metric(latency_value, NO_VALUE, false);
		set_metric(errors_value, NO_VALUE, false);
		set_metric(slow_value, NO_VALUE, false);
		set_metric(repeats_value, NO_VALUE, false);
		set_metric(capacity_value, NO_VALUE, false);
		set_metric(blocks_value, NO_VALUE, false);
		set_metric(first_error_value, NO_VALUE, false);
		set_metric(first_slow_value, NO_VALUE, false);
		set_metric(first_repeat_value, NO_VALUE, false);
		return;
	}

	show_rate(seq_value, r->seq_reads, r->seq_bps, slow_rate);
	show_rate(random_value, r->random_reads, r->random_bps, slow_rate);
	char text[64];
	if (r->random_reads) {
		snprintf(text, sizeof(text), "%u IOPS", r->random_iops);
		set_metric(iops_value, text, slow_rate);
	} else {
		set_metric(iops_value, NO_VALUE, false);
	}
	uint32_t total_reads = r->probe_reads + r->seq_reads + r->random_reads;
	if (total_reads) {
		if (r->max_read_ms) {
			snprintf(text, sizeof(text), "%u ms", r->max_read_ms);
		} else {
			snprintf(text, sizeof(text), "<1 ms");
		}
		set_metric(latency_value, text, r->slow_reads > 0);
	} else {
		set_metric(latency_value, NO_VALUE, false);
	}
	show_count(errors_value, r->read_errors);
	show_count(slow_value, r->slow_reads);
	show_count(repeats_value, r->repeated_probes);

	if (r->card_bytes) {
		sysinfo_format_size(r->card_bytes, text, sizeof(text));
		set_metric(capacity_value, text, false);
	} else {
		set_metric(capacity_value, NO_VALUE, false);
	}
	snprintf(text, sizeof(text), "%u", total_reads);
	set_metric(blocks_value, text, false);
	show_offset(first_error_value, r->first_error_offset);
	show_offset(first_slow_value, r->first_slow_offset);
	show_offset(first_repeat_value, r->first_repeat_offset);
}

static void snapshot(view_t *out) {
	pthread_mutex_lock(&lock);
	out->busy = busy;
	out->have_result = have_result;
	out->cancelling = current && __atomic_load_n(&current->cancel_requested, __ATOMIC_RELAXED);
	out->result = live;
	pthread_mutex_unlock(&lock);
}

static void refresh(void) {
	view_t v;
	snapshot(&v);
	paint(&v);
}

// ---------------------------------------------------------------------------
// one run
// ---------------------------------------------------------------------------

static void progress_cb(const sdbench_result_t *partial, void *user) {
	run_state_t *state = (run_state_t *)user;
	pthread_mutex_lock(&lock);
	if (state && state->generation == generation) {
		live = *partial;
		have_result = true;
	}
	pthread_mutex_unlock(&lock);
}

static void *worker_main(void *arg) {
	run_state_t *state = (run_state_t *)arg;
	unsigned gen = state->generation;
	thread_be_background("sd card health");

	const char *device = storage_sd_device();
	sdbench_result_t result;
	(void)sdbench_run(device, &state->cancel_requested, SDBENCH_DURATION_MS, progress_cb,
					  state, &result);

	pthread_mutex_lock(&lock);
	if (gen == generation) {
		live = result;
		have_result = true;
		busy = false;
	}
	if (current == state) {
		current = NULL;
	}
	pthread_mutex_unlock(&lock);
	// The UI no longer holds this state, so the worker is its last owner even
	// when its generation was invalidated by the stall watchdog.
	free(state);
	return NULL;
}

static void result_reset(sdbench_result_t *r) {
	memset(r, 0, sizeof(*r));
	r->status = SDBENCH_OK;
	r->phase = SDBENCH_PHASE_PROBE;
	r->verdict = SDBENCH_VERDICT_INCOMPLETE;
	r->first_error_offset = SDBENCH_NO_OFFSET;
	r->first_slow_offset = SDBENCH_NO_OFFSET;
	r->first_repeat_offset = SDBENCH_NO_OFFSET;
}

static void start_cb(lv_event_t *e) {
	(void)e;
	if (usb_storage_active()) {
		gui_notify_popup("usb_storage_shared");
		return;
	}
	if (!gui_card_available() || !storage_sd_device()) {
		gui_notify_no_card();
		return;
	}

	run_state_t *state = calloc(1, sizeof(*state));
	if (!state) {
		pthread_mutex_lock(&lock);
		have_result = true;
		result_reset(&live);
		live.status = SDBENCH_NO_MEMORY;
		live.phase = SDBENCH_PHASE_DONE;
		pthread_mutex_unlock(&lock);
		refresh();
		return;
	}

	pthread_mutex_lock(&lock);
	if (busy) {
		pthread_mutex_unlock(&lock);
		free(state);
		return;
	}
	busy = true;
	generation++;
	state->generation = generation;
	__atomic_store_n(&state->cancel_requested, false, __ATOMIC_RELAXED);
	current = state;
	have_result = false;
	result_reset(&live);
	pthread_mutex_unlock(&lock);

	started_tick = lv_tick_get();
	refresh();

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, state) != 0) {
		pthread_mutex_lock(&lock);
		if (current == state) {
			current = NULL;
		}
		busy = false;
		have_result = true;
		result_reset(&live);
		live.status = SDBENCH_START_FAILED;
		live.phase = SDBENCH_PHASE_DONE;
		pthread_mutex_unlock(&lock);
		free(state);
		refresh();
		return;
	}
	pthread_detach(thread);
}

static void cancel_cb(lv_event_t *e) {
	(void)e;
	pthread_mutex_lock(&lock);
	if (current) {
		__atomic_store_n(&current->cancel_requested, true, __ATOMIC_RELAXED);
	}
	pthread_mutex_unlock(&lock);
	refresh();
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	// A USB handover or a pulled card must not leave a worker reading a device
	// the host now owns. The worker sees the flag between reads. Keep the
	// pointer under the mutex, so it cannot be freed by a finishing worker
	// while the interface stores into it.
	bool card_gone = usb_storage_active() || !storage_card_attached();
	pthread_mutex_lock(&lock);
	if (card_gone && current) {
		__atomic_store_n(&current->cancel_requested, true, __ATOMIC_RELAXED);
	}
	bool running = busy;
	unsigned gen = generation;
	pthread_mutex_unlock(&lock);

	// A kernel read that never returns cannot be interrupted from userspace.
	// Stop waiting for it, invalidate its generation, and let the interface
	// carry on. The worker owns its state until that read returns; its result
	// is then discarded and the state is freed.
	if (running && lv_tick_elaps(started_tick) > SDBENCH_DURATION_MS + STALL_EXTRA_MS) {
		pthread_mutex_lock(&lock);
		if (busy && generation == gen) {
			if (current) {
				__atomic_store_n(&current->cancel_requested, true, __ATOMIC_RELAXED);
				current = NULL;
			}
			generation++;
			busy = false;
			have_result = true;
			result_reset(&live);
			live.status = SDBENCH_STALLED;
			live.phase = SDBENCH_PHASE_DONE;
			live.progress = 100;
		}
		pthread_mutex_unlock(&lock);
	}
	refresh();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_resume(poll);
	lv_timer_reset(poll);
	refresh();
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll);
}

static void theme_refresh(void) {
	lv_obj_set_style_bg_color(progress_bar, theme()->text_secondary, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(progress_bar, LV_OPA_40, LV_PART_MAIN);
	lv_obj_set_style_bg_color(progress_bar, theme()->accent, LV_PART_INDICATOR);
	refresh();
}

lv_obj_t *sdhealthpage_init(gui_config_t *cfg) {
	screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(screen, cfg, "sd_health_title");
	settingsrow_title_corner_slots(settingsrow_page_title(screen), cfg, 0);

	lv_obj_t *summary = card(container);
	verdict_value = paragraph(summary, "", false);
	lv_obj_set_style_text_font(verdict_value, &font_ui_20, 0);
	verdict_note = paragraph(summary, "", true);
	progress_bar = lv_bar_create(summary);
	lv_obj_set_width(progress_bar, lv_pct(100));
	lv_obj_set_height(progress_bar, 6);
	lv_bar_set_range(progress_bar, 0, 100);
	lv_obj_set_style_radius(progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_hidden(progress_bar, true);

	lv_obj_t *metrics = card(container);
	seq_value = metric(metrics, "sd_health_seq");
	random_value = metric(metrics, "sd_health_random");
	iops_value = metric(metrics, "sd_health_iops");
	latency_value = metric(metrics, "sd_health_latency");
	errors_value = metric(metrics, "sd_health_errors");
	slow_value = metric(metrics, "sd_health_slow");
	repeats_value = metric(metrics, "sd_health_repeats");
	first_error_value = metric(metrics, "sd_health_first_error");
	first_slow_value = metric(metrics, "sd_health_first_slow");
	first_repeat_value = metric(metrics, "sd_health_first_repeat");
	capacity_value = metric(metrics, "sd_health_capacity");
	blocks_value = metric(metrics, "sd_health_blocks");

	run_row = settingsrow_action(container, "sd_health_run", start_cb, NULL);
	cancel_row = settingsrow_action(container, "sd_health_cancel", cancel_cb, NULL);
	lv_obj_set_hidden(cancel_row, true);

	lv_obj_t *help = card(container);
	paragraph(help, "sd_health_help", true);
	paragraph(help, "sd_health_limits_note", true);

	poll = lv_timer_create(poll_cb, POLL_MS, NULL);
	lv_timer_pause(poll);
	lv_obj_add_event_cb(screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(screen, unloaded_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
	switcher_attach_back_gesture(screen);
	theme_register_refresh(theme_refresh);
	theme_refresh();
	return screen;
}

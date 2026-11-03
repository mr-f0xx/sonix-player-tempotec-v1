// Exercise the edge press lock with LVGL's actual pointer and scrolling code.
#include <assert.h>
#include <stdio.h>
#include "src/gui/shell/edge_lock.h"

lv_font_t font_ui_14;
static edge_lock_t lock;
static lv_point_t point;
static lv_indev_state_t state;
static bool enabled;
static int clicks;

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;
	data->point = point;
	data->state = state;
}

static void pointer_event(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_PRESSED && enabled)
		edge_lock_acquire(&lock, lv_event_get_param(e));
	if (lv_event_get_code(e) == LV_EVENT_RELEASED || lv_event_get_code(e) == LV_EVENT_INDEV_RESET)
		edge_lock_release(&lock);
}

static void clicked(lv_event_t *e) { (void)e; clicks++; }

static void read_at(lv_indev_t *indev, int x, int y, lv_indev_state_t s) {
	point.x = x; point.y = y; state = s;
	lv_tick_inc(20);
	lv_indev_read(indev);
}

int main(void) {
	lv_init();
	font_ui_14 = lv_font_montserrat_14;
	lv_display_create(240, 320);
	lv_obj_t *screen = lv_screen_active();
	lv_obj_t *list = lv_obj_create(screen);
	lv_obj_set_size(list, 240, 320);
	lv_obj_set_pos(list, 0, 0);
	lv_obj_set_style_pad_all(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_t *child = lv_obj_create(list);
	lv_obj_set_size(child, 200, 600);
	lv_obj_set_pos(child, 0, 0);
	lv_obj_set_scrollable(child, false);
	lv_obj_add_event_cb(child, clicked, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(screen);

	lv_indev_t *indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, read_pointer);
	lv_indev_add_event_cb(indev, pointer_event, LV_EVENT_ALL, NULL);

	// A pull cannot move the list under either a scrollable object or its child.
	enabled = true;
	read_at(indev, 80, 310, LV_INDEV_STATE_PRESSED);
	assert(lock.obj == child);
	assert(!lv_obj_is_scroll_chain_ver(child));
	read_at(indev, 80, 200, LV_INDEV_STATE_PRESSED);
	assert(lv_obj_get_scroll_y(list) == 0);
	lv_indev_wait_release(indev); // the sheet claims this pull
	read_at(indev, 80, 150, LV_INDEV_STATE_PRESSED);
	read_at(indev, 80, 150, LV_INDEV_STATE_RELEASED);
	edge_lock_release(&lock);
	assert(lv_obj_get_scroll_y(list) == 0 && clicks == 0);
	assert(lv_obj_is_scroll_chain_ver(child));
	assert(!lv_obj_is_scrollable(child));

	// Ordinary taps still reach the child, and a later unguarded swipe scrolls.
	read_at(indev, 80, 300, LV_INDEV_STATE_PRESSED);
	read_at(indev, 80, 300, LV_INDEV_STATE_RELEASED);
	assert(clicks == 1);
	enabled = false;
	read_at(indev, 80, 300, LV_INDEV_STATE_PRESSED);
	read_at(indev, 80, 200, LV_INDEV_STATE_PRESSED);
	assert(lv_obj_get_scroll_y(list) > 0);
	read_at(indev, 80, 200, LV_INDEV_STATE_RELEASED);

	// A scrollable target itself is locked too, with all of its original flags
	// restored (including flags that were already off).
	lv_obj_set_scroll_chain_hor(list, false);
	lv_obj_set_press_lock(list, false);
	edge_lock_acquire(&lock, list);
	assert(!lv_obj_is_scrollable(list));
	assert(lv_obj_is_press_lock(list));
	edge_lock_release(&lock);
	assert(lv_obj_is_scrollable(list));
	assert(!lv_obj_is_scroll_chain_hor(list));
	assert(!lv_obj_is_press_lock(list));

	// A deleted target cannot leave a dangling pointer in the lock.
	edge_lock_acquire(&lock, child);
	lv_obj_delete(child);
	assert(lock.obj == NULL);
	edge_lock_release(&lock);
	puts("edge lock: guarded scrolling, taps, normal scrolling and deletion passed");
	return 0;
}

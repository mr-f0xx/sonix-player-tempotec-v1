#pragma once

#include "lvgl.h"

// Temporarily keep an edge press on its original object and prevent that press
// from scrolling either it or an ancestor while its direction is undecided.
// The saved flags are restored even for objects that were not scrollable.
typedef struct {
	lv_obj_t *obj;
	bool scrollable;
	bool chain_hor;
	bool chain_ver;
	bool press_lock;
} edge_lock_t;

static void edge_lock_deleted(lv_event_t *e) {
	edge_lock_t *lock = lv_event_get_user_data(e);
	lock->obj = NULL;
}

static void edge_lock_release(edge_lock_t *lock) {
	lv_obj_t *obj = lock->obj;
	if (!obj) return;
	lock->obj = NULL;
	lv_obj_remove_event_cb_with_user_data(obj, edge_lock_deleted, lock);
	lv_obj_set_scrollable(obj, lock->scrollable);
	lv_obj_set_scroll_chain_hor(obj, lock->chain_hor);
	lv_obj_set_scroll_chain_ver(obj, lock->chain_ver);
	lv_obj_set_press_lock(obj, lock->press_lock);
}

static void edge_lock_acquire(edge_lock_t *lock, lv_obj_t *obj) {
	edge_lock_release(lock);
	if (!obj) return;
	lock->obj = obj;
	lock->scrollable = lv_obj_is_scrollable(obj);
	lock->chain_hor = lv_obj_is_scroll_chain_hor(obj);
	lock->chain_ver = lv_obj_is_scroll_chain_ver(obj);
	lock->press_lock = lv_obj_is_press_lock(obj);
	lv_obj_add_event_cb(obj, edge_lock_deleted, LV_EVENT_DELETE, lock);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_scroll_chain_hor(obj, false);
	lv_obj_set_scroll_chain_ver(obj, false);
	lv_obj_set_press_lock(obj, true);
}

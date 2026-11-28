/* No-op stand-in for the Liquid Glass background provider (glassbg.c), for
 * tests that link the theme engine or the control centre but not the
 * wallpaper feature itself: the theme asks for a source and a dim, and gets
 * the supplied art at the default dim, the same answers a player with no
 * custom picture gives. The real provider is exercised by test_liquidglass
 * and test_main_menu, which link glassbg.c instead of this file. */
#include <stdlib.h>

#include "src/gui/shell/glassbg.h"

static const uint8_t stub_px[2];
static const lv_image_dsc_t stub_dsc = {
	.header =
		{
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = LV_COLOR_FORMAT_RGB565,
			.w = 1,
			.h = 1,
			.stride = 2,
		},
	.data_size = sizeof(stub_px),
	.data = stub_px,
};

void glassbg_init(void) {}
void glassbg_set_root(const char *card_root) { (void)card_root; }
const lv_image_dsc_t *glassbg_source(void) { return &stub_dsc; }
lv_opa_t glassbg_dim_opa(void) { return (lv_opa_t)(GLASSBG_DIM_DEFAULT * 255 / 100); }
int glassbg_dim_percent(void) { return GLASSBG_DIM_DEFAULT; }
void glassbg_set_dim(int percent) { (void)percent; }
void glassbg_preview_dim(int percent) { (void)percent; }
const char *glassbg_current_name(void) { return ""; }
const char *glassbg_dir(void) { return ""; }
int glassbg_list(char names[][GLASSBG_NAME_MAX + 1], int max) {
	(void)names;
	(void)max;
	return 0;
}
uint8_t *glassbg_decode_file(const char *path, int panel_w, int panel_h) {
	(void)path;
	(void)panel_w;
	(void)panel_h;
	return NULL;
}
void glassbg_install(uint8_t *px, int panel_w, int panel_h, const char *name) {
	(void)panel_w;
	(void)panel_h;
	(void)name;
	free(px);
}
void glassbg_reset(void) {}
void glassbg_sync(void) {}

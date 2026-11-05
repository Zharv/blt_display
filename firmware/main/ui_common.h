/* Small LVGL building blocks shared by the tab screens. */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_screen(void);                                  /* blank screen, app background */
lv_obj_t *ui_card(lv_obj_t *p, int x, int y, int w, int h);
lv_obj_t *ui_label(lv_obj_t *p, const char *txt, int px, lv_color_t c);
lv_obj_t *ui_button(lv_obj_t *p, const char *txt, int w, int h, lv_color_t bg,
                    lv_event_cb_t cb, void *user);
lv_obj_t *ui_list(lv_obj_t *p, int x, int y, int w, int h);
lv_obj_t *ui_list_item(lv_obj_t *list, const char *icon, const char *txt, lv_color_t c,
                       lv_event_cb_t cb, void *user);
lv_obj_t *ui_keyboard(lv_obj_t *scr, lv_keyboard_mode_t mode);  /* hidden, above the tab bar */
lv_obj_t *ui_archive_create(void);

#ifdef __cplusplus
}
#endif

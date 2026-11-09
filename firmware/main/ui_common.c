#include "ui_common.h"
#include "ui_style.h"
#include "fonts.h"
#include "i18n.h"

lv_obj_t *ui_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t *ui_card(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, C_CARD, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 14, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *ui_label(lv_obj_t *p, const char *txt, int px, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font_ua(px), 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

lv_obj_t *ui_button(lv_obj_t *p, const char *txt, int w, int h, lv_color_t bg,
                    lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, 10, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = ui_label(b, txt, 20, C_TXT);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_list(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *l = lv_list_create(p);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_bg_color(l, C_CARD2, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_radius(l, 10, 0);
    lv_obj_set_style_pad_all(l, 6, 0);
    lv_obj_set_style_pad_row(l, 4, 0);
    return l;
}

lv_obj_t *ui_list_item(lv_obj_t *list, const char *icon, const char *txt, lv_color_t c,
                       lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_list_add_button(list, icon, txt);
    lv_obj_set_style_bg_color(b, C_CARD, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_text_color(b, c, 0);
    lv_obj_set_style_text_font(b, font_ua(20), 0);
    lv_obj_set_style_pad_ver(b, 12, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        lv_obj_add_flag(lv_event_get_target(e), LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *ui_keyboard(lv_obj_t *scr, lv_keyboard_mode_t mode)
{
    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_keyboard_set_mode(kb, mode);
    lv_obj_set_size(kb, SCR_W - 2 * PAD, 300);
    /* LVGL 9 creates the keyboard aligned BOTTOM_MID: set_pos would be an
     * offset from that (it ended below the screen) - align it explicitly */
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -(BAR_BOT_H + 8));
    lv_obj_set_style_text_font(kb, font_ua(24), 0);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    return kb;
}

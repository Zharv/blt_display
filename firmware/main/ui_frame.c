#include "ui_frame.h"
#include "ui_style.h"
#include "fonts.h"
#include "i18n.h"

#include <stdio.h>
#include <string.h>

#define MAX_FRAMES 6

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *pill, *pill_lbl;
    lv_obj_t *wifi, *bt, *prn, *clock;
    lv_obj_t *toast;
} frame_t;

static frame_t s_fr[MAX_FRAMES];
static int s_nfr;
static void (*s_nav_cb)(tab_t);
static void (*s_print_cb)(void);
static lv_timer_t *s_toast_t;

static const str_id_t TAB_STR[TAB__COUNT] = {
    S_TAB_ONLINE, S_TAB_TEST, S_TAB_ARCHIVE, S_TAB_BMS, S_TAB_PRINTER, S_TAB_SETTINGS
};
static const char *const TAB_ICON[TAB__COUNT] = {
    LV_SYMBOL_CHARGE, LV_SYMBOL_EDIT, LV_SYMBOL_LIST, LV_SYMBOL_BATTERY_FULL, LV_SYMBOL_FILE, LV_SYMBOL_SETTINGS
};

static lv_color_t ind_color(ind_t i)
{
    return i == IND_OK ? C_GREEN : i == IND_BUSY ? C_AMBER : C_MUTED;
}

static void tab_cb(lv_event_t *e)
{
    tab_t t = (tab_t)(intptr_t)lv_event_get_user_data(e);
    if (s_nav_cb) s_nav_cb(t);
}

static void print_cb(lv_event_t *e)
{
    (void)e;
    if (s_print_cb) s_print_cb();
}

static lv_obj_t *label(lv_obj_t *p, const char *txt, int px, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font_ua(px), 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

static lv_obj_t *plain(lv_obj_t *p, int x, int y, int w, int h, lv_color_t bg)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void ui_frame_attach(lv_obj_t *scr, tab_t tab)
{
    if (s_nfr >= MAX_FRAMES) return;
    frame_t *f = &s_fr[s_nfr++];
    f->scr = scr;

    /* ---- status bar ---- */
    lv_obj_t *top = plain(scr, 0, 0, SCR_W, BAR_TOP_H, C_BAR);
    lv_obj_set_style_border_side(top, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(top, 1, 0);
    lv_obj_set_style_border_color(top, C_LINE, 0);

    char title[64];
    snprintf(title, sizeof title, "BLT-160  ·  %s", tr(TAB_STR[tab]));
    lv_obj_t *t = label(top, title, 24, C_TXT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, PAD, 0);

    f->pill = lv_obj_create(top);
    lv_obj_remove_style_all(f->pill);
    lv_obj_set_size(f->pill, LV_SIZE_CONTENT, 34);
    lv_obj_set_style_radius(f->pill, 17, 0);
    lv_obj_set_style_bg_opa(f->pill, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(f->pill, C_MUTED, 0);
    lv_obj_set_style_pad_hor(f->pill, 16, 0);
    lv_obj_align(f->pill, LV_ALIGN_CENTER, -40, 0);
    f->pill_lbl = label(f->pill, "", 20, C_SUB);
    lv_obj_center(f->pill_lbl);

    /* right side, laid out right-to-left */
    lv_obj_t *lang = label(top, i18n_lang() == LANG_UA ? "UA" : "EN", 16, C_SUB);
    lv_obj_set_style_border_width(lang, 1, 0);
    lv_obj_set_style_border_color(lang, C_LINE, 0);
    lv_obj_set_style_radius(lang, 6, 0);
    lv_obj_set_style_pad_hor(lang, 6, 0);
    lv_obj_set_style_pad_ver(lang, 2, 0);
    lv_obj_align(lang, LV_ALIGN_RIGHT_MID, -PAD, 0);

    f->clock = label(top, "", 20, C_TXT);
    lv_obj_align_to(f->clock, lang, LV_ALIGN_OUT_LEFT_MID, -18, 0);

    f->prn = label(top, LV_SYMBOL_FILE, 20, C_MUTED);
    lv_obj_align(f->prn, LV_ALIGN_RIGHT_MID, -150, 0);

    f->bt = label(top, LV_SYMBOL_BLUETOOTH, 20, C_MUTED);
    lv_obj_set_size(f->bt, 190, 26);
    lv_label_set_long_mode(f->bt, LV_LABEL_LONG_DOT);
    lv_obj_align(f->bt, LV_ALIGN_RIGHT_MID, -190, 0);

    f->wifi = label(top, LV_SYMBOL_WIFI, 20, C_MUTED);
    lv_obj_set_size(f->wifi, 170, 26);
    lv_label_set_long_mode(f->wifi, LV_LABEL_LONG_DOT);
    lv_obj_align(f->wifi, LV_ALIGN_RIGHT_MID, -390, 0);

    /* ---- tab bar ---- */
    lv_obj_t *bot = plain(scr, 0, SCR_H - BAR_BOT_H, SCR_W, BAR_BOT_H, C_BAR);
    lv_obj_set_style_border_side(bot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bot, 1, 0);
    lv_obj_set_style_border_color(bot, C_LINE, 0);

    int x = PAD;
    for (int i = 0; i < TAB__COUNT; i++) {
        lv_obj_t *b = lv_button_create(bot);
        lv_obj_set_size(b, i == TAB_SETTINGS ? 190 : 150, 56);
        lv_obj_set_pos(b, x, 12);
        x += (i == TAB_SETTINGS ? 190 : 150) + 8;
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, i == (int)tab ? C_SEL : C_BAR, 0);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char txt[48];
        snprintf(txt, sizeof txt, "%s  %s", TAB_ICON[i], tr(TAB_STR[i]));
        lv_obj_t *l = label(b, txt, 20, i == (int)tab ? C_TXT : C_SUB);
        lv_obj_center(l);
    }

    lv_obj_t *pb = lv_button_create(bot);
    lv_obj_set_size(pb, 250, 56);
    lv_obj_align(pb, LV_ALIGN_RIGHT_MID, -PAD, 0);
    lv_obj_set_style_shadow_width(pb, 0, 0);
    lv_obj_set_style_radius(pb, 10, 0);
    lv_obj_set_style_bg_color(pb, C_ACCENT, 0);
    lv_obj_add_event_cb(pb, print_cb, LV_EVENT_CLICKED, NULL);
    char ptxt[48];
    snprintf(ptxt, sizeof ptxt, "%s  %s", LV_SYMBOL_FILE, tr(S_PRINT_REPORT));
    lv_obj_t *pl = label(pb, ptxt, 20, lv_color_white());
    lv_obj_center(pl);

    /* toast above the tab bar */
    f->toast = label(scr, "", 20, C_TXT);
    lv_obj_set_style_bg_color(f->toast, C_SEL, 0);
    lv_obj_set_style_bg_opa(f->toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(f->toast, 10, 0);
    lv_obj_set_style_pad_all(f->toast, 12, 0);
    lv_obj_align(f->toast, LV_ALIGN_BOTTOM_RIGHT, -PAD, -(BAR_BOT_H + 10));
    lv_obj_add_flag(f->toast, LV_OBJ_FLAG_HIDDEN);
}

void ui_frame_set_nav_cb(void (*cb)(tab_t)) { s_nav_cb = cb; }
void ui_frame_set_print_cb(void (*cb)(void)) { s_print_cb = cb; }

static void set_if_changed(lv_obj_t *l, const char *txt)
{
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, txt) != 0) lv_label_set_text(l, txt);
}

void ui_frame_update(const frame_status_t *st)
{
    char w[64], b[64];
    snprintf(w, sizeof w, "%s %s", LV_SYMBOL_WIFI, st->wifi_text ? st->wifi_text : "");
    snprintf(b, sizeof b, "%s %s", LV_SYMBOL_BLUETOOTH, st->bt_text ? st->bt_text : "");
    /* colours only when an indicator changes: style setters are expensive */
    static int last[4] = { -1, -1, -1, -1 };
    bool c_state = last[0] != (int)st->state_ind, c_wifi = last[1] != (int)st->wifi;
    bool c_bt = last[2] != (int)st->bt, c_prn = last[3] != (int)st->printer;
    last[0] = st->state_ind; last[1] = st->wifi; last[2] = st->bt; last[3] = st->printer;
    lv_obj_t *active = lv_screen_active();
    for (int i = 0; i < s_nfr; i++) {
        frame_t *f = &s_fr[i];
        if (c_state) {
            lv_obj_set_style_text_color(f->pill_lbl, ind_color(st->state_ind), 0);
            lv_obj_set_style_bg_color(f->pill, ind_color(st->state_ind), 0);
        }
        if (c_wifi) lv_obj_set_style_text_color(f->wifi, ind_color(st->wifi), 0);
        if (c_bt) lv_obj_set_style_text_color(f->bt, ind_color(st->bt), 0);
        if (c_prn) lv_obj_set_style_text_color(f->prn, ind_color(st->printer), 0);
        /* texts: the visible frame now, hidden ones are synced on screen load */
        if (f->scr != active) continue;
        set_if_changed(f->pill_lbl, st->state_text ? st->state_text : "");
        set_if_changed(f->wifi, w);
        set_if_changed(f->bt, b);
        set_if_changed(f->clock, st->clock ? st->clock : "");
    }
}

static void toast_hide(lv_timer_t *t)
{
    (void)t;
    for (int i = 0; i < s_nfr; i++) lv_obj_add_flag(s_fr[i].toast, LV_OBJ_FLAG_HIDDEN);
}

void ui_frame_toast(const char *msg)
{
    for (int i = 0; i < s_nfr; i++) {
        lv_label_set_text(s_fr[i].toast, msg);
        lv_obj_clear_flag(s_fr[i].toast, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_fr[i].toast);
    }
    if (!s_toast_t) {
        s_toast_t = lv_timer_create(toast_hide, 4000, NULL);
        lv_timer_set_repeat_count(s_toast_t, 1);
        lv_timer_set_auto_delete(s_toast_t, false);
    } else {
        lv_timer_reset(s_toast_t);
        lv_timer_set_repeat_count(s_toast_t, 1);
        lv_timer_resume(s_toast_t);
    }
}

#include "ui_wifi.h"
#include "ui_common.h"
#include "ui_style.h"
#include "i18n.h"
#include "fonts.h"
#include "netw.h"
#include "snap.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WIFI_W 832
#define LANG_X (PAD + WIFI_W + 16)
#define LANG_W (SCR_W - LANG_X - PAD)

static lv_obj_t *s_list, *s_sel_lbl, *s_pass_ta, *s_kb, *s_status, *s_poll_sw;
static char s_sel_ssid[33];

static netw_ap_t s_aps[24];
static volatile int s_scan_n = -1;   /* -1 idle, -2 scanning, >=0 ready(count) */

static void scan_task(void *arg)
{
    (void)arg;
    s_scan_n = netw_scan(s_aps, (int)(sizeof s_aps / sizeof s_aps[0]));
    vTaskDelete(NULL);
}

static void ap_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= 24) return;
    strlcpy(s_sel_ssid, s_aps[i].ssid, sizeof s_sel_ssid);
    lv_label_set_text_fmt(s_sel_lbl, "%s: %s", tr(S_NETWORK), s_sel_ssid);
    lv_textarea_set_text(s_pass_ta, "");
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_kb, s_pass_ta);
}

static void scan_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_scan_n == -2) return;
    s_scan_n = -2;
    lv_label_set_text(s_status, tr(S_WIFI_SCANNING));
    lv_obj_set_style_text_color(s_status, C_SUB, 0);
    xTaskCreate(scan_task, "wifiscan", 4096, NULL, 4, NULL);
}

static void connect_click_cb(lv_event_t *e)
{
    (void)e;
    if (!s_sel_ssid[0]) { lv_label_set_text(s_status, tr(S_WIFI_PICK_FIRST)); return; }
    lv_label_set_text_fmt(s_status, tr(S_WIFI_CONNECTING), s_sel_ssid);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    netw_connect(s_sel_ssid, lv_textarea_get_text(s_pass_ta));
}

static void pass_focus_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_kb, s_pass_ta);
}

static void restart_cb(lv_timer_t *t)
{
    (void)t;
    esp_restart();
}

static void lang_cb(lv_event_t *e)
{
    lang_t l = (lang_t)(intptr_t)lv_event_get_user_data(e);
    if (l == i18n_lang()) return;
    i18n_save(l);
    lv_timer_t *t = lv_timer_create(restart_cb, 300, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void poll_sw_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    blt_live_poll_enable(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void chem_cb(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    if (id <= 2) health_chem_set((uint8_t)id);
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    /* keep the switch in step with the polling state (it can also be set from the console) */
    if (s_poll_sw && lv_obj_has_state(s_poll_sw, LV_STATE_CHECKED) != blt_live_poll_enabled()) {
        if (blt_live_poll_enabled()) lv_obj_add_state(s_poll_sw, LV_STATE_CHECKED);
        else lv_obj_clear_state(s_poll_sw, LV_STATE_CHECKED);
    }
    if (s_scan_n >= 0) {
        int n = s_scan_n;
        s_scan_n = -1;
        lv_obj_clean(s_list);
        for (int i = 0; i < n; i++) {
            char line[64];
            snprintf(line, sizeof line, "%.32s   %s", s_aps[i].ssid, s_aps[i].secure ? LV_SYMBOL_EYE_CLOSE : "");
            ui_list_item(s_list, LV_SYMBOL_WIFI, line, C_TXT, ap_click_cb, (void *)(intptr_t)i);
        }
        lv_label_set_text_fmt(s_status, tr(S_WIFI_FOUND), n);
    }
    char ip[20];
    netw_get_ip_str(ip, sizeof ip);
    if (ip[0]) {
        char ssid[33];
        netw_get_ssid(ssid, sizeof ssid);
        lv_label_set_text_fmt(s_status, LV_SYMBOL_OK " %s   %s", ssid, ip);
        lv_obj_set_style_text_color(s_status, C_GREEN, 0);
    }
}

lv_obj_t *ui_wifi_create(void)
{
    lv_obj_t *scr = ui_screen();

    /* ---- Wi-Fi card ---- */
    lv_obj_t *c = ui_card(scr, PAD, CONTENT_Y, WIFI_W, CONTENT_H);
    lv_obj_t *h = ui_label(c, tr(S_SET_WIFI), 24, C_TXT);
    lv_obj_set_pos(h, 16, 14);
    char st[32];
    snprintf(st, sizeof st, "%s  %s", LV_SYMBOL_REFRESH, tr(S_SCAN));
    lv_obj_t *scan = ui_button(c, st, 170, 48, C_ACCENT, scan_click_cb, NULL);
    lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, -16, 10);
    s_status = ui_label(c, tr(S_WIFI_HINT), 20, C_SUB);
    lv_obj_set_pos(s_status, 16, 66);

    s_list = ui_list(c, 16, 104, 420, CONTENT_H - 120);

    s_sel_lbl = ui_label(c, tr(S_NETWORK_NONE), 24, C_TXT);
    lv_obj_set_pos(s_sel_lbl, 456, 110);
    lv_obj_set_width(s_sel_lbl, WIFI_W - 456 - 16);
    lv_label_set_long_mode(s_sel_lbl, LV_LABEL_LONG_DOT);

    s_pass_ta = lv_textarea_create(c);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_password_mode(s_pass_ta, true);
    lv_textarea_set_placeholder_text(s_pass_ta, tr(S_PASSWORD));
    lv_obj_set_style_text_font(s_pass_ta, lv_obj_get_style_text_font(s_sel_lbl, 0), 0);
    lv_obj_set_size(s_pass_ta, WIFI_W - 456 - 16, 60);
    lv_obj_set_pos(s_pass_ta, 456, 156);
    lv_obj_add_event_cb(s_pass_ta, pass_focus_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *conn = ui_button(c, tr(S_CONNECT), 220, 56, lv_color_hex(0x1f7a52), connect_click_cb, NULL);
    lv_obj_set_pos(conn, 456, 232);

    /* ---- language card ---- */
    lv_obj_t *lc = ui_card(scr, LANG_X, CONTENT_Y, LANG_W, 300);
    lv_obj_t *lh = ui_label(lc, tr(S_SET_LANGUAGE), 24, C_TXT);
    lv_obj_set_pos(lh, 16, 14);
    static const char *const names[2] = { "Українська", "English" };
    for (int i = 0; i < 2; i++) {
        bool on = (i == (int)i18n_lang());
        lv_obj_t *b = ui_button(lc, names[i], LANG_W - 32, 64, on ? C_SEL : lv_color_hex(0x0f1630),
                                lang_cb, (void *)(intptr_t)i);
        lv_obj_set_pos(b, 16, 64 + i * 76);
        if (on) {
            lv_obj_set_style_border_width(b, 2, 0);
            lv_obj_set_style_border_color(b, C_ACCENT, 0);
        }
    }
    lv_obj_t *note = ui_label(lc, tr(S_SET_LANG_NOTE), 16, C_SUB);
    lv_obj_set_pos(note, 16, 222);

    /* ---- BLT tester card ---- */
    lv_obj_t *tc = ui_card(scr, LANG_X, CONTENT_Y + 316, LANG_W, 332);
    lv_obj_t *th = ui_label(tc, tr(S_SET_TESTER), 24, C_TXT);
    lv_obj_set_pos(th, 16, 14);
    lv_obj_t *tl = ui_label(tc, tr(S_SET_TESTER_POLL), 20, C_TXT);
    lv_obj_set_pos(tl, 16, 70);
    lv_obj_t *sw = s_poll_sw = lv_switch_create(tc);
    lv_obj_set_size(sw, 72, 38);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -16, 62);
    lv_obj_set_style_bg_color(sw, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (blt_live_poll_enabled()) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, poll_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *tn = ui_label(tc, tr(S_SET_TESTER_NOTE), 16, C_SUB);
    lv_obj_set_pos(tn, 16, 120);
    lv_obj_set_width(tn, LANG_W - 32);
    lv_label_set_long_mode(tn, LV_LABEL_LONG_WRAP);

    /* battery chemistry: limits of the battery-status card */
    lv_obj_t *cl = ui_label(tc, tr(S_SET_CHEM), 20, C_TXT);
    lv_obj_set_pos(cl, 16, 214);
    static const char *chem_map[4];
    chem_map[0] = tr(S_CHEM_AUTO); chem_map[1] = "NMC"; chem_map[2] = "LFP"; chem_map[3] = "";
    lv_obj_t *bm = lv_buttonmatrix_create(tc);
    lv_buttonmatrix_set_map(bm, chem_map);
    lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(bm, true);
    lv_buttonmatrix_set_button_ctrl(bm, health_chem_get() <= 2 ? health_chem_get() : 0, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(bm, 216, 52);
    lv_obj_align(bm, LV_ALIGN_TOP_RIGHT, -16, 202);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 4, 0);
    lv_obj_set_style_text_font(bm, font_ua(20), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, C_CARD2, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, C_TXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, C_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(bm, chem_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *ch = ui_label(tc, tr(S_SET_CHEM_HINT), 16, C_SUB);
    lv_obj_set_pos(ch, 16, 266);
    lv_obj_set_width(ch, LANG_W - 32);
    lv_label_set_long_mode(ch, LV_LABEL_LONG_WRAP);

    s_kb = ui_keyboard(scr, LV_KEYBOARD_MODE_TEXT_LOWER);

    lv_timer_create(poll_cb, 500, NULL);
    return scr;
}

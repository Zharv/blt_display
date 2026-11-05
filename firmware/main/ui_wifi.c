#include "ui_wifi.h"
#include "netw.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define COL_BG    lv_color_hex(0x0b1020)
#define COL_CARD  lv_color_hex(0x161d33)
#define COL_TXT   lv_color_hex(0xf0f3ff)
#define COL_SUB   lv_color_hex(0x9aa4c2)
#define COL_BLUE  lv_color_hex(0x2f6df6)
#define COL_GREEN lv_color_hex(0x59e6a0)

static lv_obj_t *s_screen;
static lv_obj_t *s_list;
static lv_obj_t *s_sel_lbl;
static lv_obj_t *s_pass_ta;
static lv_obj_t *s_kb;
static lv_obj_t *s_status;
static void (*s_back_cb)(void);

static char s_sel_ssid[33];

/* scan handoff: -1 idle, -2 scanning, >=0 ready(count) */
static netw_ap_t s_aps[24];
static volatile int s_scan_n = -1;

static void scan_task(void *arg)
{
    (void)arg;
    int n = netw_scan(s_aps, (int)(sizeof s_aps / sizeof s_aps[0]));
    s_scan_n = n;
    vTaskDelete(NULL);
}

static void ap_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= 24) return;
    strlcpy(s_sel_ssid, s_aps[i].ssid, sizeof s_sel_ssid);
    lv_label_set_text_fmt(s_sel_lbl, "Network: %s", s_sel_ssid);
    lv_textarea_set_text(s_pass_ta, "");
    lv_obj_clear_flag(s_pass_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_kb, s_pass_ta);
}

static void scan_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_scan_n == -2) return;
    s_scan_n = -2;
    lv_label_set_text(s_status, "scanning...");
    xTaskCreate(scan_task, "wifiscan", 4096, NULL, 4, NULL);
}

static void connect_click_cb(lv_event_t *e)
{
    (void)e;
    if (!s_sel_ssid[0]) { lv_label_set_text(s_status, "pick a network first"); return; }
    const char *pass = lv_textarea_get_text(s_pass_ta);
    lv_label_set_text_fmt(s_status, "connecting to %s...", s_sel_ssid);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    netw_connect(s_sel_ssid, pass);
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb) s_back_cb();
}

/* periodic: fill scan results (in LVGL ctx) + refresh connection status */
static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_scan_n >= 0) {
        int n = s_scan_n;
        s_scan_n = -1;
        lv_obj_clean(s_list);
        for (int i = 0; i < n; i++) {
            lv_obj_t *b = lv_list_add_button(s_list, LV_SYMBOL_WIFI, s_aps[i].ssid);
            lv_obj_set_style_bg_color(b, COL_CARD, 0);
            lv_obj_set_style_text_color(b, COL_TXT, 0);
            lv_obj_add_event_cb(b, ap_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        lv_label_set_text_fmt(s_status, "found %d networks", n);
    }
    char ip[20];
    netw_get_ip_str(ip, sizeof ip);
    if (ip[0]) {
        char ssid[33];
        netw_get_ssid(ssid, sizeof ssid);
        lv_label_set_text_fmt(s_status, LV_SYMBOL_OK " %s  %s", ssid, ip);
        lv_obj_set_style_text_color(s_status, COL_GREEN, 0);
    }
}

lv_obj_t *ui_wifi_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "Wi-Fi SETTINGS");
    lv_obj_set_style_text_color(title, COL_TXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 14);

    lv_obj_t *back = lv_button_create(s_screen);
    lv_obj_set_size(back, 120, 44);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -20, 8);
    lv_obj_set_style_bg_color(back, COL_CARD, 0);
    lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " BACK");
    lv_obj_center(bl);

    lv_obj_t *scan = lv_button_create(s_screen);
    lv_obj_set_size(scan, 150, 44);
    lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, -160, 8);
    lv_obj_set_style_bg_color(scan, COL_BLUE, 0);
    lv_obj_add_event_cb(scan, scan_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(scan);
    lv_label_set_text(sl, LV_SYMBOL_REFRESH " SCAN");
    lv_obj_center(sl);

    s_status = lv_label_create(s_screen);
    lv_label_set_text(s_status, "tap SCAN to list networks");
    lv_obj_set_style_text_color(s_status, COL_SUB, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_18, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 20, 56);

    /* AP list (left) */
    s_list = lv_list_create(s_screen);
    lv_obj_set_size(s_list, 560, 400);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 16, 92);
    lv_obj_set_style_bg_color(s_list, COL_CARD, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);

    /* right panel: selected + password + connect */
    s_sel_lbl = lv_label_create(s_screen);
    lv_label_set_text(s_sel_lbl, "Network: (none)");
    lv_obj_set_style_text_color(s_sel_lbl, COL_TXT, 0);
    lv_obj_set_style_text_font(s_sel_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_sel_lbl, LV_ALIGN_TOP_LEFT, 600, 100);

    s_pass_ta = lv_textarea_create(s_screen);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_password_mode(s_pass_ta, true);
    lv_textarea_set_placeholder_text(s_pass_ta, "password");
    lv_obj_set_size(s_pass_ta, 560, 56);
    lv_obj_align(s_pass_ta, LV_ALIGN_TOP_LEFT, 600, 140);
    lv_obj_add_flag(s_pass_ta, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *conn = lv_button_create(s_screen);
    lv_obj_set_size(conn, 200, 56);
    lv_obj_align(conn, LV_ALIGN_TOP_LEFT, 600, 210);
    lv_obj_set_style_bg_color(conn, COL_GREEN, 0);
    lv_obj_add_event_cb(conn, connect_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(conn);
    lv_label_set_text(cl, "CONNECT");
    lv_obj_set_style_text_color(cl, lv_color_hex(0x07210f), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_20, 0);
    lv_obj_center(cl);

    /* on-screen keyboard (bottom), hidden until a network is picked */
    s_kb = lv_keyboard_create(s_screen);
    lv_obj_set_size(s_kb, LV_HOR_RES, 300);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(poll_cb, 500, NULL);
    return s_screen;
}

void ui_wifi_set_back_cb(void (*cb)(void)) { s_back_cb = cb; }

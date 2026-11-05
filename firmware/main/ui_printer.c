#include "ui_printer.h"
#include "ui_common.h"
#include "ui_style.h"
#include "i18n.h"
#include "netw.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LIST_W 832
#define IP_X   (PAD + LIST_W + 16)
#define IP_W   (SCR_W - IP_X - PAD)

static lv_obj_t *s_list, *s_status, *s_ip_ta, *s_kb;
static netw_printer_t s_printers[16];
static volatile int s_scan_n = -1;

static void save_and_report(const netw_printer_t *p)
{
    netw_set_printer(p);
    char buf[128];
    snprintf(buf, sizeof buf, tr(S_PRN_SAVED), p->name[0] ? p->name : p->ip, p->ip, (unsigned)p->port);
    lv_label_set_text_fmt(s_status, LV_SYMBOL_OK " %s", buf);
    lv_obj_set_style_text_color(s_status, C_GREEN, 0);
}

static void scan_task(void *arg)
{
    (void)arg;
    s_scan_n = netw_scan_printers(s_printers, (int)(sizeof s_printers / sizeof s_printers[0]));
    vTaskDelete(NULL);
}

static void printer_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < 16) save_and_report(&s_printers[i]);
}

static void scan_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_scan_n == -2) return;
    s_scan_n = -2;
    lv_label_set_text(s_status, tr(S_PRN_SEARCHING));
    lv_obj_set_style_text_color(s_status, C_SUB, 0);
    xTaskCreate(scan_task, "ippscan", 4096, NULL, 4, NULL);
}

static void ip_focus_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_kb, s_ip_ta);
}

static void use_ip_cb(lv_event_t *e)
{
    (void)e;
    const char *ip = lv_textarea_get_text(s_ip_ta);
    if (!ip || !ip[0]) { lv_label_set_text(s_status, tr(S_PRN_ENTER_IP)); return; }
    netw_printer_t p = { 0 };
    strlcpy(p.ip, ip, sizeof p.ip);
    strlcpy(p.resource, "/ipp/print", sizeof p.resource);
    strlcpy(p.name, "manual", sizeof p.name);
    p.port = 631;
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    save_and_report(&p);
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    static bool direct_shown;
    char ssid[33];
    netw_get_ssid(ssid, sizeof ssid);
    bool direct = netw_is_connected() && strncmp(ssid, "DIRECT-", 7) == 0;
    if (direct && !direct_shown && s_scan_n == -1) {
        lv_label_set_text_fmt(s_status, LV_SYMBOL_OK " Wi-Fi Direct:  %.32s", ssid);
        lv_obj_set_style_text_color(s_status, C_GREEN, 0);
    }
    direct_shown = direct;
    if (s_scan_n < 0) return;
    int n = s_scan_n;
    s_scan_n = -1;
    lv_obj_clean(s_list);
    for (int i = 0; i < n; i++) {
        char line[160];
        snprintf(line, sizeof line, "%.100s   (%.15s)", s_printers[i].name, s_printers[i].ip);
        ui_list_item(s_list, LV_SYMBOL_FILE, line, C_TXT, printer_click_cb, (void *)(intptr_t)i);
    }
    if (n == 0) lv_label_set_text(s_status, tr(S_PRN_NONE));
    else lv_label_set_text_fmt(s_status, tr(S_PRN_FOUND), n);
}

lv_obj_t *ui_printer_create(void)
{
    lv_obj_t *scr = ui_screen();

    lv_obj_t *c = ui_card(scr, PAD, CONTENT_Y, LIST_W, CONTENT_H);
    lv_obj_t *h = ui_label(c, "AirPrint / IPP", 24, C_TXT);
    lv_obj_set_pos(h, 16, 14);
    char st[32];
    snprintf(st, sizeof st, "%s  %s", LV_SYMBOL_REFRESH, tr(S_PRN_SEARCH));
    lv_obj_t *scan = ui_button(c, st, 170, 48, C_ACCENT, scan_click_cb, NULL);
    lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, -16, 10);
    s_status = ui_label(c, tr(S_PRN_HINT), 20, C_SUB);
    lv_obj_set_pos(s_status, 16, 66);
    lv_obj_set_width(s_status, LIST_W - 32);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    s_list = ui_list(c, 16, 104, LIST_W - 32, CONTENT_H - 120);

    lv_obj_t *ic = ui_card(scr, IP_X, CONTENT_Y, IP_W, CONTENT_H);
    lv_obj_t *ih = ui_label(ic, tr(S_PRN_MANUAL), 24, C_TXT);
    lv_obj_set_pos(ih, 16, 14);
    s_ip_ta = lv_textarea_create(ic);
    lv_textarea_set_one_line(s_ip_ta, true);
    lv_textarea_set_placeholder_text(s_ip_ta, "192.168.0.101");
    lv_obj_set_style_text_font(s_ip_ta, lv_obj_get_style_text_font(ih, 0), 0);
    lv_obj_set_size(s_ip_ta, IP_W - 32, 60);
    lv_obj_set_pos(s_ip_ta, 16, 60);
    lv_obj_add_event_cb(s_ip_ta, ip_focus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *use = ui_button(ic, tr(S_PRN_USE_IP), IP_W - 32, 56, lv_color_hex(0x1f7a52), use_ip_cb, NULL);
    lv_obj_set_pos(use, 16, 136);
    lv_obj_t *hint = ui_label(ic, tr(S_PRN_DIRECT), 16, C_SUB);
    lv_obj_set_pos(hint, 16, 210);
    lv_obj_set_width(hint, IP_W - 32);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    s_kb = ui_keyboard(scr, LV_KEYBOARD_MODE_NUMBER);

    netw_printer_t saved;
    if (netw_get_printer(&saved)) {
        lv_textarea_set_text(s_ip_ta, saved.ip);
        lv_label_set_text_fmt(s_status, tr(S_PRN_CURRENT), saved.name[0] ? saved.name : saved.ip, saved.ip);
    }

    lv_timer_create(poll_cb, 500, NULL);
    return scr;
}

#include "ui_printer.h"
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
static lv_obj_t *s_status;
static void (*s_back_cb)(void);

static netw_printer_t s_printers[16];
static volatile int s_scan_n = -1;   /* -1 idle, -2 scanning, >=0 ready(count) */

static void scan_task(void *arg)
{
    (void)arg;
    int n = netw_scan_printers(s_printers, (int)(sizeof s_printers / sizeof s_printers[0]));
    s_scan_n = n;
    vTaskDelete(NULL);
}

static void printer_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= 16) return;
    netw_set_printer(&s_printers[i]);
    lv_label_set_text_fmt(s_status, LV_SYMBOL_OK " saved: %s  (%s:%u)",
                          s_printers[i].name, s_printers[i].ip, (unsigned)s_printers[i].port);
    lv_obj_set_style_text_color(s_status, COL_GREEN, 0);
}

static void scan_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_scan_n == -2) return;
    s_scan_n = -2;
    lv_label_set_text(s_status, "searching for printers...");
    lv_obj_set_style_text_color(s_status, COL_SUB, 0);
    xTaskCreate(scan_task, "ippscan", 4096, NULL, 4, NULL);
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb) s_back_cb();
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_scan_n < 0) return;
    int n = s_scan_n;
    s_scan_n = -1;
    lv_obj_clean(s_list);
    for (int i = 0; i < n; i++) {
        char line[160];
        snprintf(line, sizeof line, "%.100s  (%.15s)", s_printers[i].name, s_printers[i].ip);
        lv_obj_t *b = lv_list_add_button(s_list, LV_SYMBOL_SAVE, line);
        lv_obj_set_style_bg_color(b, COL_CARD, 0);
        lv_obj_set_style_text_color(b, COL_TXT, 0);
        lv_obj_add_event_cb(b, printer_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (n == 0) {
        lv_label_set_text(s_status, "no IPP printers found - check it's on the same network");
    } else {
        lv_label_set_text_fmt(s_status, "found %d printer(s) - tap to select", n);
    }
}

lv_obj_t *ui_printer_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "PRINTER");
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
    lv_obj_set_size(scan, 170, 44);
    lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, -160, 8);
    lv_obj_set_style_bg_color(scan, COL_BLUE, 0);
    lv_obj_add_event_cb(scan, scan_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(scan);
    lv_label_set_text(sl, LV_SYMBOL_REFRESH " SEARCH");
    lv_obj_center(sl);

    s_status = lv_label_create(s_screen);
    lv_label_set_text(s_status, "tap SEARCH to find IPP/AirPrint printers");
    lv_obj_set_style_text_color(s_status, COL_SUB, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_18, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 20, 58);

    s_list = lv_list_create(s_screen);
    lv_obj_set_size(s_list, LV_HOR_RES - 32, LV_VER_RES - 120);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 16, 100);
    lv_obj_set_style_bg_color(s_list, COL_CARD, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);

    /* preselect: show the saved printer if any */
    netw_printer_t saved;
    if (netw_get_printer(&saved)) {
        lv_label_set_text_fmt(s_status, "current: %s (%s) - SEARCH to change",
                              saved.name[0] ? saved.name : saved.ip, saved.ip);
    }

    lv_timer_create(poll_cb, 500, NULL);
    return s_screen;
}

void ui_printer_set_back_cb(void (*cb)(void)) { s_back_cb = cb; }

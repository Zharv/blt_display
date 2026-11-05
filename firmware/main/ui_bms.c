#include "ui_bms.h"
#include "bms_ble.h"

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
#define COL_RED   lv_color_hex(0xff6b6b)

#define MAX_DEVS 20

static lv_obj_t *s_list;
static lv_obj_t *s_status;
static lv_obj_t *s_live;
static void (*s_back_cb)(void);

static bms_ble_dev_t s_devs[MAX_DEVS];
static volatile int s_scan_n = -1;   /* -1 idle, -2 scanning, >=0 ready(count) */

static void scan_task(void *arg)
{
    (void)arg;
    s_scan_n = bms_ble_scan(s_devs, MAX_DEVS, 6000);
    vTaskDelete(NULL);
}

static void scan_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_scan_n == -2) return;
    s_scan_n = -2;
    lv_obj_clean(s_list);
    lv_label_set_text(s_status, "scanning for 6 s...");
    xTaskCreate(scan_task, "blescan", 4096, NULL, 4, NULL);
}

static void dev_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= MAX_DEVS) return;
    if (bms_ble_connect(&s_devs[i]) == ESP_OK)
        lv_label_set_text_fmt(s_status, "connecting to %.30s...", s_devs[i].name);
    else
        lv_label_set_text(s_status, "Bluetooth not ready yet");
}

static void disc_click_cb(lv_event_t *e)
{
    (void)e;
    bms_ble_disconnect();
    lv_label_set_text(s_status, "disconnected, BMS forgotten");
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb) s_back_cb();
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_scan_n >= 0) {
        int n = s_scan_n;
        s_scan_n = -1;
        lv_obj_clean(s_list);
        for (int i = 0; i < n; i++) {
            char line[80];
            snprintf(line, sizeof line, "%s%.30s   %d dBm", s_devs[i].is_jk ? "[JK]  " : "",
                     s_devs[i].name[0] ? s_devs[i].name : "(no name)", s_devs[i].rssi);
            lv_obj_t *b = lv_list_add_button(s_list, LV_SYMBOL_BLUETOOTH, line);
            lv_obj_set_style_bg_color(b, COL_CARD, 0);
            lv_obj_set_style_text_color(b, s_devs[i].is_jk ? COL_GREEN : COL_TXT, 0);
            lv_obj_add_event_cb(b, dev_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        lv_label_set_text_fmt(s_status, n ? "found %d - tap your BMS" : "nothing found - is the BMS awake?", n);
    }

    char name[32];
    bms_ble_state_t st = bms_ble_state(name, sizeof name);
    bms_data_t d;
    uint32_t age = 0;
    if (st == BMS_BLE_STREAMING && bms_ble_get(&d, &age)) {
        lv_label_set_text_fmt(s_live,
            LV_SYMBOL_OK " %.24s   %.2f V   %.2f A   SOC %u%%   %u cells   d %u mV   (%lu frames)",
            name, d.pack_mv / 1000.0, d.pack_ma / 1000.0, d.soc_pct, d.cell_count,
            d.imbalance_mv, (unsigned long)bms_ble_frames());
        lv_obj_set_style_text_color(s_live, COL_GREEN, 0);
    } else {
        lv_label_set_text_fmt(s_live, "%s%s%.24s", bms_ble_state_str(st),
                              name[0] ? "  -  " : "", name);
        lv_obj_set_style_text_color(s_live, st == BMS_BLE_ERROR ? COL_RED : COL_SUB, 0);
    }
}

lv_obj_t *ui_bms_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "BMS (Bluetooth)");
    lv_obj_set_style_text_color(title, COL_TXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 14);

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 120, 44);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -20, 8);
    lv_obj_set_style_bg_color(back, COL_CARD, 0);
    lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " BACK");
    lv_obj_center(bl);

    lv_obj_t *scan = lv_button_create(scr);
    lv_obj_set_size(scan, 150, 44);
    lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, -160, 8);
    lv_obj_set_style_bg_color(scan, COL_BLUE, 0);
    lv_obj_add_event_cb(scan, scan_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(scan);
    lv_label_set_text(sl, LV_SYMBOL_REFRESH " SCAN");
    lv_obj_center(sl);

    lv_obj_t *disc = lv_button_create(scr);
    lv_obj_set_size(disc, 190, 44);
    lv_obj_align(disc, LV_ALIGN_TOP_RIGHT, -330, 8);
    lv_obj_set_style_bg_color(disc, COL_CARD, 0);
    lv_obj_add_event_cb(disc, disc_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(disc);
    lv_label_set_text(dl, LV_SYMBOL_CLOSE " DISCONNECT");
    lv_obj_center(dl);

    s_status = lv_label_create(scr);
    lv_label_set_text(s_status, "tap SCAN, then your BMS (JK shown first)");
    lv_obj_set_style_text_color(s_status, COL_SUB, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_18, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 20, 58);

    s_live = lv_label_create(scr);
    lv_label_set_text(s_live, "");
    lv_obj_set_style_text_font(s_live, &lv_font_montserrat_20, 0);
    lv_obj_align(s_live, LV_ALIGN_TOP_LEFT, 20, 88);

    s_list = lv_list_create(scr);
    lv_obj_set_size(s_list, LV_HOR_RES - 32, LV_VER_RES - 140);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 16, 124);
    lv_obj_set_style_bg_color(s_list, COL_CARD, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);

    lv_timer_create(poll_cb, 500, NULL);
    return scr;
}

void ui_bms_set_back_cb(void (*cb)(void)) { s_back_cb = cb; }

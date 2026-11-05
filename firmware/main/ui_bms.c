#include "ui_bms.h"
#include "ui_common.h"
#include "ui_style.h"
#include "i18n.h"
#include "bms_ble.h"
#include "esp_log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_DEVS 20

static lv_obj_t *s_list, *s_status, *s_live;
static bms_ble_dev_t s_devs[MAX_DEVS];
static volatile int s_scan_n = -1;

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
    lv_label_set_text(s_status, tr(S_BMS_SCANNING));
    xTaskCreate(scan_task, "blescan", 4096, NULL, 4, NULL);
}

static void dev_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= MAX_DEVS) return;
    if (bms_ble_connect(&s_devs[i]) == ESP_OK)
        lv_label_set_text_fmt(s_status, tr(S_BMS_CONNECTING), s_devs[i].name);
    else
        lv_label_set_text(s_status, tr(S_BMS_NOT_READY));
}

static void disc_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI("ui_bms", "disconnect tapped");
    bms_ble_disconnect();
    lv_label_set_text(s_status, tr(S_BMS_FORGOT));
}

static str_id_t state_str(bms_ble_state_t s)
{
    switch (s) {
    case BMS_BLE_OFF:        return S_BLE_OFF;
    case BMS_BLE_IDLE:       return S_BLE_IDLE;
    case BMS_BLE_SCANNING:   return S_BLE_SCANNING;
    case BMS_BLE_CONNECTING: return S_BLE_CONNECTING;
    case BMS_BLE_CONNECTED:  return S_BLE_CONNECTED;
    case BMS_BLE_STREAMING:  return S_BLE_STREAMING;
    default:                 return S_BLE_ERROR;
    }
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
            snprintf(line, sizeof line, "%s%.30s      %d dBm", s_devs[i].is_jk ? "[JK]  " : "",
                     s_devs[i].name[0] ? s_devs[i].name : "?", s_devs[i].rssi);
            ui_list_item(s_list, LV_SYMBOL_BLUETOOTH, line, s_devs[i].is_jk ? C_GREEN : C_TXT,
                         dev_click_cb, (void *)(intptr_t)i);
        }
        if (n) lv_label_set_text_fmt(s_status, tr(S_BMS_FOUND), n);
        else lv_label_set_text(s_status, tr(S_BMS_NOTHING));
    }

    char name[32];
    bms_ble_state_t st = bms_ble_state(name, sizeof name);
    bms_data_t d;
    uint32_t age = 0;
    if (st == BMS_BLE_STREAMING && bms_ble_get(&d, &age)) {
        lv_label_set_text_fmt(s_live,
            LV_SYMBOL_OK "  %.24s    %.2f %s    %.1f %s    SOC %u%%    %uS    Δ %u %s",
            name, d.pack_mv / 1000.0, tr(S_U_V), d.pack_ma / 1000.0, tr(S_U_A), d.soc_pct,
            d.cell_count, d.imbalance_mv, tr(S_U_MV));
        lv_obj_set_style_text_color(s_live, C_GREEN, 0);
    } else {
        lv_label_set_text_fmt(s_live, "%s%s%.24s", tr(state_str(st)), name[0] ? "  —  " : "", name);
        lv_obj_set_style_text_color(s_live, st == BMS_BLE_ERROR ? C_RED : C_SUB, 0);
    }
}

lv_obj_t *ui_bms_create(void)
{
    lv_obj_t *scr = ui_screen();
    lv_obj_t *c = ui_card(scr, PAD, CONTENT_Y, SCR_W - 2 * PAD, CONTENT_H);

    lv_obj_t *h = ui_label(c, tr(S_BMS_TITLE), 24, C_TXT);
    lv_obj_set_pos(h, 16, 14);

    char st[32];
    snprintf(st, sizeof st, "%s  %s", LV_SYMBOL_REFRESH, tr(S_SCAN));
    lv_obj_t *scan = ui_button(c, st, 170, 48, C_ACCENT, scan_click_cb, NULL);
    lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, -16, 10);
    char dt[48];
    snprintf(dt, sizeof dt, "%s  %s", LV_SYMBOL_CLOSE, tr(S_DISCONNECT));
    lv_obj_t *disc = ui_button(c, dt, 220, 48, lv_color_hex(0x0f1630), disc_click_cb, NULL);
    lv_obj_align(disc, LV_ALIGN_TOP_RIGHT, -200, 10);

    s_live = ui_label(c, "", 24, C_SUB);
    lv_obj_set_pos(s_live, 16, 70);
    s_status = ui_label(c, tr(S_BMS_HINT), 20, C_SUB);
    lv_obj_set_pos(s_status, 16, 110);

    s_list = ui_list(c, 16, 148, SCR_W - 2 * PAD - 32, CONTENT_H - 164);

    lv_timer_create(poll_cb, 500, NULL);
    return scr;
}

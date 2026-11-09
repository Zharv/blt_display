/*
 * "Архів" tab: the tests stored in the tester's recorder that have a
 * discharge result (empty / aborted records are skipped), newest first.
 * Tap one to see it; print its test protocol. Read-only (RI/RD).
 */
#include "ui_common.h"
#include "ui_style.h"
#include "ui_frame.h"
#include "i18n.h"
#include "fonts.h"
#include "snap.h"
#include "blt_usb.h"
#include "blt_archive.h"
#include "test_sum.h"
#include "sn_fix.h"
#include "sd_arch.h"
#include "sd_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psram_task.h"

#define LIST_W   600
#define ARCH_MAX 150
#define VIEW_MAX 600

static lv_obj_t *s_scr, *s_status, *s_list, *s_refresh;
static lv_obj_t *s_d_title, *s_d_hint, *s_d_rows, *s_d_print, *s_d_edit, *s_d_ta, *s_kb;

static blt_arch_entry_t *s_cache;      /* tester records (PSRAM, ARCH_MAX), kept between refreshes */
static int s_cache_n;
static blt_arch_entry_t *s_sd;         /* tests on the SD card (PSRAM, VIEW_MAX) */
static blt_arch_entry_t *s_view;       /* what the list shows: both merged, newest first */
static int s_view_n, s_sd_n;
static volatile bool s_tester_ok;      /* the last refresh reached the tester */
static int s_sel = -1;

/* worker state (written by the task, read by the LVGL timer) */
static volatile bool s_busy;
static volatile int s_prog_done, s_prog_total;
static volatile int s_result;           /* 0 none, 1 ok, -1 error */
static int64_t s_loaded_us;

static void progress(int done, int total) { s_prog_done = done; s_prog_total = total; }

static bool tester_ready(void)
{
    return blt_usb_reads_allowed() && blt_usb_state(NULL, NULL) == BLT_USB_CONNECTED;
}

static void list_once(void);
static TaskHandle_t s_list_worker;

static void list_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        list_once();
    }
}

static void list_once(void)
{
    esp_err_t e = ESP_OK;
    s_tester_ok = false;
    if (tester_ready()) {
        e = blt_archive_list(s_cache, &s_cache_n, ARCH_MAX, progress);
        s_tester_ok = e == ESP_OK;
    }
    s_sd_n = sd_arch_list(s_sd, VIEW_MAX);
    /* merge: tester records (newest first) + tests only on the card */
    int n = 0;
    if (s_tester_ok)
        for (int k = 0; k < s_cache_n && n < VIEW_MAX; k++) {
            s_view[n] = s_cache[k];
            s_view[n].on_sd = false;
            for (int j = 0; j < s_sd_n; j++)
                if (s_sd[j].n == s_cache[k].n) { s_view[n].on_sd = true; break; }
            n++;
        }
    int base = n;
    for (int j = 0; j < s_sd_n && n < VIEW_MAX; j++) {
        bool dup = false;
        for (int k = 0; k < base; k++) if (s_view[k].n == s_sd[j].n) { dup = true; break; }
        if (!dup) s_view[n++] = s_sd[j];
    }
    /* newest first overall (cycle index grows with time) */
    for (int a = 1; a < n; a++) {
        blt_arch_entry_t x = s_view[a];
        int b = a - 1;
        while (b >= 0 && s_view[b].n < x.n) { s_view[b + 1] = s_view[b]; b--; }
        s_view[b + 1] = x;
    }
    s_view_n = n;
    s_result = (e == ESP_OK || n > 0) ? 1 : -1;
    s_busy = false;
}

/* with_sync: also copy new finished tests to the card (not when the refresh
 * itself follows a copy - that looped: refresh -> copy -> refresh -> ...) */
static void start_refresh(bool with_sync)
{
    if (s_busy) return;
    if (!tester_ready() && !sd_store_ready()) {
        lv_label_set_text(s_status, tr(S_AR_NO_TESTER));
        return;
    }
    if (with_sync && tester_ready()) sd_arch_request_sync();
    s_busy = true;
    s_result = 0;
    s_prog_done = 0;
    s_prog_total = 0;
    lv_label_set_text(s_status, tr(S_AR_READING_START));
    if (!s_list_worker && psram_task(list_task, "blt_list", 6144, NULL, 4, &s_list_worker, tskNO_AFFINITY) != pdPASS) {
        s_busy = false;
        return;
    }
    xTaskNotifyGive(s_list_worker);
}

static void refresh_cb(lv_event_t *e) { (void)e; start_refresh(true); }

static void screen_cb(lv_event_t *e)
{
    (void)e;
    /* opening the tab refreshes a list older than a minute */
    if (!s_busy && (s_loaded_us == 0 || esp_timer_get_time() - s_loaded_us > 60 * 1000000LL)) start_refresh(true);
}

static int alarms_of(const blt_rec_t *r, int *first)
{
    blt_rec_t a = *r;
    a.mem_alarms[3] &= (uint8_t)~(1u << 3);     /* 28: voltage threshold, not a fault */
    *first = blt_rec_alarm_first(&a);
    return blt_rec_alarm_count(&a);
}

static bool running(const blt_arch_entry_t *e) { return e->current && e->rec.state == 1; }   /* discharging */

static void add_row(const char *label, const char *value)
{
    lv_obj_t *r = lv_obj_create(s_d_rows);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), 34);
    lv_obj_t *l = ui_label(r, label, 20, C_SUB);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = ui_label(r, value, 20, C_TXT);
    lv_obj_set_width(v, lv_obj_get_width(s_d_rows) - 250);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_align(v, LV_ALIGN_LEFT_MID, 250, 0);
}

static void show_detail(void)
{
    lv_obj_clean(s_d_rows);
    if (s_sel < 0 || s_sel >= s_view_n) {
        lv_obj_add_flag(s_d_print, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_d_edit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_d_hint, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_d_title, "");
        return;
    }
    lv_obj_add_flag(s_d_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_d_print, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_d_edit, LV_OBJ_FLAG_HIDDEN);
    const blt_arch_entry_t *e = &s_view[s_sel];
    const blt_rec_t *r = &e->rec;
    char v[96];
    lv_label_set_text_fmt(s_d_title, "%s %u", tr(S_AR_TEST), r->pr_test);

    char fix[20];
    if (sn_fix_get(e->n, fix, sizeof fix)) {
        snprintf(v, sizeof v, "%s  (%s %s)", fix, tr(S_AR_SN_TESTER), r->battery_id[0] ? r->battery_id : "—");
        add_row(tr(S_AR_SN), v);
    } else {
        add_row(tr(S_AR_SN), r->battery_id[0] ? r->battery_id : "—");
    }
    snprintf(v, sizeof v, "%s", tr(running(e) ? S_AR_RUNNING : S_AR_DONE));
    add_row(tr(S_AR_STATE), v);
    snprintf(v, sizeof v, "%02u.%02u.20%02u %02u:%02u", r->dis_d, r->dis_mo, r->dis_y, r->dis_h, r->dis_mi);
    add_row(tr(S_AR_START), v);
    snprintf(v, sizeof v, "%u:%02u", r->t_dis_min / 60, r->t_dis_min % 60);
    add_row(tr(S_AR_TIME), v);
    if (r->rated_ah) snprintf(v, sizeof v, "%u %s (%.1f %%)", r->ah_dis, tr(S_U_AH), r->ah_dis * 100.0f / r->rated_ah);
    else snprintf(v, sizeof v, "%u %s", r->ah_dis, tr(S_U_AH));
    add_row(tr(S_AR_AH), v);
    snprintf(v, sizeof v, "%u %s · %u %s", r->rated_v, tr(S_U_V), r->rated_ah, tr(S_U_AH));
    add_row(tr(S_AR_RATED), v);
    snprintf(v, sizeof v, "%u %s · %s %.1f %s", r->discharge_a, tr(S_U_A), tr(S_AR_TO), blt_rec_stop_v(r), tr(S_U_V));
    add_row(tr(S_AR_CURR), v);
    int a1, na = alarms_of(r, &a1);
    if (na) snprintf(v, sizeof v, "%d · %s", na, blt_alarm_name_ua(a1));
    else snprintf(v, sizeof v, "%s", tr(S_AR_NONE));
    add_row(tr(S_AR_ALARMS), v);
    add_row(tr(S_AR_WHERE), tr(e->on_sd && e->on_tester ? S_AR_WHERE_BOTH : e->on_sd ? S_AR_WHERE_SD : S_AR_WHERE_TESTER));
    test_sum_t s;
    if ((test_sum_get(r->pr_test, &s) || sd_arch_get_sum(r->pr_cycle, &s)) && s.cells) {
        snprintf(v, sizeof v, "%.16s · %uS · Δ %u→%u %s", s.bms, s.cells, s.delta0_mv, s.delta_max_mv, tr(S_U_MV));
        add_row("BMS", v);
    } else {
        add_row("BMS", tr(S_AR_NO_BMS));
    }
}

static void item_cb(lv_event_t *e)
{
    s_sel = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t n = lv_obj_get_child_count(s_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_get_child(s_list, (int32_t)i);
        lv_obj_set_style_bg_color(b, (int)i == s_sel ? C_SEL : C_CARD, 0);
    }
    show_detail();
}

static void rebuild_list(void);

static void sn_edit_cb(lv_event_t *e)
{
    (void)e;
    if (s_sel < 0 || s_sel >= s_view_n) return;
    const blt_arch_entry_t *a = &s_view[s_sel];
    char buf[20];
    lv_textarea_set_text(s_d_ta, sn_fix_apply(a->n, a->rec.battery_id, buf, sizeof buf));
    lv_obj_clear_flag(s_d_ta, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_kb, s_d_ta);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static void sn_done_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_add_flag(s_d_ta, LV_OBJ_FLAG_HIDDEN);
    if (code != LV_EVENT_READY || s_sel < 0 || s_sel >= s_view_n) return;
    const blt_arch_entry_t *a = &s_view[s_sel];
    const char *t = lv_textarea_get_text(s_d_ta);
    /* the tester's own serial again = no correction */
    sn_fix_set(a->n, strcmp(t, a->rec.battery_id) == 0 ? "" : t);
    rebuild_list();
}

static void detailed_cb(lv_event_t *e)
{
    app_protocol_set_detailed(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void print_cb(lv_event_t *e)
{
    (void)e;
    if (s_sel < 0 || s_sel >= s_view_n) return;
    const blt_arch_entry_t *a = &s_view[s_sel];
    app_print_protocol(a->current ? BLT_ARCH_CURRENT : a->n);
}

static void rebuild_list(void)
{
    lv_obj_clean(s_list);
    uint16_t keep = s_sel >= 0 && s_sel < s_view_n ? s_view[s_sel].n : 0xFFFF;
    s_sel = -1;
    for (int i = 0; i < s_view_n; i++) {
        const blt_arch_entry_t *e = &s_view[i];
        const blt_rec_t *r = &e->rec;
        char t[160];
        char fixb[20];
        const char *sn = sn_fix_apply(e->n, r->battery_id[0] ? r->battery_id : "—", fixb, sizeof fixb);
        snprintf(t, sizeof t, "%02u.%02u.20%02u %02u:%02u    %s\n%u %s  ·  %u:%02u  ·  %s %u%s%s",
                 r->dis_d, r->dis_mo, r->dis_y, r->dis_h, r->dis_mi, sn,
                 r->ah_dis, tr(S_U_AH), r->t_dis_min / 60, r->t_dis_min % 60, tr(S_AR_TEST_SHORT), r->pr_test,
                 running(e) ? "  ·  " : "", running(e) ? tr(S_AR_RUNNING) : "");
        lv_obj_t *b = ui_list_item(s_list, e->on_sd ? LV_SYMBOL_SD_CARD : LV_SYMBOL_FILE, t, running(e) ? C_GREEN : C_TXT, item_cb, (void *)(intptr_t)i);
        lv_obj_set_style_text_font(b, font_ua(20), 0);
        if (e->n == keep) s_sel = i;
    }
    if (s_sel >= 0) lv_obj_set_style_bg_color(lv_obj_get_child(s_list, s_sel), C_SEL, 0);
    show_detail();
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_busy) {
        if (s_prog_total) lv_label_set_text_fmt(s_status, tr(S_AR_READING), s_prog_done, s_prog_total);
        return;
    }
    static bool was_syncing;
    int sd_done, sd_total;
    bool syncing = sd_arch_sync_busy(&sd_done, &sd_total);
    if (syncing && sd_total) {
        lv_label_set_text_fmt(s_status, tr(S_AR_SYNCING), sd_done, sd_total);
    } else if (was_syncing && !syncing && sd_total > 0 && lv_screen_active() == s_scr) {
        start_refresh(false);               /* show the SD marks of what was just copied */
    }
    was_syncing = syncing;
    if (s_result == 1) {
        s_result = 0;
        s_loaded_us = esp_timer_get_time();
        int on_sd = 0;
        for (int k = 0; k < s_view_n; k++) on_sd += s_view[k].on_sd;
        lv_label_set_text_fmt(s_status, tr(s_tester_ok ? S_AR_COUNT : S_AR_COUNT_SD), s_view_n, on_sd);
        rebuild_list();
    } else if (s_result == -1) {
        s_result = 0;
        lv_label_set_text(s_status, tr(S_AR_FAIL));
    }
}

lv_obj_t *ui_archive_create(void)
{
    s_cache = heap_caps_calloc(ARCH_MAX, sizeof *s_cache, MALLOC_CAP_SPIRAM);
    s_sd = heap_caps_calloc(VIEW_MAX, sizeof *s_sd, MALLOC_CAP_SPIRAM);
    s_view = heap_caps_calloc(VIEW_MAX, sizeof *s_view, MALLOC_CAP_SPIRAM);
    s_scr = ui_screen();
    lv_obj_add_event_cb(s_scr, screen_cb, LV_EVENT_SCREEN_LOADED, NULL);

    /* list */
    lv_obj_t *c = ui_card(s_scr, PAD, CONTENT_Y, LIST_W, CONTENT_H);
    lv_obj_t *h = ui_label(c, tr(S_AR_TITLE), 24, C_TXT);
    lv_obj_set_pos(h, 16, 16);
    s_refresh = ui_button(c, LV_SYMBOL_REFRESH, 64, 48, C_CARD2, refresh_cb, NULL);
    lv_obj_align(s_refresh, LV_ALIGN_TOP_RIGHT, -12, 8);
    s_status = ui_label(c, "", 16, C_SUB);
    lv_obj_set_pos(s_status, 16, 52);
    s_list = ui_list(c, 10, 82, LIST_W - 20, CONTENT_H - 92);

    /* detail */
    int dx = PAD + LIST_W + 16, dw = SCR_W - dx - PAD;
    lv_obj_t *d = ui_card(s_scr, dx, CONTENT_Y, dw, CONTENT_H);
    s_d_title = ui_label(d, "", 24, C_TXT);
    lv_obj_set_pos(s_d_title, 20, 16);
    s_d_hint = ui_label(d, tr(S_AR_PICK), 20, C_SUB);
    lv_obj_align(s_d_hint, LV_ALIGN_CENTER, 0, -20);
    s_d_edit = ui_button(d, tr(S_AR_SN_EDIT), 200, 44, C_CARD2, sn_edit_cb, NULL);
    lv_obj_align(s_d_edit, LV_ALIGN_TOP_RIGHT, -16, 10);
    lv_obj_add_flag(s_d_edit, LV_OBJ_FLAG_HIDDEN);
    s_d_ta = lv_textarea_create(d);
    lv_textarea_set_one_line(s_d_ta, true);
    lv_textarea_set_max_length(s_d_ta, 16);
    lv_textarea_set_accepted_chars(s_d_ta, "0123456789");
    lv_obj_set_style_text_font(s_d_ta, font_ua(24), 0);
    lv_obj_set_size(s_d_ta, 300, 52);
    lv_obj_align(s_d_ta, LV_ALIGN_TOP_RIGHT, -228, 6);
    lv_obj_add_flag(s_d_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_d_ta, sn_done_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_d_ta, sn_done_cb, LV_EVENT_CANCEL, NULL);

    s_d_rows = lv_obj_create(d);
    lv_obj_remove_style_all(s_d_rows);
    lv_obj_set_pos(s_d_rows, 20, 60);
    lv_obj_set_size(s_d_rows, dw - 40, 10 * 34 + 8);
    lv_obj_set_flex_flow(s_d_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *note = ui_label(d, tr(S_AR_GRAPH_NOTE), 16, C_MUTED);
    lv_obj_set_width(note, dw - 40);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 20, -140);
    lv_obj_t *cb = lv_checkbox_create(d);
    lv_checkbox_set_text(cb, tr(S_AR_DETAILED));
    lv_obj_set_style_text_font(cb, font_ua(20), 0);
    lv_obj_set_style_text_color(cb, C_TXT, 0);
    lv_obj_set_style_bg_color(cb, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (app_protocol_detailed()) lv_obj_add_state(cb, LV_STATE_CHECKED);
    lv_obj_add_event_cb(cb, detailed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(cb, LV_ALIGN_BOTTOM_LEFT, 20, -88);
    s_d_print = ui_button(d, "", dw - 40, 60, C_ACCENT, print_cb, NULL);
    lv_label_set_text_fmt(lv_obj_get_child(s_d_print, 0), "%s  %s", LV_SYMBOL_FILE, tr(S_AR_PRINT));
    lv_obj_align(s_d_print, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_flag(s_d_print, LV_OBJ_FLAG_HIDDEN);

    s_kb = ui_keyboard(s_scr, LV_KEYBOARD_MODE_NUMBER);

    lv_timer_create(poll_cb, 300, NULL);
    return s_scr;
}

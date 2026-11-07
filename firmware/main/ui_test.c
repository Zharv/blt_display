/*
 * "Тест" tab: prepare the BLT tester for the next battery - battery serial
 * (YYYYMMDDNNNN, next number suggested from the tester's own clock) and the
 * planned capacity (rated Ah). Written only while the tester is in START.
 */
#include "ui_test.h"
#include "ui_common.h"
#include "ui_style.h"
#include "ui_frame.h"
#include "i18n.h"
#include "blt_prepare.h"
#include "blt_usb.h"
#include "blt_live.h"
#include "blt_blocks.h"
#include "snap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern bool blt_live_latest(blt_live_t *out, uint32_t *age_ms);

static lv_obj_t *s_scr, *s_state, *s_now, *s_sn, *s_ah, *s_write, *s_result, *s_kb, *s_room, *s_room_st;

/* worker results, applied by the LVGL timer */
static volatile int s_read_done;            /* 1 = values below are fresh */
static char s_cur_sn[20];
static uint16_t s_cur_ah;
static volatile bool s_busy;
static volatile int s_write_done;           /* 1 ok, -1 failed */
static char s_msg[96];
static char s_req_sn[20];
static uint16_t s_req_ah;

static void read_task(void *arg)
{
    (void)arg;
    char sn[20] = "";
    uint16_t ah = 0;
    if (blt_prepare_read(sn, sizeof sn, &ah) == ESP_OK) {
        strlcpy(s_cur_sn, sn, sizeof s_cur_sn);
        s_cur_ah = ah;
        s_read_done = 1;
    }
    s_busy = false;
    vTaskDelete(NULL);
}

static void write_task(void *arg)
{
    (void)arg;
    esp_err_t e = blt_prepare_write(s_req_sn, s_req_ah, s_msg, sizeof s_msg);
    s_write_done = (e == ESP_OK) ? 1 : -1;
    if (e == ESP_OK) {                      /* show what the tester holds now */
        char sn[20] = "";
        uint16_t ah = 0;
        if (blt_prepare_read(sn, sizeof sn, &ah) == ESP_OK) {
            strlcpy(s_cur_sn, sn, sizeof s_cur_sn);
            s_cur_ah = ah;
            s_read_done = 1;
        }
    }
    s_busy = false;
    vTaskDelete(NULL);
}

static void suggest_serial(const blt_live_t *l)
{
    char nx[20];
    blt_next_serial(nx, sizeof nx, 2000 + l->rtc_y, l->rtc_mo, l->rtc_d, s_cur_sn[0] ? s_cur_sn : l->battery_id);
    lv_textarea_set_text(s_sn, nx);
}

static void write_cb(lv_event_t *e)
{
    (void)e;
    if (s_busy) return;
    const char *sn = lv_textarea_get_text(s_sn);
    int ah = atoi(lv_textarea_get_text(s_ah));
    if (ah <= 0 || ah > 9999) { lv_label_set_text(s_result, tr(S_PREP_BAD_AH)); return; }
    strlcpy(s_req_sn, sn, sizeof s_req_sn);
    s_req_ah = (uint16_t)ah;
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_result, tr(S_PREP_WRITING));
    lv_obj_set_style_text_color(s_result, C_SUB, 0);
    s_busy = true;
    xTaskCreate(write_task, "blt_prep", 6144, NULL, 4, NULL);
}

static void new_sn_cb(lv_event_t *e)
{
    (void)e;
    blt_live_t l;
    uint32_t age;
    if (blt_live_latest(&l, &age)) suggest_serial(&l);
}

/* room temperature: kept on the panel and given to the running / next test */
static void room_ready_cb(lv_event_t *e)
{
    (void)e;
    const char *t = lv_textarea_get_text(s_room);
    if (!t[0]) return;
    int v = atoi(t);
    if (v < -30 || v > 60) { lv_label_set_text(s_room_st, tr(S_PREP_ROOM_BAD)); return; }
    room_temp_set((float)v);
    lv_textarea_set_text(s_room, "");
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_kb, lv_event_get_target(e));
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    blt_live_t l;
    uint32_t age = 0;
    bool live = blt_live_latest(&l, &age) && age < 5000;
    bool start = live && l.state == BLT_ST_START;

    if (!live) lv_label_set_text(s_state, tr(S_NO_TESTER));
    else lv_label_set_text_fmt(s_state, "%s:  %s", tr(S_PREP_STATE), blt_state_name_it(l.state));
    lv_obj_set_style_text_color(s_state, start ? C_GREEN : C_AMBER, 0);

    /* read the tester's current values once it is reachable (and after writes) */
    static bool asked;
    if (live && !asked && !s_busy && lv_screen_active() == s_scr) {
        asked = true;
        s_busy = true;
        xTaskCreate(read_task, "blt_prep_rd", 6144, NULL, 4, NULL);
    }
    if (!live) asked = false;

    if (s_read_done) {
        s_read_done = 0;
        lv_label_set_text_fmt(s_now, "%s:  SN %s   ·   %u %s", tr(S_PREP_NOW),
                              s_cur_sn[0] ? s_cur_sn : "—", s_cur_ah, tr(S_U_AH));
        if (!lv_textarea_get_text(s_ah)[0]) lv_textarea_set_text(s_ah, "");
        if (!lv_textarea_get_text(s_ah)[0] && s_cur_ah) {
            char b[8];
            snprintf(b, sizeof b, "%u", s_cur_ah);
            lv_textarea_set_text(s_ah, b);
        }
        if (!lv_textarea_get_text(s_sn)[0] && live) suggest_serial(&l);
    }
    if (s_write_done) {
        lv_label_set_text(s_result, s_msg);
        lv_obj_set_style_text_color(s_result, s_write_done > 0 ? C_GREEN : C_RED, 0);
        ui_frame_toast(s_msg);
        s_write_done = 0;
    }

    float rt;
    if (room_temp_pending(&rt)) lv_label_set_text_fmt(s_room_st, tr(S_PREP_ROOM_PENDING), (int)lroundf(rt));
    else lv_label_set_text(s_room_st, tr(S_PREP_ROOM_NONE));

    bool can = start && !s_busy;
    if (can) lv_obj_clear_state(s_write, LV_STATE_DISABLED);
    else lv_obj_add_state(s_write, LV_STATE_DISABLED);
}

static lv_obj_t *field(lv_obj_t *p, int y, str_id_t cap, int max_len)
{
    lv_obj_t *l = ui_label(p, tr(cap), 20, C_SUB);
    lv_obj_set_pos(l, 16, y);
    lv_obj_t *ta = lv_textarea_create(p);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_accepted_chars(ta, "0123456789");
    lv_obj_set_style_text_font(ta, lv_obj_get_style_text_font(l, 0), 0);
    lv_obj_set_size(ta, 360, 56);
    lv_obj_set_pos(ta, 16, y + 30);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

lv_obj_t *ui_test_create(void)
{
    s_scr = ui_screen();
    lv_obj_t *c = ui_card(s_scr, PAD, CONTENT_Y, SCR_W - 2 * PAD, CONTENT_H);

    lv_obj_t *h = ui_label(c, tr(S_PREP_TITLE), 24, C_TXT);
    lv_obj_set_pos(h, 16, 14);
    s_state = ui_label(c, "", 20, C_SUB);
    lv_obj_set_pos(s_state, 16, 56);
    s_now = ui_label(c, "", 20, C_SUB);
    lv_obj_set_pos(s_now, 16, 88);

    s_sn = field(c, 136, S_PREP_SERIAL, 16);
    lv_obj_t *nb = ui_button(c, tr(S_PREP_NEXT_SN), 200, 56, lv_color_hex(0x0f1630), new_sn_cb, NULL);
    lv_obj_set_pos(nb, 392, 166);
    s_ah = field(c, 236, S_PREP_AH, 4);

    s_write = ui_button(c, tr(S_PREP_WRITE), 360, 64, lv_color_hex(0x1f7a52), write_cb, NULL);
    lv_obj_set_pos(s_write, 16, 344);
    lv_obj_set_style_bg_color(s_write, C_LINE, LV_STATE_DISABLED);
    s_result = ui_label(c, "", 20, C_SUB);
    lv_obj_set_pos(s_result, 16, 424);

    /* room temperature (no BMS: the protocol judges the cold from it) */
    lv_obj_t *rl = ui_label(c, tr(S_PREP_ROOM), 20, C_SUB);
    lv_obj_set_pos(rl, 660, 136);
    s_room = lv_textarea_create(c);
    lv_textarea_set_one_line(s_room, true);
    lv_textarea_set_max_length(s_room, 3);
    lv_textarea_set_accepted_chars(s_room, "-0123456789");
    lv_obj_set_style_text_font(s_room, lv_obj_get_style_text_font(rl, 0), 0);
    lv_obj_set_size(s_room, 200, 56);
    lv_obj_set_pos(s_room, 660, 166);
    lv_obj_add_event_cb(s_room, ta_focus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_room, room_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_t *rb = ui_button(c, tr(S_PREP_ROOM_SET), 200, 56, lv_color_hex(0x0f1630), room_ready_cb, NULL);
    lv_obj_set_pos(rb, 876, 166);
    s_room_st = ui_label(c, "", 20, C_TXT);
    lv_obj_set_pos(s_room_st, 660, 236);
    lv_obj_t *rh = ui_label(c, tr(S_PREP_ROOM_HINT), 16, C_SUB);
    lv_obj_set_pos(rh, 660, 274);
    lv_obj_set_width(rh, 560);
    lv_label_set_long_mode(rh, LV_LABEL_LONG_WRAP);

    lv_obj_t *note = ui_label(c, tr(S_PREP_NOTE), 16, C_SUB);
    lv_obj_set_pos(note, 16, 470);
    lv_obj_set_width(note, 560);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

    s_kb = ui_keyboard(s_scr, LV_KEYBOARD_MODE_NUMBER);
    lv_timer_create(poll_cb, 1000, NULL);
    return s_scr;
}

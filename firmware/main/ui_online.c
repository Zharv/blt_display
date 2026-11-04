#include "ui.h"
#include "lvgl.h"

#include <stdio.h>

/* palette */
#define COL_BG     lv_color_hex(0x0b1020)
#define COL_CARD   lv_color_hex(0x161d33)
#define COL_TXT    lv_color_hex(0xf0f3ff)
#define COL_SUB    lv_color_hex(0x9aa4c2)
#define COL_BLUE   lv_color_hex(0x2f6df6)
#define COL_GREEN  lv_color_hex(0x59e6a0)
#define COL_RED    lv_color_hex(0xff6b6b)
#define COL_AMBER  lv_color_hex(0xffcc66)

#define CHART_POINTS 120
#define V_MIN_MV 40000
#define V_MAX_MV 55000
#define I_MAX_MA 20000

static lv_obj_t *s_lbl_v, *s_lbl_a, *s_lbl_ah, *s_lbl_time, *s_lbl_temp, *s_lbl_state;
static lv_obj_t *s_lbl_soc, *s_lbl_packv, *s_lbl_imb, *s_lbl_btemp;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_ser_v, *s_ser_i;
static lv_obj_t *s_cell_bar[UI_MAX_CELLS];
static lv_obj_t *s_cell_lbl[UI_MAX_CELLS];
static int s_cell_shown;

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h, const char *title)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    if (title) {
        lv_obj_t *t = lv_label_create(c);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, COL_SUB, 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    return c;
}

/* caption (small, grey) + value (bold font). Returns the value label. */
static lv_obj_t *make_stat(lv_obj_t *card, const char *cap, int x, int y, const lv_font_t *vf)
{
    lv_obj_t *l = lv_label_create(card);
    lv_label_set_text(l, cap);
    lv_obj_set_style_text_color(l, COL_SUB, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(l, x, y);

    lv_obj_t *v = lv_label_create(card);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_color(v, COL_TXT, 0);
    lv_obj_set_style_text_font(v, vf, 0);
    lv_obj_set_pos(v, x, y + 20);
    return v;
}

void ui_online_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* header */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "BLT  \xE2\x80\x94  ONLINE");
    lv_obj_set_style_text_color(title, COL_TXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 14);

    s_lbl_state = lv_label_create(scr);
    lv_label_set_text(s_lbl_state, "--");
    lv_obj_set_style_text_color(s_lbl_state, COL_GREEN, 0);
    lv_obj_set_style_text_font(s_lbl_state, &lv_font_montserrat_22, 0);
    lv_obj_align(s_lbl_state, LV_ALIGN_TOP_RIGHT, -20, 16);

    /* tester card (left) */
    lv_obj_t *ct = make_card(scr, 16, 56, 612, 300, "TESTER");
    s_lbl_v = make_stat(ct, "VOLTAGE (V)", 0, 40, &lv_font_montserrat_48);
    lv_obj_set_style_text_color(s_lbl_v, COL_GREEN, 0);
    s_lbl_a   = make_stat(ct, "CURRENT (A)", 320, 40, &lv_font_montserrat_48);
    s_lbl_ah  = make_stat(ct, "CAPACITY (Ah)", 0, 150, &lv_font_montserrat_30);
    s_lbl_time = make_stat(ct, "TIME", 220, 150, &lv_font_montserrat_30);
    s_lbl_temp = make_stat(ct, "TEMP (\xC2\xB0""C)", 420, 150, &lv_font_montserrat_30);

    /* BMS card (right) */
    lv_obj_t *cb = make_card(scr, 652, 56, 612, 300, "BMS");
    s_lbl_soc   = make_stat(cb, "SOC (%)", 0, 40, &lv_font_montserrat_48);
    lv_obj_set_style_text_color(s_lbl_soc, COL_BLUE, 0);
    s_lbl_packv = make_stat(cb, "PACK (V)", 220, 40, &lv_font_montserrat_48);
    s_lbl_imb   = make_stat(cb, "IMBALANCE (mV)", 0, 150, &lv_font_montserrat_30);
    s_lbl_btemp = make_stat(cb, "TEMP (\xC2\xB0""C)", 300, 150, &lv_font_montserrat_30);

    /* cell bar strip inside BMS card */
    lv_obj_t *strip = lv_obj_create(cb);
    lv_obj_set_size(strip, 580, 70);
    lv_obj_set_pos(strip, 0, 205);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < UI_MAX_CELLS; i++) {
        lv_obj_t *b = lv_bar_create(strip);
        lv_obj_set_size(b, 18, 56);
        lv_bar_set_range(b, 0, 100);
        lv_bar_set_value(b, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x0b1020), LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, COL_GREEN, LV_PART_INDICATOR);
        lv_obj_set_style_radius(b, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(b, 3, LV_PART_INDICATOR);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        s_cell_bar[i] = b;
        s_cell_lbl[i] = NULL;
    }

    /* discharge chart (bottom, full width): voltage (primary) + current (secondary) */
    lv_obj_t *cc = make_card(scr, 16, 372, 1248, 412, "DISCHARGE");
    s_chart = lv_chart_create(cc);
    lv_obj_set_size(s_chart, 1210, 330);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(s_chart, 5, 8);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_line_width(s_chart, 3, LV_PART_ITEMS);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, V_MIN_MV, V_MAX_MV);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_SECONDARY_Y, 0, I_MAX_MA);
    s_ser_v = lv_chart_add_series(s_chart, COL_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    s_ser_i = lv_chart_add_series(s_chart, COL_BLUE, LV_CHART_AXIS_SECONDARY_Y);
}

static void update_cells(const online_sample_t *s)
{
    int n = s->cell_count;
    if (n > UI_MAX_CELLS) n = UI_MAX_CELLS;
    s_cell_shown = n;
    int lo = s->cell_min_mv ? s->cell_min_mv - 30 : 3000;
    int hi = s->cell_max_mv ? s->cell_max_mv + 30 : 3600;
    if (hi <= lo) hi = lo + 1;
    for (int i = 0; i < UI_MAX_CELLS; i++) {
        if (i < n) {
            int pct = (int)((long)(s->cell_mv[i] - lo) * 100 / (hi - lo));
            if (pct < 2) pct = 2;
            if (pct > 100) pct = 100;
            lv_bar_set_value(s_cell_bar[i], pct, LV_ANIM_OFF);
            lv_color_t c = COL_GREEN;
            if (s->imbalance_mv >= 30) {
                if (s->cell_mv[i] == s->cell_max_mv) c = COL_RED;
                else if (s->cell_mv[i] == s->cell_min_mv) c = COL_BLUE;
            }
            lv_obj_set_style_bg_color(s_cell_bar[i], c, LV_PART_INDICATOR);
            lv_obj_clear_flag(s_cell_bar[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_cell_bar[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_online_update(const online_sample_t *s)
{
    if (!s) return;

    lv_label_set_text_fmt(s_lbl_v, "%.2f", s->voltage_v);
    lv_label_set_text_fmt(s_lbl_a, "%.1f", s->current_a);
    lv_label_set_text_fmt(s_lbl_ah, "%.2f", s->capacity_ah);
    lv_label_set_text_fmt(s_lbl_time, "%02u:%02u",
                          (unsigned)(s->elapsed_s / 60), (unsigned)(s->elapsed_s % 60));
    lv_label_set_text_fmt(s_lbl_temp, "%.1f", s->temp_c);
    lv_label_set_text(s_lbl_state, s->state ? s->state : "--");

    if (s->bms_present) {
        lv_label_set_text_fmt(s_lbl_soc, "%u", s->soc_pct);
        lv_label_set_text_fmt(s_lbl_packv, "%.2f", s->bms_pack_v);
        lv_label_set_text_fmt(s_lbl_imb, "%u", s->imbalance_mv);
        lv_label_set_text_fmt(s_lbl_btemp, "%.1f", s->bms_temp_c);
        lv_obj_set_style_text_color(s_lbl_imb, s->imbalance_mv >= 50 ? COL_AMBER : COL_TXT, 0);
        update_cells(s);
    } else {
        lv_label_set_text(s_lbl_soc, "--");
        lv_label_set_text(s_lbl_packv, "--");
    }

    lv_chart_set_next_value(s_chart, s_ser_v, (int32_t)(s->voltage_v * 1000.0f));
    lv_chart_set_next_value(s_chart, s_ser_i, (int32_t)(s->current_a < 0 ? -s->current_a * 1000.0f
                                                                         : s->current_a * 1000.0f));
}

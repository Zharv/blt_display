#include "ui.h"
#include "ui_style.h"
#include "fonts.h"
#include "i18n.h"
#include "hist.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHART_PTS   120
#define CHART_X     68
#define CHART_Y     66
#define CHART_W     (848 - 2 * CHART_X)
#define CHART_H     (384 - CHART_Y - 46)

/* layout */
#define COL_L_X  PAD
#define COL_L_W  384
#define COL_R_X  (COL_L_X + COL_L_W + 16)
#define COL_R_W  (SCR_W - COL_R_X - PAD)
#define TEST_H   312
#define BMS_Y    (CONTENT_Y + TEST_H + 12)
#define BMS_H    (CONTENT_Y + CONTENT_H - BMS_Y)
#define CHART_CARD_H 384
#define CELLS_Y  (CONTENT_Y + CHART_CARD_H + 12)
#define CELLS_H  (CONTENT_Y + CONTENT_H - CELLS_Y)

typedef enum { MODE_DISCHARGE = 0, MODE_IMBALANCE } chart_mode_t;

/* tester */
static lv_obj_t *s_t_tag, *s_t_v, *s_t_a, *s_t_ah, *s_t_wh, *s_t_time, *s_t_temp;
/* bms */
static lv_obj_t *s_b_title, *s_b_tag, *s_b_soc, *s_b_v, *s_b_a, *s_b_delta, *s_b_minmax, *s_b_temps;
/* chart */
static lv_obj_t *s_chart, *s_seg[2], *s_leg[4], *s_sc_l, *s_sc_r, *s_sc_b;
static lv_chart_series_t *s_ser_ut, *s_ser_ub, *s_ser_i, *s_ser_d, *s_ser_c[UI_MAX_CELLS];
static chart_mode_t s_mode;
/* cells */
static lv_obj_t *s_cells_hdr, *s_cell[UI_MAX_CELLS], *s_cell_no[UI_MAX_CELLS],
                *s_cell_bar[UI_MAX_CELLS], *s_cell_v[UI_MAX_CELLS];
static int s_cells_laid;

/* ------------------------------------------------------------- helpers */

static lv_obj_t *mk_label(lv_obj_t *p, const char *t, int px, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, font_ua(px), 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

static lv_obj_t *mk_box(lv_obj_t *p, int x, int y, int w, int h, lv_color_t bg, int radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* value + unit on a common bottom line; returns the value label */
static lv_obj_t *mk_value(lv_obj_t *p, int x, int y, int px, lv_color_t c, const char *unit, int upx)
{
    lv_obj_t *row = lv_obj_create(p);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, x, y);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_t *v = mk_label(row, "--", px, c);
    if (unit) {
        lv_obj_t *u = mk_label(row, unit, upx, C_SUB);
        lv_obj_set_style_pad_bottom(u, px >= 40 ? 6 : 3, 0);
    }
    return v;
}

static lv_obj_t *mk_stat(lv_obj_t *p, int x, int y, str_id_t cap)
{
    mk_label(p, tr(cap), 16, C_SUB);
    lv_obj_t *cl = lv_obj_get_child(p, -1);
    lv_obj_set_pos(cl, x, y);
    lv_obj_t *v = mk_label(p, "--", 24, C_TXT);
    lv_obj_set_pos(v, x, y + 22);
    return v;
}

static void set_tag(lv_obj_t *tag, const char *txt, lv_color_t c)
{
    lv_label_set_text(tag, txt);
    lv_obj_set_style_text_color(tag, c, 0);
}

/* ------------------------------------------------------------- scales */

static lv_obj_t *mk_scale(lv_obj_t *p, lv_scale_mode_t mode, int x, int y, int w, int h)
{
    lv_obj_t *s = lv_scale_create(p);
    lv_scale_set_mode(s, mode);
    lv_obj_set_pos(s, x, y);
    lv_obj_set_size(s, w, h);
    lv_scale_set_label_show(s, true);
    lv_obj_set_style_text_font(s, font_ua(16), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s, C_SUB, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_line_color(s, C_LINE, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s, C_LINE, LV_PART_ITEMS);
    lv_obj_set_style_length(s, 5, LV_PART_INDICATOR);
    lv_obj_set_style_length(s, 0, LV_PART_ITEMS);
    lv_obj_set_style_line_width(s, 0, LV_PART_MAIN);
    return s;
}

static void scale_range(lv_obj_t *s, int lo, int hi, int ticks)
{
    if (hi <= lo) hi = lo + 1;
    lv_scale_set_range(s, lo, hi);
    lv_scale_set_total_tick_count(s, ticks);
    lv_scale_set_major_tick_every(s, 1);
}

/* round a span to a "nice" step so at most ~5 intervals are shown */
static int nice_step(int span)
{
    static const int st[] = { 1, 2, 5, 10, 20, 25, 50, 100, 200, 500, 1000, 2000, 5000 };
    for (size_t i = 0; i < sizeof st / sizeof st[0]; i++)
        if (span / st[i] <= 5) return st[i];
    return 10000;
}

/* ------------------------------------------------------------- chart */

static void legend(int i, const char *txt, lv_color_t c)
{
    if (!txt) { lv_obj_add_flag(s_leg[i], LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(s_leg[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_leg[i], "%s %s", LV_SYMBOL_MINUS, txt);
    lv_obj_set_style_text_color(s_leg[i], c, 0);
}

static void show_mode_series(void)
{
    bool d = (s_mode == MODE_DISCHARGE);
    lv_chart_hide_series(s_chart, s_ser_ut, !d);
    lv_chart_hide_series(s_chart, s_ser_ub, !d);
    lv_chart_hide_series(s_chart, s_ser_i, !d);
    lv_chart_hide_series(s_chart, s_ser_d, d);
    for (int c = 0; c < UI_MAX_CELLS; c++) lv_chart_hide_series(s_chart, s_ser_c[c], true);
    for (int i = 0; i < 2; i++) {
        bool on = (i == (int)s_mode);
        lv_obj_set_style_bg_color(s_seg[i], on ? C_SEL : C_CARD2, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_seg[i], 0), on ? C_TXT : C_SUB, 0);
    }
}

static void seg_cb(lv_event_t *e)
{
    ui_online_set_chart_mode((int)(intptr_t)lv_event_get_user_data(e));
}

void ui_online_set_chart_mode(int mode)
{
    s_mode = mode ? MODE_IMBALANCE : MODE_DISCHARGE;
    show_mode_series();
    ui_online_chart_refresh();
}

static void fill_none(lv_chart_series_t *s)
{
    for (int k = 0; k < CHART_PTS; k++) lv_chart_set_value_by_id(s_chart, s, k, LV_CHART_POINT_NONE);
}

void ui_online_chart_refresh(void)
{
    if (!s_chart) return;
    int n = hist_count();
    int pts = n < 2 ? 2 : (n < CHART_PTS ? n : CHART_PTS);
    lv_chart_set_point_count(s_chart, pts);

    uint32_t t_end = n ? hist_at(n - 1)->t_s : 0;
    int mins = (int)((t_end + 59) / 60);
    if (mins < 1) mins = 1;
    int tstep = nice_step(mins);
    int tmax = ((mins + tstep - 1) / tstep) * tstep;
    scale_range(s_sc_b, 0, tmax, tmax / tstep + 1);

    if (n < 2) {
        fill_none(s_ser_ut); fill_none(s_ser_ub); fill_none(s_ser_i); fill_none(s_ser_d);
        lv_chart_refresh(s_chart);
        return;
    }
    /* the chart x-axis spans [0, tmax]; samples are placed by index, so stretch
     * the drawn points over the elapsed part only */
    (void)t_end;

    if (s_mode == MODE_DISCHARGE) {
        float vmin = 1e9f, vmax = -1e9f, imax = 0;
        for (int j = 0; j < n; j++) {
            const hist_pt_t *p = hist_at(j);
            if (p->v_t > 0) { vmin = fminf(vmin, p->v_t); vmax = fmaxf(vmax, p->v_t); }
            if (p->v_b > 0) { vmin = fminf(vmin, p->v_b); vmax = fmaxf(vmax, p->v_b); }
            imax = fmaxf(imax, fabsf(p->i_t));
        }
        if (vmax < vmin) { vmin = 0; vmax = 1; }
        int vlo = (int)floorf(vmin - 0.5f), vhi = (int)ceilf(vmax + 0.5f);
        int vst = nice_step(vhi - vlo);
        vlo = (vlo / vst) * vst;
        vhi = ((vhi + vst - 1) / vst) * vst;
        int ist = nice_step((int)ceilf(imax) + 1);
        int ihi = (((int)ceilf(imax) + ist) / ist) * ist;
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, vlo * 1000, vhi * 1000);
        lv_chart_set_range(s_chart, LV_CHART_AXIS_SECONDARY_Y, 0, ihi * 10);
        scale_range(s_sc_l, vlo, vhi, (vhi - vlo) / vst + 1);
        scale_range(s_sc_r, 0, ihi, ihi / ist + 1);

        for (int k = 0; k < pts; k++) {
            const hist_pt_t *p = hist_at((int)((long)k * (n - 1) / (pts - 1)));
            lv_chart_set_value_by_id(s_chart, s_ser_ut, k, (int32_t)(p->v_t * 1000));
            lv_chart_set_value_by_id(s_chart, s_ser_ub, k,
                                     p->v_b > 0 ? (int32_t)(p->v_b * 1000) : LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s_chart, s_ser_i, k, (int32_t)(fabsf(p->i_t) * 10));
        }
        char l1[48], l2[48], l3[48];
        snprintf(l1, sizeof l1, "%s, %s", tr(S_CH_U_TESTER), tr(S_U_V));
        snprintf(l2, sizeof l2, "%s, %s", tr(S_CH_U_BMS), tr(S_U_V));
        snprintf(l3, sizeof l3, "%s, %s", tr(S_CH_I), tr(S_U_A));
        legend(0, l1, C_GREEN);
        legend(1, l2, C_VIOLET);
        legend(2, l3, C_BLUE);
        legend(3, NULL, C_SUB);
    } else {
        /* deviation of every cell from the pack mean, and max-min */
        int ncell = 0;
        float dmax = 1, dabs = 1;
        for (int j = 0; j < n; j++) {
            const hist_pt_t *p = hist_at(j);
            if (p->n < 2) continue;
            if (p->n > ncell) ncell = p->n;
            float mean = 0, lo = 1e9f, hi = -1e9f;
            for (int c = 0; c < p->n; c++) mean += p->cell[c];
            mean /= p->n;
            for (int c = 0; c < p->n; c++) {
                float d = p->cell[c] - mean;
                lo = fminf(lo, d); hi = fmaxf(hi, d);
                dabs = fmaxf(dabs, fabsf(d));
            }
            dmax = fmaxf(dmax, hi - lo);
        }
        if (ncell == 0) {
            fill_none(s_ser_d);
            legend(0, tr(S_BMS_NO_LINK), C_SUB);
            legend(1, NULL, C_SUB); legend(2, NULL, C_SUB); legend(3, NULL, C_SUB);
            lv_chart_refresh(s_chart);
            return;
        }
        int dst = nice_step((int)ceilf(dabs) * 2);
        int dr = (((int)ceilf(dabs) + dst - 1) / dst) * dst;
        if (dr < 5) dr = 5;
        int rst = nice_step((int)ceilf(dmax));
        int rhi = (((int)ceilf(dmax) + rst - 1) / rst) * rst;
        if (rhi < 5) rhi = 5;
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -dr * 10, dr * 10);
        lv_chart_set_range(s_chart, LV_CHART_AXIS_SECONDARY_Y, 0, rhi * 10);
        scale_range(s_sc_l, -dr, dr, 5);
        scale_range(s_sc_r, 0, rhi, rhi / rst + 1);

        /* who drifted most by the end of the record */
        const hist_pt_t *last = hist_at(n - 1);
        int hi_c = 0, lo_c = 0;
        if (last->n >= 2) {
            for (int c = 1; c < last->n; c++) {
                if (last->cell[c] > last->cell[hi_c]) hi_c = c;
                if (last->cell[c] < last->cell[lo_c]) lo_c = c;
            }
        }
        for (int c = 0; c < UI_MAX_CELLS; c++) {
            bool use = c < ncell;
            lv_chart_hide_series(s_chart, s_ser_c[c], !use);
            if (!use) continue;
            lv_chart_set_series_color(s_chart, s_ser_c[c],
                                      c == hi_c ? C_RED : c == lo_c ? C_BLUE : C_GRAYLINE);
        }
        for (int k = 0; k < pts; k++) {
            const hist_pt_t *p = hist_at((int)((long)k * (n - 1) / (pts - 1)));
            float mean = 0, lo = 1e9f, hi = -1e9f;
            for (int c = 0; c < p->n; c++) mean += p->cell[c];
            if (p->n) mean /= p->n;
            for (int c = 0; c < ncell; c++) {
                if (c < p->n) {
                    float d = p->cell[c] - mean;
                    lo = fminf(lo, d); hi = fmaxf(hi, d);
                    lv_chart_set_value_by_id(s_chart, s_ser_c[c], k, (int32_t)(d * 10));
                } else {
                    lv_chart_set_value_by_id(s_chart, s_ser_c[c], k, LV_CHART_POINT_NONE);
                }
            }
            lv_chart_set_value_by_id(s_chart, s_ser_d, k,
                                     p->n >= 2 ? (int32_t)((hi - lo) * 10) : LV_CHART_POINT_NONE);
        }
        /* draw the highlighted cells and the delta on top of the grey ones */
        char l1[48], l2[32], l3[32];
        snprintf(l1, sizeof l1, "%s, %s", tr(S_CH_DELTA), tr(S_U_MV));
        snprintf(l2, sizeof l2, "#%d %s", hi_c + 1, tr(S_CH_UP));
        snprintf(l3, sizeof l3, "#%d %s", lo_c + 1, tr(S_CH_DOWN));
        legend(0, l1, C_AMBER);
        legend(1, l2, C_RED);
        legend(2, l3, C_BLUE);
        legend(3, tr(S_CH_OTHERS), C_GRAYLINE);
    }
    lv_chart_refresh(s_chart);
}

/* ------------------------------------------------------------- cells */

static void cells_layout(int n)
{
    if (n == s_cells_laid) return;
    s_cells_laid = n;
    int cols = (n + 1) / 2;
    if (cols < 1) cols = 1;
    int gap = 6;
    int inner_w = COL_R_W - 24;
    int cw = (inner_w - (cols - 1) * gap) / cols;
    int ch = (CELLS_H - 48 - gap) / 2;
    for (int i = 0; i < UI_MAX_CELLS; i++) {
        if (i >= n) { lv_obj_add_flag(s_cell[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(s_cell[i], LV_OBJ_FLAG_HIDDEN);
        int r = i / cols, c = i % cols;
        lv_obj_set_pos(s_cell[i], 12 + c * (cw + gap), 40 + r * (ch + gap));
        lv_obj_set_size(s_cell[i], cw, ch);
        lv_obj_set_width(s_cell_bar[i], cw > 30 ? 16 : 10);
    }
}

static void update_cells(const online_sample_t *s)
{
    int n = s->bms_present ? s->cell_count : 0;
    if (n > UI_MAX_CELLS) n = UI_MAX_CELLS;
    cells_layout(n);
    lv_label_set_text_fmt(s_cells_hdr, "%s  ·  %dS", tr(S_CELLS), n);
    if (!n) return;

    int lo = s->cell_min_mv - 15, hi = s->cell_max_mv + 15;
    if (hi <= lo) hi = lo + 1;
    int bar_h = 46;
    for (int i = 0; i < n; i++) {
        uint16_t mv = s->cell_mv[i];
        int h = 4 + (int)((long)(mv - lo) * bar_h / (hi - lo));
        if (h < 4) h = 4;
        if (h > bar_h + 4) h = bar_h + 4;
        lv_obj_set_height(s_cell_bar[i], h);
        lv_color_t c = C_GREEN;
        if (s->imbalance_mv >= 5) {
            if (i == s->cell_max_idx) c = C_RED;
            else if (i == s->cell_min_idx) c = C_BLUE;
        }
        lv_obj_set_style_bg_color(s_cell_bar[i], c, 0);
        bool bal = (s->balance_bits >> i) & 1u;
        lv_obj_set_style_border_width(s_cell[i], bal ? 2 : 0, 0);
        lv_label_set_text_fmt(s_cell_v[i], "%d.%03d", mv / 1000, mv % 1000);
    }
}

/* ------------------------------------------------------------- build */

static lv_obj_t *mk_card(lv_obj_t *p, int x, int y, int w, int h)
{
    return mk_box(p, x, y, w, h, C_CARD, 14);
}

lv_obj_t *ui_online_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- tester card ---- */
    lv_obj_t *ct = mk_card(scr, COL_L_X, CONTENT_Y, COL_L_W, TEST_H);
    lv_obj_t *h = mk_label(ct, tr(S_TESTER), 20, C_SUB);
    lv_obj_set_pos(h, 16, 12);
    s_t_tag = mk_label(ct, "", 16, C_AMBER);
    lv_obj_align(s_t_tag, LV_ALIGN_TOP_RIGHT, -16, 14);
    s_t_v = mk_value(ct, 16, 40, 48, C_GREEN, tr(S_U_V), 24);
    s_t_a = mk_value(ct, 16, 104, 32, C_TXT, tr(S_U_A), 20);
    s_t_ah = mk_stat(ct, 16, 160, S_CAPACITY);
    s_t_wh = mk_stat(ct, 200, 160, S_ENERGY);
    s_t_time = mk_stat(ct, 16, 228, S_TIME);
    s_t_temp = mk_stat(ct, 200, 228, S_TEMP);

    /* ---- BMS card ---- */
    lv_obj_t *cb = mk_card(scr, COL_L_X, BMS_Y, COL_L_W, BMS_H);
    s_b_title = mk_label(cb, "BMS", 20, C_SUB);
    lv_obj_set_pos(s_b_title, 16, 12);
    lv_obj_set_width(s_b_title, 250);
    lv_label_set_long_mode(s_b_title, LV_LABEL_LONG_DOT);
    s_b_tag = mk_label(cb, "", 16, C_SUB);
    lv_obj_align(s_b_tag, LV_ALIGN_TOP_RIGHT, -16, 14);
    s_b_soc = mk_value(cb, 16, 40, 48, C_BLUE, "%", 24);
    s_b_v = mk_value(cb, 170, 40, 32, C_TXT, tr(S_U_V), 20);
    s_b_a = mk_value(cb, 170, 84, 32, C_TXT, tr(S_U_A), 20);
    lv_obj_t *dl = mk_label(cb, tr(S_CELL_DELTA), 16, C_SUB);
    lv_obj_set_pos(dl, 16, 136);
    s_b_delta = mk_value(cb, 16, 158, 32, C_AMBER, tr(S_U_MV), 20);
    s_b_minmax = mk_label(cb, "", 20, C_TXT);
    lv_obj_set_pos(s_b_minmax, 16, 206);
    s_b_temps = mk_label(cb, "", 20, C_SUB);
    lv_obj_set_pos(s_b_temps, 16, 244);

    /* ---- chart card ---- */
    lv_obj_t *cc = mk_card(scr, COL_R_X, CONTENT_Y, COL_R_W, CHART_CARD_H);
    lv_obj_t *seg = mk_box(cc, 16, 12, 330, 40, C_CARD2, 10);
    for (int i = 0; i < 2; i++) {
        s_seg[i] = lv_button_create(seg);
        lv_obj_set_size(s_seg[i], 161, 34);
        lv_obj_set_pos(s_seg[i], 3 + i * 164, 3);
        lv_obj_set_style_shadow_width(s_seg[i], 0, 0);
        lv_obj_set_style_radius(s_seg[i], 8, 0);
        lv_obj_add_event_cb(s_seg[i], seg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = mk_label(s_seg[i], tr(i == 0 ? S_CH_DISCHARGE : S_CH_IMBALANCE), 20, C_TXT);
        lv_obj_center(l);
    }
    lv_obj_t *leg = lv_obj_create(cc);
    lv_obj_remove_style_all(leg);
    lv_obj_set_pos(leg, 362, 20);
    lv_obj_set_size(leg, COL_R_W - 362 - 16, 28);
    lv_obj_set_flex_flow(leg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(leg, 18, 0);
    for (int i = 0; i < 4; i++) s_leg[i] = mk_label(leg, "", 16, C_SUB);

    s_chart = lv_chart_create(cc);
    lv_obj_set_pos(s_chart, CHART_X, CHART_Y);
    lv_obj_set_size(s_chart, CHART_W, CHART_H);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_PTS);
    lv_chart_set_div_line_count(s_chart, 5, 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 0, 0);
    lv_obj_set_style_line_color(s_chart, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_chart, 0, LV_PART_INDICATOR);
    for (int c = 0; c < UI_MAX_CELLS; c++)
        s_ser_c[c] = lv_chart_add_series(s_chart, C_GRAYLINE, LV_CHART_AXIS_PRIMARY_Y);
    s_ser_d = lv_chart_add_series(s_chart, C_AMBER, LV_CHART_AXIS_SECONDARY_Y);
    s_ser_i = lv_chart_add_series(s_chart, C_BLUE, LV_CHART_AXIS_SECONDARY_Y);
    s_ser_ub = lv_chart_add_series(s_chart, C_VIOLET, LV_CHART_AXIS_PRIMARY_Y);
    s_ser_ut = lv_chart_add_series(s_chart, C_GREEN, LV_CHART_AXIS_PRIMARY_Y);

    s_sc_l = mk_scale(cc, LV_SCALE_MODE_VERTICAL_LEFT, CHART_X - 56, CHART_Y, 56, CHART_H);
    s_sc_r = mk_scale(cc, LV_SCALE_MODE_VERTICAL_RIGHT, CHART_X + CHART_W, CHART_Y, 56, CHART_H);
    s_sc_b = mk_scale(cc, LV_SCALE_MODE_HORIZONTAL_BOTTOM, CHART_X, CHART_Y + CHART_H, CHART_W, 34);
    lv_obj_t *mn = mk_label(cc, tr(S_U_MIN), 16, C_SUB);
    lv_obj_set_pos(mn, CHART_X + CHART_W + 14, CHART_Y + CHART_H + 10);

    s_mode = MODE_DISCHARGE;
    show_mode_series();

    /* ---- cells card ---- */
    lv_obj_t *cl = mk_card(scr, COL_R_X, CELLS_Y, COL_R_W, CELLS_H);
    s_cells_hdr = mk_label(cl, tr(S_CELLS), 20, C_SUB);
    lv_obj_set_pos(s_cells_hdr, 16, 10);
    lv_obj_t *k1 = mk_label(cl, "min", 16, C_BLUE);
    lv_obj_align(k1, LV_ALIGN_TOP_RIGHT, -200, 14);
    lv_obj_t *k2 = mk_label(cl, "max", 16, C_RED);
    lv_obj_align(k2, LV_ALIGN_TOP_RIGHT, -140, 14);
    lv_obj_t *k3 = mk_label(cl, tr(S_BALANCING), 16, C_AMBER);
    lv_obj_align(k3, LV_ALIGN_TOP_RIGHT, -16, 14);
    for (int i = 0; i < UI_MAX_CELLS; i++) {
        s_cell[i] = mk_box(cl, 0, 0, 60, 90, C_CARD2, 8);
        lv_obj_set_style_border_color(s_cell[i], C_AMBER, 0);
        s_cell_no[i] = mk_label(s_cell[i], "", 16, C_SUB);
        lv_label_set_text_fmt(s_cell_no[i], "%d", i + 1);
        lv_obj_align(s_cell_no[i], LV_ALIGN_TOP_MID, 0, 4);
        s_cell_bar[i] = mk_box(s_cell[i], 0, 0, 16, 10, C_GREEN, 3);
        lv_obj_align(s_cell_bar[i], LV_ALIGN_BOTTOM_MID, 0, -28);
        s_cell_v[i] = mk_label(s_cell[i], "", 16, C_TXT);
        lv_obj_align(s_cell_v[i], LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_add_flag(s_cell[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_cells_laid = -1;

    return scr;
}

/* ------------------------------------------------------------- update */

void ui_sample_finish_cells(online_sample_t *s)
{
    uint16_t lo = 0xFFFF, hi = 0;
    s->cell_min_idx = s->cell_max_idx = 0;
    for (int i = 0; i < s->cell_count && i < UI_MAX_CELLS; i++) {
        uint16_t v = s->cell_mv[i];
        if (!v) continue;
        if (v < lo) { lo = v; s->cell_min_idx = (uint8_t)i; }
        if (v > hi) { hi = v; s->cell_max_idx = (uint8_t)i; }
    }
    if (hi == 0) { s->cell_min_mv = s->cell_max_mv = s->imbalance_mv = 0; return; }
    s->cell_min_mv = lo;
    s->cell_max_mv = hi;
    s->imbalance_mv = (uint16_t)(hi - lo);
}

void ui_online_update(const online_sample_t *s)
{
    if (!s) return;

    set_tag(s_t_tag, tr(s->tester_live ? S_LIVE : S_SIMULATED), s->tester_live ? C_GREEN : C_AMBER);
    lv_label_set_text_fmt(s_t_v, "%.2f", s->voltage_v);
    lv_label_set_text_fmt(s_t_a, "%s%.1f", s->current_a < 0 ? "−" : "", fabsf(s->current_a));
    lv_label_set_text_fmt(s_t_ah, "%.2f %s", s->capacity_ah, tr(S_U_AH));
    lv_label_set_text_fmt(s_t_wh, "%.0f %s", s->energy_wh, tr(S_U_WH));
    lv_label_set_text_fmt(s_t_time, "%02u:%02u:%02u", (unsigned)(s->elapsed_s / 3600),
                          (unsigned)(s->elapsed_s / 60 % 60), (unsigned)(s->elapsed_s % 60));
    lv_label_set_text_fmt(s_t_temp, "%.1f °C", s->temp_c);

    if (s->bms_present) {
        lv_label_set_text_fmt(s_b_title, "BMS  ·  %s", s->bms_name[0] ? s->bms_name : "—");
        set_tag(s_b_tag, tr(s->bms_live ? S_LIVE : S_SIMULATED), s->bms_live ? C_GREEN : C_AMBER);
        lv_label_set_text_fmt(s_b_soc, "%u", s->soc_pct);
        lv_label_set_text_fmt(s_b_v, "%.2f", s->bms_pack_v);
        lv_label_set_text_fmt(s_b_a, "%s%.1f", s->bms_current_a < 0 ? "−" : "", fabsf(s->bms_current_a));
        lv_label_set_text_fmt(s_b_delta, "%u", s->imbalance_mv);
        lv_obj_set_style_text_color(s_b_delta, s->imbalance_mv >= 50 ? C_RED : C_AMBER, 0);
        if (s->cell_count)
            lv_label_set_text_fmt(s_b_minmax, "min #%u %d.%03d   ·   max #%u %d.%03d",
                                  s->cell_min_idx + 1, s->cell_min_mv / 1000, s->cell_min_mv % 1000,
                                  s->cell_max_idx + 1, s->cell_max_mv / 1000, s->cell_max_mv % 1000);
        char t[80] = "";
        static const char *const tn[] = { "T1", "T2", "MOS", "T4" };
        size_t o = 0;
        for (int i = 0; i < s->temp_count && i < 4; i++)
            o += snprintf(t + o, sizeof t - o, "%s%s %.1f", i ? "  ·  " : "", tn[i], s->temp_dc[i] / 10.0f);
        if (s->temp_count) snprintf(t + o, sizeof t - o, " °C");
        lv_label_set_text(s_b_temps, t);
    } else {
        lv_label_set_text(s_b_title, "BMS");
        set_tag(s_b_tag, tr(S_BMS_NO_LINK), C_SUB);
        lv_label_set_text(s_b_soc, "--");
        lv_label_set_text(s_b_v, "--");
        lv_label_set_text(s_b_a, "--");
        lv_label_set_text(s_b_delta, "--");
        lv_label_set_text(s_b_minmax, "");
        lv_label_set_text(s_b_temps, "");
    }
    update_cells(s);
}

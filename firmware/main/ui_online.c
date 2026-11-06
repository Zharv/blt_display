#include "ui.h"
#include "ui_style.h"
#include "fonts.h"
#include "i18n.h"
#include "hist.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
/* The plot is rasterised straight into a canvas instead of using lv_chart:
 * LVGL 9.2's chart makes one draw task per line segment, and with ~21 series x
 * 120 points its O(n^2) task scheduler (is_independent) hung the LVGL task
 * until the watchdog fired. Now a redraw is a single image blit. */
/* Per-cell identity colours: the same colour on the tile and on its plot line,
 * so a cell can be followed through the test. Picked greedily in OKLab: each
 * next colour is the one farthest from ALL earlier ones (chroma <= 0.135 so
 * they stay muted on the dark UI), so the first N cells of any pack get the N
 * best-separated colours and none repeats (closest pair among the first 20:
 * dE 9.9). A tap on a tile also highlights that cell on the plot. */
static const uint32_t CELL_PAL[UI_MAX_CELLS] = {
    0xb58d89, 0x49def2, 0x0d7cb6, 0xd5cb65, 0x49863b, 0xe7aefe, 0x98599e, 0x79a0f0,
    0x4eb986, 0xa66205, 0xfda296, 0xaa992e, 0x62908e, 0xc487d2, 0x7ee0a5, 0xabbfdf,
    0xbc5b64, 0x36b3c9, 0x7e7ace, 0xb0b996, 0xe1994f, 0x707191, 0x797654, 0xc8793b,
    0xe27e98, 0xd3a4be, 0x8d97b8, 0x039e78, 0xe4c09a, 0x8f9f7e, 0xa77395, 0x9ab85b,
};
static inline lv_color_t cell_color(int i) { return lv_color_hex(CELL_PAL[i % UI_MAX_CELLS]); }
static int s_sel = -1;          /* highlighted cell, -1 = none */
static bool s_plot_dirty;

#define AX_PRI 0
#define AX_SEC 1
#define PT_NONE LV_CHART_POINT_NONE
typedef struct {
    int32_t    v[CHART_PTS];
    lv_color_t c;
    bool       hidden;
    uint8_t    axis;
    int8_t     cell;            /* cell index, -1 for the other series */
} pser_t;

static pser_t s_ps[UI_MAX_CELLS + 4];
static int s_ps_n;
static int s_pts = CHART_PTS;
static int32_t s_rng[2][2] = { { 0, 100 }, { 0, 100 } };
static lv_draw_buf_t *s_plot_buf;

static lv_obj_t *s_chart, *s_seg[2], *s_leg[4], *s_sc_l, *s_sc_r, *s_sc_b;
static pser_t *s_ser_ut, *s_ser_ub, *s_ser_i, *s_ser_d, *s_ser_c[UI_MAX_CELLS];

static pser_t *ps_add(lv_color_t c, int axis)
{
    pser_t *p = &s_ps[s_ps_n++];
    p->c = c;
    p->axis = (uint8_t)axis;
    p->hidden = false;
    p->cell = -1;
    for (int k = 0; k < CHART_PTS; k++) p->v[k] = PT_NONE;
    return p;
}

static inline void ps_set(pser_t *p, int k, int32_t v) { if (k >= 0 && k < CHART_PTS) p->v[k] = v; }
static inline void ps_hide(pser_t *p, bool h) { p->hidden = h; }
static inline void ps_color(pser_t *p, lv_color_t c) { p->c = c; }
static inline void ps_range(int axis, int32_t lo, int32_t hi) { s_rng[axis][0] = lo; s_rng[axis][1] = hi > lo ? hi : lo + 1; }
static inline void ps_points(int n) { s_pts = n < 2 ? 2 : n > CHART_PTS ? CHART_PTS : n; }

static void plot_line(uint16_t *fb, int sp, int x0, int y0, int x1, int y1, uint16_t c, int th)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < CHART_W)
            for (int t = 0; t < th; t++) {
                int y = y0 + t - th / 2;
                if (y >= 0 && y < CHART_H) fb[y * sp + x0] = c;
            }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void plot_series(uint16_t *fb, int sp, const pser_t *p, lv_color_t col, int th)
{
    int32_t lo = s_rng[p->axis][0], hi = s_rng[p->axis][1];
    uint16_t c = lv_color_to_u16(col);
    int px = 0, py = 0;
    bool have = false;
    for (int k = 0; k < s_pts; k++) {
        if (p->v[k] == PT_NONE) { have = false; continue; }
        int x = (int)((long)k * (CHART_W - 1) / (s_pts - 1));
        long t = (long)(p->v[k] - lo) * (CHART_H - 2) / (hi - lo);
        if (t < 0) t = 0;
        if (t > CHART_H - 2) t = CHART_H - 2;
        int y = CHART_H - 2 - (int)t;
        if (have) plot_line(fb, sp, px, py, x, y, c, th);
        px = x; py = y; have = true;
    }
}

static void plot_render(void)
{
    if (!s_plot_buf) return;
    uint16_t *fb = (uint16_t *)s_plot_buf->data;
    int sp = (int)(s_plot_buf->header.stride / 2);
    uint16_t bg = lv_color_to_u16(C_CARD), grid = lv_color_to_u16(C_LINE);
    for (int y = 0; y < CHART_H; y++) {
        uint16_t *row = fb + y * sp;
        for (int x = 0; x < CHART_W; x++) row[x] = bg;
    }
    for (int i = 0; i < 5; i++) {                   /* 5 horizontal grid lines */
        int y = i * (CHART_H - 1) / 4;
        for (int x = 0; x < CHART_W; x++) fb[y * sp + x] = grid;
    }
    /* cells first (dimmed when another one is highlighted), then the other
     * series, then the highlighted cell thick on top */
    lv_color_t bgc = C_CARD;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < s_ps_n; i++) {
            const pser_t *p = &s_ps[i];
            if (p->hidden) continue;
            bool is_cell = p->cell >= 0, is_sel = is_cell && p->cell == s_sel;
            if (pass == 0 && is_cell && !is_sel)
                plot_series(fb, sp, p, s_sel >= 0 ? lv_color_mix(p->c, bgc, 80) : p->c, 2);
            else if (pass == 1 && !is_cell)
                plot_series(fb, sp, p, p->c, 2);
            else if (pass == 2 && is_sel)
                plot_series(fb, sp, p, lv_color_lighten(p->c, 60), 4);
        }
    lv_obj_invalidate(s_chart);
}
static chart_mode_t s_mode;
/* cells */
static lv_obj_t *s_cells_hdr, *s_cell[UI_MAX_CELLS], *s_cell_no[UI_MAX_CELLS],
                *s_cell_bar[UI_MAX_CELLS], *s_cell_v[UI_MAX_CELLS], *s_cell_strip[UI_MAX_CELLS];
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

/* Every lv_label_set_text / lv_obj_set_style_* invalidates and re-lays out the
 * object even when nothing changed; with ~70 readouts refreshed 4x a second
 * that kept the LVGL task at 100% CPU. Only touch what actually changed. */
static void set_txt(lv_obj_t *l, const char *fmt, ...)
{
    char b[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, b) != 0) lv_label_set_text(l, b);
}

static void set_txt_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c))
        lv_obj_set_style_text_color(o, c, 0);
}

static void set_bg_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), c))
        lv_obj_set_style_bg_color(o, c, 0);
}

static void set_hidden(lv_obj_t *o, bool hide)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != hide) {
        if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_tag(lv_obj_t *tag, const char *txt, lv_color_t c)
{
    set_txt(tag, "%s", txt);
    set_txt_color(tag, c);
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
    if (lv_scale_get_range_min_value(s) != lo || lv_scale_get_range_max_value(s) != hi)
        lv_scale_set_range(s, lo, hi);
    if ((int)lv_scale_get_total_tick_count(s) != ticks) {
        lv_scale_set_total_tick_count(s, ticks);
        lv_scale_set_major_tick_every(s, 1);
    }
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
    set_hidden(s_leg[i], !txt);
    if (!txt) return;
    set_txt(s_leg[i], "%s %s", LV_SYMBOL_MINUS, txt);
    set_txt_color(s_leg[i], c);
}

static void legend_plain(int i, const char *txt, lv_color_t c)
{
    set_hidden(s_leg[i], !txt);
    if (!txt) return;
    set_txt(s_leg[i], "%s", txt);
    set_txt_color(s_leg[i], c);
}

/* last visibility / colour applied to each cell series (-1 = unknown) */
static int8_t s_cell_shown[UI_MAX_CELLS] = { [0 ... UI_MAX_CELLS - 1] = -1 };

static void show_mode_series(void)
{
    bool d = (s_mode == MODE_DISCHARGE);
    ps_hide(s_ser_ut, !d);
    ps_hide(s_ser_ub, !d);
    ps_hide(s_ser_i, !d);
    ps_hide(s_ser_d, true);          /* Δ is shown as text, not on a second axis */
    set_hidden(s_sc_r, !d);
    for (int c = 0; c < UI_MAX_CELLS; c++) {
        ps_hide(s_ser_c[c], true);
        s_cell_shown[c] = 0;
    }
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

static void fill_none(pser_t *s)
{
    for (int k = 0; k < CHART_PTS; k++) ps_set(s, k, LV_CHART_POINT_NONE);
}

void ui_online_chart_refresh(void)
{
    if (!s_chart || lv_obj_get_screen(s_chart) != lv_screen_active()) return;
    static int last_n = -1, last_mode = -1;
    static const hist_pt_t *last_tail;
    int n = hist_count();
    const hist_pt_t *tail = n ? hist_at(n - 1) : NULL;
    uint32_t tail_t = tail ? tail->t_s : 0;
    static uint32_t last_t;
    if (!s_plot_dirty && n == last_n && (int)s_mode == last_mode && tail == last_tail && tail_t == last_t) return;
    s_plot_dirty = false;
    last_n = n; last_mode = (int)s_mode; last_tail = tail; last_t = tail_t;
    int pts = n < 2 ? 2 : (n < CHART_PTS ? n : CHART_PTS);
    ps_points(pts);

    uint32_t t_end = n ? hist_at(n - 1)->t_s : 0;
    int mins = (int)((t_end + 59) / 60);
    if (mins < 1) mins = 1;
    int tstep = nice_step(mins);
    int tmax = ((mins + tstep - 1) / tstep) * tstep;
    scale_range(s_sc_b, 0, tmax, tmax / tstep + 1);

    if (n < 2) {
        fill_none(s_ser_ut); fill_none(s_ser_ub); fill_none(s_ser_i); fill_none(s_ser_d);
        plot_render();
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
        ps_range(AX_PRI, vlo * 1000, vhi * 1000);
        ps_range(AX_SEC, 0, ihi * 10);
        scale_range(s_sc_l, vlo, vhi, (vhi - vlo) / vst + 1);
        scale_range(s_sc_r, 0, ihi, ihi / ist + 1);

        for (int k = 0; k < pts; k++) {
            const hist_pt_t *p = hist_at((int)((long)k * (n - 1) / (pts - 1)));
            ps_set(s_ser_ut, k, (int32_t)(p->v_t * 1000));
            ps_set(s_ser_ub, k,
                                     p->v_b > 0 ? (int32_t)(p->v_b * 1000) : LV_CHART_POINT_NONE);
            ps_set(s_ser_i, k, (int32_t)(fabsf(p->i_t) * 10));
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
            plot_render();
            return;
        }
        int dst = nice_step((int)ceilf(dabs) * 2);
        int dr = (((int)ceilf(dabs) + dst - 1) / dst) * dst;
        if (dr < 5) dr = 5;
        int rst = nice_step((int)ceilf(dmax));
        int rhi = (((int)ceilf(dmax) + rst - 1) / rst) * rst;
        if (rhi < 5) rhi = 5;
        (void)rhi;
        ps_range(AX_PRI, -dr * 10, dr * 10);
        scale_range(s_sc_l, -dr, dr, 5);

        /* spread now (last sample) */
        const hist_pt_t *last = hist_at(n - 1);
        int dnow = 0;
        if (last->n >= 2) {
            uint16_t lo = 0xFFFF, hi = 0;
            for (int c = 0; c < last->n; c++) {
                if (last->cell[c] < lo) lo = last->cell[c];
                if (last->cell[c] > hi) hi = last->cell[c];
            }
            dnow = hi - lo;
        }
        for (int c = 0; c < UI_MAX_CELLS; c++) {
            int use = c < ncell;
            if (s_cell_shown[c] != use) { ps_hide(s_ser_c[c], !use); s_cell_shown[c] = (int8_t)use; }
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
                    ps_set(s_ser_c[c], k, (int32_t)(d * 10));
                } else {
                    ps_set(s_ser_c[c], k, LV_CHART_POINT_NONE);
                }
            }
            ps_set(s_ser_d, k,
                                     p->n >= 2 ? (int32_t)((hi - lo) * 10) : LV_CHART_POINT_NONE);
        }
        char l1[64], l3[16];
        snprintf(l1, sizeof l1, tr(S_CH_DELTA_NOW), dnow, (int)lroundf(dmax));
        legend_plain(0, l1, C_TXT);
        legend_plain(1, tr(S_CH_DEV), C_SUB);
        if (s_sel >= 0 && s_sel < ncell) {
            snprintf(l3, sizeof l3, "#%d", s_sel + 1);
            legend(2, l3, cell_color(s_sel));
        } else {
            legend(2, NULL, C_SUB);
        }
        legend(3, NULL, C_SUB);
    }
    plot_render();
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
        lv_obj_set_width(s_cell_strip[i], cw - 16);
        lv_obj_set_width(s_cell_bar[i], cw > 30 ? 16 : 10);
    }
}

/* tap a tile: highlight that cell on the plot (tap again to clear) */
static void cell_tap_cb(lv_event_t *e)
{
    ui_online_select_cell((int)(intptr_t)lv_event_get_user_data(e));
}

void ui_online_select_cell(int i)
{
    if (i < -1 || i >= UI_MAX_CELLS) return;
    if (s_sel >= 0) lv_obj_set_style_outline_width(s_cell[s_sel], 0, 0);
    s_sel = (s_sel == i || i < 0) ? -1 : i;
    if (s_sel >= 0) lv_obj_set_style_outline_width(s_cell[s_sel], 2, 0);
    if (s_mode != MODE_IMBALANCE) ui_online_set_chart_mode(1);   /* jump to the cells plot */
    s_plot_dirty = true;
    ui_online_chart_refresh();
}

static void update_cells(const online_sample_t *s)
{
    int n = s->bms_present ? s->cell_count : 0;
    if (n > UI_MAX_CELLS) n = UI_MAX_CELLS;
    cells_layout(n);
    set_txt(s_cells_hdr, "%s  ·  %dS", tr(S_CELLS), n);
    if (!n) return;

    int lo = s->cell_min_mv - 15, hi = s->cell_max_mv + 15;
    if (hi <= lo) hi = lo + 1;
    int bar_h = 46;
    static uint16_t last_mv[UI_MAX_CELLS];
    static uint32_t last_key[UI_MAX_CELLS];
    for (int i = 0; i < n; i++) {
        uint16_t mv = s->cell_mv[i];
        /* one dirty area per changed tile instead of bar + label + border */
        uint32_t key = ((uint32_t)(i == s->cell_max_idx) << 0) | ((uint32_t)(i == s->cell_min_idx) << 1) |
                       (((s->balance_bits >> i) & 1u) << 2) | ((uint32_t)(s->imbalance_mv >= 5) << 3) |
                       ((uint32_t)(lo & 0xFFF) << 4) | ((uint32_t)(hi & 0xFFF) << 16);
        if (mv == last_mv[i] && key == last_key[i]) continue;
        last_mv[i] = mv;
        last_key[i] = key;
        lv_obj_invalidate(s_cell[i]);
        int h = 4 + (int)((long)(mv - lo) * bar_h / (hi - lo));
        if (h < 4) h = 4;
        if (h > bar_h + 4) h = bar_h + 4;
        if (lv_obj_get_style_height(s_cell_bar[i], LV_PART_MAIN) != h) lv_obj_set_height(s_cell_bar[i], h);
        int bw = ((s->balance_bits >> i) & 1u) ? 2 : 0;
        if (lv_obj_get_style_border_width(s_cell[i], LV_PART_MAIN) != bw)
            lv_obj_set_style_border_width(s_cell[i], bw, 0);
        const char *mark = "";
        if (s->imbalance_mv >= 5) {
            if (i == s->cell_max_idx) mark = LV_SYMBOL_UP " ";
            else if (i == s->cell_min_idx) mark = LV_SYMBOL_DOWN " ";
        }
        set_txt(s_cell_v[i], "%s%d.%03d", mark, mv / 1000, mv % 1000);
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

    s_chart = lv_canvas_create(cc);
    s_plot_buf = lv_draw_buf_create(CHART_W, CHART_H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    lv_canvas_set_draw_buf(s_chart, s_plot_buf);
    lv_obj_set_pos(s_chart, CHART_X, CHART_Y);
    s_ps_n = 0;
    for (int c = 0; c < UI_MAX_CELLS; c++) {
        s_ser_c[c] = ps_add(cell_color(c), AX_PRI);
        s_ser_c[c]->cell = (int8_t)c;
    }
    s_ser_d = ps_add(C_AMBER, AX_SEC);
    s_ser_i = ps_add(C_BLUE, AX_SEC);
    s_ser_ub = ps_add(C_VIOLET, AX_PRI);
    s_ser_ut = ps_add(C_GREEN, AX_PRI);
    plot_render();                      /* the draw buffer starts uninitialised */

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
    lv_obj_t *keys = lv_obj_create(cl);
    lv_obj_remove_style_all(keys);
    lv_obj_set_size(keys, LV_SIZE_CONTENT, 24);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(keys, 18, 0);
    lv_obj_align(keys, LV_ALIGN_TOP_RIGHT, -16, 12);
    mk_label(keys, tr(S_CELL_TAP), 16, C_MUTED);
    mk_label(keys, LV_SYMBOL_UP " max", 16, C_SUB);
    mk_label(keys, LV_SYMBOL_DOWN " min", 16, C_SUB);
    mk_label(keys, tr(S_BALANCING), 16, C_AMBER);
    for (int i = 0; i < UI_MAX_CELLS; i++) {
        s_cell[i] = mk_box(cl, 0, 0, 60, 90, C_CARD2, 8);
        lv_obj_set_style_border_color(s_cell[i], C_AMBER, 0);
        s_cell_no[i] = mk_label(s_cell[i], "", 16, C_SUB);
        lv_label_set_text_fmt(s_cell_no[i], "%d", i + 1);
        lv_obj_align(s_cell_no[i], LV_ALIGN_TOP_MID, 0, 4);
        s_cell_strip[i] = mk_box(s_cell[i], 0, 0, 40, 4, cell_color(i), 2);
        lv_obj_align(s_cell_strip[i], LV_ALIGN_TOP_MID, 0, 2);
        s_cell_bar[i] = mk_box(s_cell[i], 0, 0, 16, 10, cell_color(i), 3);
        lv_obj_add_flag(s_cell[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_cell[i], cell_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_style_outline_color(s_cell[i], C_TXT, 0);
        lv_obj_set_style_outline_pad(s_cell[i], 1, 0);
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

    set_tag(s_t_tag, tr(s->tester_live ? S_LIVE : s->tester_usb ? S_USB_LINK : S_SIMULATED),
            s->tester_live ? C_GREEN : s->tester_usb ? C_BLUE : C_AMBER);
    set_txt(s_t_v, "%.2f", s->voltage_v);
    set_txt(s_t_a, "%s%.1f", s->current_a < 0 ? "−" : "", fabsf(s->current_a));
    set_txt(s_t_ah, "%.2f %s", s->capacity_ah, tr(S_U_AH));
    set_txt(s_t_wh, "%.0f %s", s->energy_wh, tr(S_U_WH));
    set_txt(s_t_time, "%02u:%02u:%02u", (unsigned)(s->elapsed_s / 3600),
                          (unsigned)(s->elapsed_s / 60 % 60), (unsigned)(s->elapsed_s % 60));
    set_txt(s_t_temp, "%.1f °C", s->temp_c);

    if (s->bms_present) {
        set_txt(s_b_title, "BMS  ·  %s", s->bms_name[0] ? s->bms_name : "—");
        set_tag(s_b_tag, tr(s->bms_live ? S_LIVE : S_SIMULATED), s->bms_live ? C_GREEN : C_AMBER);
        set_txt(s_b_soc, "%u", s->soc_pct);
        set_txt(s_b_v, "%.2f", s->bms_pack_v);
        set_txt(s_b_a, "%s%.1f", s->bms_current_a < 0 ? "−" : "", fabsf(s->bms_current_a));
        set_txt(s_b_delta, "%u", s->imbalance_mv);
        set_txt_color(s_b_delta, s->imbalance_mv >= 50 ? C_RED : C_AMBER);
        if (s->cell_count)
            set_txt(s_b_minmax, "min #%u %d.%03d   ·   max #%u %d.%03d",
                                  s->cell_min_idx + 1, s->cell_min_mv / 1000, s->cell_min_mv % 1000,
                                  s->cell_max_idx + 1, s->cell_max_mv / 1000, s->cell_max_mv % 1000);
        char t[80] = "";
        static const char *const tn[] = { "T1", "T2", "MOS", "T4" };
        size_t o = 0;
        for (int i = 0; i < s->temp_count && i < 4; i++)
            o += snprintf(t + o, sizeof t - o, "%s%s %.1f", i ? "  ·  " : "", tn[i], s->temp_dc[i] / 10.0f);
        if (s->temp_count) snprintf(t + o, sizeof t - o, " °C");
        set_txt(s_b_temps, "%s", t);
    } else {
        set_txt(s_b_title, "BMS");
        set_tag(s_b_tag, tr(S_BMS_NO_LINK), C_SUB);
        set_txt(s_b_soc, "--");
        set_txt(s_b_v, "--");
        set_txt(s_b_a, "--");
        set_txt(s_b_delta, "--");
        set_txt(s_b_minmax, "");
        set_txt(s_b_temps, "");
    }
    update_cells(s);
}

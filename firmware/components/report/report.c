#include "report.h"

#include <stdio.h>
#include <string.h>

#define BLACK 0
#define GRAY  128
#define LGRAY 190

static void kv(raster_t *r, int x, int y, int s, const char *k, const char *v)
{
    int nx = raster_text(r, x, y, k, s, GRAY);
    raster_text(r, nx + 8 * s, y, v, s, BLACK);
}

static void kvf(raster_t *r, int x, int y, int s, const char *k, const char *fmt, double val)
{
    char buf[48];
    snprintf(buf, sizeof buf, fmt, val);
    kv(r, x, y, s, k, buf);
}

static void draw_curve(raster_t *r, int gx, int gy, int gw, int gh,
                       const float *v, int n, float vmin, float vmax, uint8_t gray)
{
    if (!v || n < 2 || vmax <= vmin) return;
    int px = gx, py = 0;
    for (int i = 0; i < n; i++) {
        int x = gx + (int)((long)i * (gw - 1) / (n - 1));
        float t = (v[i] - vmin) / (vmax - vmin);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        int y = gy + gh - 1 - (int)(t * (gh - 1));
        if (i > 0) raster_line(r, px, py, x, y, gray);
        px = x; py = y;
    }
}

float report_nice_ceil(float v)
{
    static const float steps[] = { 5, 10, 20, 30, 50, 100, 200, 300, 500, 1000 };
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++)
        if (v <= steps[i]) return steps[i];
    return v;
}

void report_render(const report_data_t *d, raster_t *r)
{
    int W = r->w, H = r->h;
    int s = W / 500; if (s < 1) s = 1;      /* body font scale */
    int st = s * 2;                          /* title scale     */
    int m = 16 * s;                          /* margin          */
    int lh = 10 * s;                         /* line height     */
    int y = m;

    /* ---- header ---- */
    const char *title = d->title ? d->title : "BATTERY TEST REPORT";
    raster_text(r, m, y, title, st, BLACK);
    y += 8 * st + 4 * s;
    raster_fill(r, m, y, W - 2 * m, (s > 1 ? 2 : 1), BLACK);
    y += lh;

    /* ---- meta (two columns) ---- */
    int colw = (W - 2 * m) / 2;
    int y0 = y;
    kv(r, m, y, s, "Date:",     d->date ? d->date : "-");            y += lh;
    kv(r, m, y, s, "Operator:", d->operator_name ? d->operator_name : "-"); y += lh;
    kv(r, m, y, s, "Battery:",  d->battery_id ? d->battery_id : "-"); y += lh;
    int yr = y0;
    kvf(r, m + colw, yr, s, "Nominal V:",  "%.1f", d->nominal_v);   yr += lh;
    kvf(r, m + colw, yr, s, "Nominal Ah:", "%.1f", d->nominal_ah);  yr += lh;
    kvf(r, m + colw, yr, s, "Discharge A:", "%.1f", d->discharge_a); yr += lh;
    if (yr > y) y = yr;
    y += lh / 2;

    /* ---- result box ---- */
    int rbh = 14 * s;
    raster_rect(r, m, y, W - 2 * m, rbh, BLACK);
    int rb = y + 3 * s;
    char buf[128];
    snprintf(buf, sizeof buf, "RESULT:  %.2f Ah   %u:%02u:%02u   Vmin %.2f V   Tmax %.1f C",
             d->discharged_ah, (unsigned)(d->duration_s / 3600), (unsigned)(d->duration_s / 60 % 60),
             (unsigned)(d->duration_s % 60), d->min_voltage_v, d->max_temp_c);
    raster_text(r, m + 6 * s, rb, buf, s, BLACK);
    y += rbh + lh;

    /* ---- discharge graph ---- */
    raster_text(r, m, y, "DISCHARGE", s, GRAY);
    y += lh;
    int gx = m + 10 * s, gy = y, gw = W - 2 * m - 12 * s, gh = (H - y) / 3;
    raster_rect(r, gx, gy, gw, gh, BLACK);
    /* gridlines */
    for (int i = 1; i < 4; i++) raster_hline(r, gx + 1, gx + gw - 2, gy + gh * i / 4, LGRAY);
    draw_curve(r, gx, gy, gw, gh, d->curve_i, d->curve_n, 0.0f, d->curve_imax, GRAY);
    draw_curve(r, gx, gy, gw, gh, d->curve_v, d->curve_n, d->curve_vmin, d->curve_vmax, BLACK);
    /* y-axis labels */
    snprintf(buf, sizeof buf, "%.0f", d->curve_vmax); raster_text(r, m, gy - 2, buf, s, GRAY);
    snprintf(buf, sizeof buf, "%.0f", d->curve_vmin); raster_text(r, m, gy + gh - 8 * s, buf, s, GRAY);
    raster_text(r, gx + 4 * s, gy + 2 * s, "V (black)  I (gray)", s, GRAY);
    y = gy + gh + lh;

    /* ---- cell imbalance graph (max - min over the test) ---- */
    if (d->curve_delta_mv && d->curve_n >= 2) {
        float dmax = 0, dend = d->curve_delta_mv[d->curve_n - 1];
        for (int i = 0; i < d->curve_n; i++)
            if (d->curve_delta_mv[i] > dmax) dmax = d->curve_delta_mv[i];
        float top = report_nice_ceil(dmax);
        snprintf(buf, sizeof buf, "CELL IMBALANCE  max-min, mV   (end %.0f, peak %.0f)", dend, dmax);
        raster_text(r, m, y, buf, s, GRAY);
        y += lh;
        int ih = gh / 2;
        raster_rect(r, gx, y, gw, ih, BLACK);
        for (int i = 1; i < 4; i++) raster_hline(r, gx + 1, gx + gw - 2, y + ih * i / 4, LGRAY);
        draw_curve(r, gx, y, gw, ih, d->curve_delta_mv, d->curve_n, 0.0f, top, BLACK);
        snprintf(buf, sizeof buf, "%.0f", top); raster_text(r, m, y - 2, buf, s, GRAY);
        raster_text(r, m, y + ih - 8 * s, "0", s, GRAY);
        y += ih + lh;
    }

    /* ---- BMS section ---- */
    if (d->bms_present && d->cell_mv && d->cell_count > 0) {
        raster_text(r, m, y, "BMS", s, GRAY);
        int imin = 0, imax = 0;
        for (int i = 1; i < d->cell_count; i++) {
            if (d->cell_mv[i] < d->cell_mv[imin]) imin = i;
            if (d->cell_mv[i] > d->cell_mv[imax]) imax = i;
        }
        snprintf(buf, sizeof buf, "SOC %u%%   dU %u mV   min #%d %u   max #%d %u",
                 d->soc_pct, d->imbalance_mv, imin + 1, d->cell_mv[imin], imax + 1, d->cell_mv[imax]);
        raster_text(r, m + 6 * 8 * s, y, buf, s, BLACK);
        y += lh;
        int n = d->cell_count;
        int bw = (W - 2 * m) / (n + 1);
        if (bw < 4 * s) bw = 4 * s;
        int bar_area = 10 * s;
        int lo = d->cell_min_mv ? d->cell_min_mv - 30 : 3000;
        int hi = d->cell_max_mv ? d->cell_max_mv + 30 : 3600;
        if (hi <= lo) hi = lo + 1;
        for (int i = 0; i < n; i++) {
            int bx = m + i * bw;
            int hpx = (int)((long)(d->cell_mv[i] - lo) * bar_area / (hi - lo));
            if (hpx < s) hpx = s;
            uint8_t g = GRAY;
            if (d->imbalance_mv >= 30) {
                if (d->cell_mv[i] == d->cell_max_mv) g = BLACK;
                else if (d->cell_mv[i] == d->cell_min_mv) g = LGRAY;
            }
            raster_fill(r, bx, y + (bar_area - hpx), bw - 2 * s, hpx, g);
            raster_rect(r, bx, y, bw - 2 * s, bar_area, LGRAY);
        }
        y += bar_area + lh;
    }

    /* ---- footer ---- */
    raster_fill(r, m, H - m - 2 * s, W - 2 * m, s, LGRAY);
    raster_text(r, m, H - m, "Generated by blt-esp-display", s, GRAY);
}

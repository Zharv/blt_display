#include "doc.h"
#include "rtext.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* layout units are 300-dpi A4 pixels, scaled to the raster */
static float s_k = 1.0f;
#define U(x) ((int)((x) * s_k + 0.5f))

static const char *plural(uint32_t n, const char *one, const char *few, const char *many)
{
    uint32_t a = n % 100, b = n % 10;
    if (a >= 11 && a <= 14) return many;
    if (b == 1) return one;
    if (b >= 2 && b <= 4) return few;
    return many;
}

void doc_duration_ua(uint32_t minutes, char *out, int n)
{
    uint32_t h = minutes / 60, m = minutes % 60;
    if (h && m)
        snprintf(out, (size_t)n, "%u %s %u %s", (unsigned)h, plural(h, "година", "години", "годин"),
                 (unsigned)m, plural(m, "хвилина", "хвилини", "хвилин"));
    else if (h)
        snprintf(out, (size_t)n, "%u %s", (unsigned)h, plural(h, "година", "години", "годин"));
    else
        snprintf(out, (size_t)n, "%u %s", (unsigned)m, plural(m, "хвилина", "хвилини", "хвилин"));
}

/* ------------------------------------------------------------ drawing */

static void thick_line(raster_t *r, int x0, int y0, int x1, int y1, int th, uint8_t g)
{
    RASTER_YIELD();
    for (int a = -th / 2; a < th - th / 2; a++)
        for (int b = -th / 2; b < th - th / 2; b++)
            raster_line(r, x0 + a, y0 + b, x1 + a, y1 + b, g);
}

static void dashed_hline(raster_t *r, int x0, int x1, int y, int th, int dash, uint8_t g)
{
    for (int x = x0; x < x1; x += 2 * dash) {
        int e = x + dash < x1 ? x + dash : x1;
        for (int t = 0; t < th; t++) raster_hline(r, x, e, y + t, g);
    }
}

static float nice_step(float span, int max_div)
{
    static const float st[] = { 0.1f, 0.2f, 0.5f, 1, 2, 5, 10, 20, 25, 50, 100, 200, 500 };
    for (size_t i = 0; i < sizeof st / sizeof st[0]; i++)
        if (span / st[i] <= max_div) return st[i];
    return 1000;
}

static int nice_min_step(int span_min)
{
    static const int st[] = { 1, 2, 5, 10, 15, 20, 30, 60, 120, 180, 240, 360, 720, 1440 };
    for (size_t i = 0; i < sizeof st / sizeof st[0]; i++)
        if (span_min / st[i] <= 10) return st[i];
    return 2880;
}

/* ------------------------------------------------------------ figure 1 */

static int info_table(raster_t *r, int x, int y, int w, const doc_kv_t *kv, int n)
{
    const int rows = (n + 1) / 2, rh = U(64), px = U(38);
    const int cw = w / 2, lw = cw * 56 / 100;
    for (int i = 0; i < n; i++) {
        int col = i / rows, row = i % rows;
        int cx = x + col * cw, cy = y + row * rh;
        if (row % 2 == 0) raster_fill(r, cx, cy, cw, rh, 242);
        rtext_draw(r, cx + U(16), cy + U(10), kv[i].label, px, 70, false);
        rtext_draw(r, cx + lw, cy + U(10), kv[i].value, px, 0, true);
    }
    int h = rows * rh;
    for (int t = 0; t < U(3); t++) raster_rect(r, x - t, y - t, w + 2 * t, h + 2 * t, 0);
    raster_vline(r, x + cw, y, y + h - 1, 120);
    return y + h;
}

/* ------------------------------------------------------------ figure 2 */

/* both diagrams share the time axis: 0 .. the last sample of either */
static int axis_tmax(const doc_protocol_t *d)
{
    int tmax = 1;
    for (int k = 0; k < d->n; k++) if (d->t_min[k] > tmax) tmax = d->t_min[k];
    if (d->dn > 0 && d->dt_min[d->dn - 1] > tmax) tmax = (int)ceilf(d->dt_min[d->dn - 1]);
    return tmax;
}

static void diagram(raster_t *r, int x, int y, int w, int h, const doc_protocol_t *d)
{
    const int px = U(34);
    const int ax = x + U(120), ay = y + U(70);          /* plot area */
    const int aw = w - U(120) - (d->i ? U(120) : U(30)), ah = h - U(70) - U(110);

    /* ranges */
    float vmin = 1e9f, vmax = -1e9f, imax = 0;
    int tmax = 1;
    for (int k = 0; k < d->n; k++) {
        if (d->v[k] < vmin) vmin = d->v[k];
        if (d->v[k] > vmax) vmax = d->v[k];
        if (d->i && d->i[k] > imax) imax = d->i[k];
    }
    tmax = axis_tmax(d);
    if (d->v_stop > 0) { vmin = fminf(vmin, d->v_stop); vmax = fmaxf(vmax, d->v_stop); }
    if (vmax < vmin) { vmin = 0; vmax = 100; }
    float vs = nice_step(vmax - vmin + 1, 8);
    float v0 = floorf((vmin - vs * 0.3f) / vs) * vs, v1 = ceilf((vmax + vs * 0.3f) / vs) * vs;
    float is = nice_step(imax * 1.15f + 1, 8);
    float i1 = ceilf((imax * 1.15f + 1) / is) * is;
    /* the time axis is exactly the recorded span; grid lines at round clock times */
    int ts = nice_min_step(tmax);
    int t1 = tmax;

#define XO(t) (ax + (int)((int64_t)(t) * (aw - 1) / t1))
#define YV(v) (ay + ah - 1 - (int)(((v) - v0) * (ah - 1) / (v1 - v0)))
#define YI(i) (ay + ah - 1 - (int)((i) * (ah - 1) / i1))

    /* grid + axis labels */
    char s[96];
    for (float v = v0; v <= v1 + vs * 0.01f; v += vs) {
        int yy = YV(v);
        raster_hline(r, ax, ax + aw - 1, yy, 200);
        snprintf(s, sizeof s, vs < 1 ? "%.1f" : "%.0f", v);
        rtext_draw_right(r, ax - U(14), yy - px * 5 / 8, s, px, 40, false);
    }
    if (d->i)
        for (float i = 0; i <= i1 + is * 0.01f; i += is) {
            snprintf(s, sizeof s, "%.0f", i);
            rtext_draw(r, ax + aw + U(14), YI(i) - px * 5 / 8, s, px, 110, false);
        }
    int c0 = d->start_h * 60 + d->start_m;
    for (int t = (ts - c0 % ts) % ts; t <= t1; t += ts) {
        int xx = XO(t);
        raster_vline(r, xx, ay, ay + ah - 1, 200);
        int m = c0 + t;
        snprintf(s, sizeof s, "%02d:%02d", (m / 60) % 24, m % 60);
        rtext_draw_center(r, xx, ay + ah + U(12), s, px, 40, false);
    }
    for (int t = 0; t < U(3); t++) raster_rect(r, ax - t, ay - t, aw + 2 * t, ah + 2 * t, 0);
    rtext_draw(r, ax - U(110), ay - U(62), "U, В", px, 0, true);
    if (d->i) rtext_draw_right(r, ax + aw + U(110), ay - U(62), "I, А", px, 110, true);
    rtext_draw_center(r, ax + aw / 2, ay + ah + U(60), "час", px, 40, false);

    /* stop-voltage line */
    if (d->v_stop > 0) dashed_hline(r, ax, ax + aw, YV(d->v_stop), U(3), U(18), 90);

    /* curves */
    if (d->i)
        for (int k = 1; k < d->n; k++)
            thick_line(r, XO(d->t_min[k - 1]), YI(d->i[k - 1]), XO(d->t_min[k]), YI(d->i[k]), U(4), 150);
    for (int k = 1; k < d->n; k++)
        thick_line(r, XO(d->t_min[k - 1]), YV(d->v[k - 1]), XO(d->t_min[k]), YV(d->v[k]), U(5), 0);

    /* legend */
    int lx = ax + aw / 2 - U(700), ly = y + U(4);
    thick_line(r, lx, ly + U(24), lx + U(70), ly + U(24), U(5), 0);
    lx = rtext_draw(r, lx + U(86), ly, "напруга батареї", px, 0, false) + U(60);
    if (d->i) {
        thick_line(r, lx, ly + U(24), lx + U(70), ly + U(24), U(4), 150);
        lx = rtext_draw(r, lx + U(86), ly, "струм розряду", px, 0, false) + U(60);
    }
    if (d->v_stop > 0) {
        dashed_hline(r, lx, lx + U(70), ly + U(23), U(3), U(14), 90);
        snprintf(s, sizeof s, "кінець розряду %.1f В", d->v_stop);
        for (char *c = s; *c; c++) if (*c == '.') *c = ',';
        rtext_draw(r, lx + U(86), ly, s, px, 0, false);
    }
#undef XO
#undef YV
#undef YI
}

/* imbalance (max - min cell) over the test, aligned with diagram() */
static void delta_diagram(raster_t *r, int x, int y, int w, int h, const doc_protocol_t *d)
{
    const int px = U(34);
    const int ax = x + U(120), ay = y + U(70);
    const int aw = w - U(120) - (d->i ? U(120) : U(30)), ah = h - U(70) - U(110);
    float dmax = 1;
    for (int k = 0; k < d->dn; k++) if (d->dmv[k] > dmax) dmax = d->dmv[k];
    float ds = nice_step(dmax * 1.15f + 1, 6);
    float d1 = ceilf((dmax * 1.15f + 1) / ds) * ds;
    int t1 = axis_tmax(d), ts = nice_min_step(t1);

#define XO(t) (ax + (int)((t) * (aw - 1) / t1))
#define YD(v) (ay + ah - 1 - (int)((v) * (ah - 1) / d1))
    char s[64];
    for (float v = 0; v <= d1 + ds * 0.01f; v += ds) {
        int yy = YD(v);
        raster_hline(r, ax, ax + aw - 1, yy, 200);
        snprintf(s, sizeof s, "%.0f", v);
        rtext_draw_right(r, ax - U(14), yy - px * 5 / 8, s, px, 40, false);
    }
    int c0 = d->start_h * 60 + d->start_m;
    for (int t = (ts - c0 % ts) % ts; t <= t1; t += ts) {
        int xx = XO((float)t);
        raster_vline(r, xx, ay, ay + ah - 1, 200);
        int m = c0 + t;
        snprintf(s, sizeof s, "%02d:%02d", (m / 60) % 24, m % 60);
        rtext_draw_center(r, xx, ay + ah + U(12), s, px, 40, false);
    }
    for (int t = 0; t < U(3); t++) raster_rect(r, ax - t, ay - t, aw + 2 * t, ah + 2 * t, 0);
    rtext_draw(r, ax - U(110), ay - U(62), "Δ, мВ", px, 0, true);
    rtext_draw_center(r, ax + aw / 2, ay + ah + U(60), "час", px, 40, false);
    for (int k = 1; k < d->dn; k++)
        thick_line(r, XO(d->dt_min[k - 1]), YD(d->dmv[k - 1]), XO(d->dt_min[k]), YD(d->dmv[k]), U(4), 0);
    int lx = ax + aw / 2 - U(500), ly = y + U(4);
    thick_line(r, lx, ly + U(24), lx + U(70), ly + U(24), U(4), 0);
    rtext_draw(r, lx + U(86), ly, "розбаланс комірок (найвища - найнижча), мВ", px, 0, false);
#undef XO
#undef YD
}

/* ------------------------------------------------------------ page */

void doc_protocol_render(const doc_protocol_t *d, raster_t *r)
{
    s_k = (float)r->w / DOC_A4_W;
    const int L = U(250), R = r->w - U(180), W = R - L;
    const int body = U(46), lh = U(66);
    char s[256], s2[192];
    int y = U(170);

    /* company header */
    rtext_draw(r, L, y, d->company, U(46), 0, true);           y += U(66);
    snprintf(s, sizeof s, "Код: %s", d->company_code);
    rtext_draw(r, L, y, s, U(40), 40, false);                  y += U(56);
    snprintf(s, sizeof s, "Адреса: %s", d->address[0]);
    rtext_draw(r, L, y, s, U(40), 40, false);                  y += U(56);
    rtext_draw(r, L, y, d->address[1], U(40), 40, false);      y += U(64);
    for (int t = 0; t < U(4); t++) raster_hline(r, L, R, y + t, 0);
    y += U(110);

    /* title */
    snprintf(s, sizeof s, "ПРОТОКОЛ ВИПРОБУВАНЬ №%s", d->number);
    rtext_draw_center(r, (L + R) / 2, y, s, U(62), 0, true);   y += U(92);
    if (d->in_progress) {
        rtext_draw_center(r, (L + R) / 2, y, "(проміжний — тест ще триває)", U(38), 90, false);
        y += U(60);
    }
    y += U(40);

    char vb[24] = "___", tb[24];
    if (d->v_before > 0) snprintf(vb, sizeof vb, "%.1f", d->v_before);
    snprintf(tb, sizeof tb, "%.0f", d->temp_start > -100 ? d->temp_start : 20.0f);
    for (char *p = vb; *p; p++) if (*p == '.') *p = ',';

    if (!d->detailed) {
        /* the simple protocol (the company's Word template) */
        snprintf(s, sizeof s, " SN:%s%s%s, напруга батареї до тестування %sВ, температура на початок тесту %s°C",
                 d->serial, d->battery_desc ? ", " : "", d->battery_desc ? d->battery_desc : "", vb, tb);
        rtext_run_t q1[] = { { "Дані про батарею:", true }, { s, false } };
        y = rtext_para(r, L, y, W, q1, 2, body, lh, 0) + U(14);
        snprintf(s2, sizeof s2, " %s", d->equipment);
        rtext_run_t q2[] = { { "Обладнання для тестування:", true }, { s2, false } };
        y = rtext_para(r, L, y, W, q2, 2, body, lh, 0) + U(14);
        rtext_run_t q3[] = { { "Мета випробувань:", true },
                             { " Визначення електричної ємності акумуляторної батареї", false } };
        y = rtext_para(r, L, y, W, q3, 2, body, lh, 0) + U(14);
        snprintf(s2, sizeof s2, " Розряд постійним навантаженням струмом %dА", d->discharge_a);
        rtext_run_t q4[] = { { "Метод випробувань:", true }, { s2, false } };
        y = rtext_para(r, L, y, W, q4, 2, body, lh, 0) + U(40);

        rtext_draw_center(r, (L + R) / 2, y, "Висновки", U(52), 0, true);
        y += U(84);
        snprintf(s2, sizeof s2, " %.0f А·год", d->capacity_ah);
        rtext_run_t q5[] = { { d->in_progress ? "Знята ємність на момент друку:" : "Встановлена ємність:", true },
                             { s2, false } };
        y = rtext_para(r, L, y, W, q5, 2, body, lh, 0) + U(14);
        char dur0[64];
        doc_duration_ua(d->discharge_min, dur0, sizeof dur0);
        snprintf(s2, sizeof s2, " %s", dur0);
        rtext_run_t q6[] = { { "Час розряджання:", true }, { s2, false } };
        y = rtext_para(r, L, y, W, q6, 2, body, lh, 0) + U(14);
        if (d->stop_reason) {
            snprintf(s2, sizeof s2, " %s", d->stop_reason);
            rtext_run_t q7[] = { { "Причина зупинки тесту:", true }, { s2, false } };
            y = rtext_para(r, L, y, W, q7, 2, body, lh, 0) + U(14);
        }
        y += U(50);
    } else {
    /* object, method, conditions */
    snprintf(s, sizeof s, " акумуляторна батарея SN %s%s%s", d->serial,
             d->battery_desc ? ", " : "", d->battery_desc ? d->battery_desc : "");
    rtext_run_t p1[] = { { "Об'єкт випробувань:", true }, { s, false } };
    y = rtext_para(r, L, y, W, p1, 2, body, lh, 0) + U(10);
    snprintf(s2, sizeof s2, " %s", d->equipment);
    rtext_run_t p2[] = { { "Обладнання для тестування:", true }, { s2, false } };
    y = rtext_para(r, L, y, W, p2, 2, body, lh, 0) + U(10);
    rtext_run_t p3[] = { { "Мета випробувань:", true },
                         { " визначення електричної ємності акумуляторної батареї", false } };
    y = rtext_para(r, L, y, W, p3, 2, body, lh, 0) + U(10);
    snprintf(s2, sizeof s2, " розряд постійним струмом %d А", d->discharge_a);
    rtext_run_t p4[] = { { "Метод випробувань:", true }, { s2, false } };
    y = rtext_para(r, L, y, W, p4, 2, body, lh, 0) + U(10);
    snprintf(s, sizeof s, " початок %s, температура батареї %s °C, напруга до тестування %s В",
             d->start_str[0] ? d->start_str : "—", tb, vb);
    rtext_run_t p5[] = { { "Умови:", true }, { s, false } };
    y = rtext_para(r, L, y, W, p5, 2, body, lh, 0) + U(40);

    /* results */
    if (d->info_n > 0) {
        rtext_draw_center(r, (L + R) / 2, y, "Результати", U(50), 0, true);
        y += U(80);
        y = info_table(r, L, y, W, d->info, d->info_n) + U(50);
    }

    /* conclusion: verdict and what influenced it */
    if (d->verdict) {
        rtext_draw_center(r, (L + R) / 2, y, "Висновок", U(50), 0, true);
        y += U(80);
        rtext_run_t pv[] = { { d->verdict, true } };
        y = rtext_para(r, L, y, W, pv, 1, body, lh, 0) + U(12);
        const int fpx = U(40), flh = U(56);
        for (int i = 0; i < d->findings_n; i++) {
            const doc_finding_t *f = &d->findings[i];
            rtext_draw(r, L + U(10), y, f->level == DOC_F_CAUSE ? "!" : "•", fpx, 0, true);
            rtext_run_t pf[] = { { f->text, f->level == DOC_F_CAUSE } };
            y = rtext_para(r, L + U(50), y, W - U(50), pf, 1, fpx, flh, 0) + U(6);
        }
        y += U(40);
    }
    }   /* detailed */

    /* figure 2 */
    int sig_h = U(230);
    int gh = r->h - U(150) - sig_h - y - U(70);
    if (d->n >= 2 && gh > U(450)) {
        if (d->detailed && d->dn >= 2 && gh > U(900)) {   /* discharge on top, imbalance below */
            int g1 = gh * 58 / 100;
            diagram(r, L, y, W, g1, d);
            delta_diagram(r, L, y + g1 + U(10), W, gh - g1 - U(10), d);
        } else {
            diagram(r, L, y, W, gh, d);
        }
        y += gh + U(30);
    } else {
        rtext_draw_center(r, (L + R) / 2, y + U(40), "Розрядний графік: немає даних (тестер його вже перезаписав)", U(40), 90, false);
        y += U(130);
    }

    /* signature */
    y = r->h - U(150) - sig_h + U(60);
    rtext_draw(r, L, y, d->signer_title, body, 0, false);
    y += lh;
    rtext_draw(r, L, y, d->signer_name, body, 0, false);
    rtext_draw_right(r, R, y, d->date, body, 0, false);
    raster_hline(r, L + U(700), R - U(400), y + body, 0);
}

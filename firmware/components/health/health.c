#include "health.h"

#include <math.h>
#include <string.h>

typedef struct {
    float delta_y, delta_r;     /* imbalance, mV */
    float grow_y, grow_r;       /* growth since the start, mV */
    float cell_y, cell_r;       /* lowest cell, V (below = bad) */
    float knee;                 /* lowest cell below this: end of discharge, imbalance at most WARN */
    float scale_lo, scale_hi;   /* lowest-cell gauge */
} chem_lim_t;

/* lowest-cell limits sit below a normal end of discharge (the tester stops
 * NMC at ~2.7 V/cell, LFP at ~2.5 V/cell): only a cell diving past it trips */
static const chem_lim_t NMC = { 25, 40, 15, 30, 2.90f, 2.60f, 3.30f, 2.4f, 4.2f };
static const chem_lim_t LFP = { 60, 100, 30, 60, 2.60f, 2.40f, 3.00f, 2.2f, 3.65f };

#define CURR_Y 5.0f      /* % off the set current */
#define CURR_R 10.0f
#define VDROP_Y 1.0f     /* V between tester and BMS */
#define VDROP_R 2.0f
#define TEMP_Y 45.0f
#define TEMP_R 55.0f
#define TEMP_RISE_Y 15.0f
#define MOS_Y 70.0f
#define MOS_R 85.0f

chem_t health_guess_chem(uint8_t rated_v, int cells, uint16_t cell_max_mv)
{
    if (rated_v && cells > 0) return (float)rated_v / cells >= 3.45f ? CHEM_NMC : CHEM_LFP;
    if (cell_max_mv) return cell_max_mv > 3650 ? CHEM_NMC : CHEM_LFP;
    return CHEM_NMC;
}

/* priority among equal levels: lower = shown first */
static int prio(hreason_t r)
{
    switch (r) {
    case HR_ALARM:      return 0;
    case HR_CELL_LOW:   return 1;
    case HR_DELTA_GROW: return 2;
    case HR_DELTA:      return 3;
    case HR_CURRENT:    return 4;
    case HR_TEMP:       return 5;
    case HR_MOS:        return 6;
    case HR_VDROP:      return 7;
    case HR_NO_BMS:     return 8;
    default:            return 9;
    }
}

static void cand(health_out_t *o, hlevel_t lv, hreason_t r, int cell, float a, float b)
{
    if (lv <= HL_OK) return;
    if (lv > o->level || (lv == o->level && prio(r) < prio(o->reason))) {
        o->level = lv; o->reason = r; o->cell = cell; o->a = a; o->b = b;
    }
}

static hlevel_t above(float v, float y, float r) { return v >= r ? HL_STOP : v >= y ? HL_WARN : HL_OK; }
static hlevel_t below(float v, float y, float r) { return v < r ? HL_STOP : v < y ? HL_WARN : HL_OK; }
static hlevel_t hmax(hlevel_t a, hlevel_t b) { return a > b ? a : b; }

static void zones3(hgauge_t *g, float lo, float hi, float e1, float e2, hlevel_t z0, hlevel_t z1, hlevel_t z2)
{
    g->lo = lo; g->hi = hi; g->nz = 3;
    g->edge[0] = e1; g->edge[1] = e2; g->edge[2] = hi;
    g->zl[0] = z0; g->zl[1] = z1; g->zl[2] = z2;
}

void health_eval(const health_in_t *in, health_out_t *o)
{
    memset(o, 0, sizeof *o);
    o->chem = in->chem != CHEM_AUTO ? in->chem
            : health_guess_chem(in->rated_v, in->bms ? in->cells : 0, in->bms ? in->cell_max_mv : 0);
    const chem_lim_t *L = o->chem == CHEM_LFP ? &LFP : &NMC;
    o->level = in->discharging ? HL_OK : HL_NONE;
    o->reason = in->discharging ? HR_OK : HR_IDLE;
    const bool judge = in->discharging;

    /* imbalance */
    hgauge_t *g = &o->g[HG_DELTA];
    zones3(g, 0, L->delta_r * 1.25f, L->delta_y, L->delta_r, HL_OK, HL_WARN, HL_STOP);
    if (in->bms && in->cells >= 2) {
        float d = (float)(in->cell_max_mv - in->cell_min_mv);
        float vmin = in->cell_min_mv / 1000.0f;
        bool knee = vmin < L->knee;
        hlevel_t la = above(d, L->delta_y, L->delta_r);
        hlevel_t lg = HL_OK;
        float grow = 0;
        if (in->has_base) {
            grow = d - in->delta0_mv;
            lg = above(grow, L->grow_y, L->grow_r);
        }
        if (knee) { if (la > HL_WARN) la = HL_WARN; if (lg > HL_WARN) lg = HL_WARN; }
        g->valid = true; g->val = d; g->level = hmax(la, lg);
        if (judge) {
            cand(o, lg, HR_DELTA_GROW, in->cell_min_idx + 1, (float)in->delta0_mv, d);
            cand(o, la, HR_DELTA, in->cell_min_idx + 1, d, grow);
        }
    }

    /* lowest cell */
    g = &o->g[HG_CELL];
    zones3(g, L->scale_lo, L->scale_hi, L->cell_r, L->cell_y, HL_STOP, HL_WARN, HL_OK);
    if (in->bms && in->cells >= 1) {
        float v = in->cell_min_mv / 1000.0f;
        g->valid = true; g->val = v; g->level = below(v, L->cell_y, L->cell_r);
        if (judge) cand(o, g->level, HR_CELL_LOW, in->cell_min_idx + 1, v, 0);
    }

    /* tester current vs the programmed one (% deviation) */
    g = &o->g[HG_CURR];
    g->lo = -2 * CURR_R; g->hi = 2 * CURR_R; g->nz = 5;
    g->edge[0] = -CURR_R; g->edge[1] = -CURR_Y; g->edge[2] = CURR_Y; g->edge[3] = CURR_R; g->edge[4] = g->hi;
    g->zl[0] = HL_STOP; g->zl[1] = HL_WARN; g->zl[2] = HL_OK; g->zl[3] = HL_WARN; g->zl[4] = HL_STOP;
    if (in->i_set > 0 && in->discharging && in->elapsed_s >= 60) {
        float dev = (in->i_tester - in->i_set) * 100.0f / in->i_set;
        g->valid = true;
        g->val = dev < g->lo ? g->lo : dev > g->hi ? g->hi : dev;
        g->level = above(fabsf(dev), CURR_Y, CURR_R);
        cand(o, g->level, HR_CURRENT, 0, in->i_tester, in->i_set);
    }

    /* battery temperature (absolute, and the rise since the start) */
    g = &o->g[HG_TEMP];
    zones3(g, 0, 70, TEMP_Y, TEMP_R, HL_OK, HL_WARN, HL_STOP);
    if (in->bms && in->has_temp) {
        float rise = in->has_base ? in->temp_bat - in->temp0 : 0;
        hlevel_t l = above(in->temp_bat, TEMP_Y, TEMP_R);
        if (rise > TEMP_RISE_Y && l < HL_WARN) l = HL_WARN;
        g->valid = true; g->val = in->temp_bat; g->level = l;
        if (judge) cand(o, l, HR_TEMP, 0, in->temp_bat, rise);
    }

    if (!judge) return;
    if (in->alarms > 0) cand(o, HL_STOP, HR_ALARM, 0, (float)in->alarms, (float)in->alarm_id);
    if (in->bms && in->has_mos) cand(o, above(in->temp_mos, MOS_Y, MOS_R), HR_MOS, 0, in->temp_mos, 0);
    if (in->bms && in->v_bms > 0 && in->v_tester > 0) {
        float dv = fabsf(in->v_tester - in->v_bms);
        cand(o, above(dv, VDROP_Y, VDROP_R), HR_VDROP, 0, dv, 0);
    }
    if (!in->bms) cand(o, HL_WARN, HR_NO_BMS, 0, 0, 0);
}

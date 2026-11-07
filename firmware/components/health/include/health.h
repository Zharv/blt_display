/*
 * Battery status during a discharge test: a traffic-light verdict (OK /
 * look closer / stop the test) with its main reason, and four gauges
 * (cell imbalance, lowest cell, tester current, battery temperature).
 * Thresholds depend on the chemistry (NMC / LFP). Pure C, host-tested.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CHEM_AUTO = 0, CHEM_NMC, CHEM_LFP } chem_t;
typedef enum { HL_NONE = 0, HL_OK, HL_WARN, HL_STOP } hlevel_t;

typedef enum {
    HR_IDLE = 0,        /* no test running: nothing to judge */
    HR_OK,
    HR_NO_BMS,          /* discharging without BMS data */
    HR_ALARM,           /* tester alarm active               a = count, b = first id */
    HR_CELL_LOW,        /* lowest cell under the limit       a = V, cell */
    HR_DELTA,           /* imbalance too high                a = mV, cell */
    HR_DELTA_GROW,      /* imbalance grew since the start    a = from mV, b = to mV, cell */
    HR_CURRENT,         /* tester current off the set value  a = A, b = set A */
    HR_VDROP,           /* tester V - BMS V (cables/contacts) a = V */
    HR_TEMP,            /* battery temperature               a = degC, b = rise */
    HR_MOS,             /* BMS MOSFET temperature            a = degC */
} hreason_t;

typedef struct {
    bool     discharging;     /* tester in DISCHARGE (pauses are not judged) */
    uint32_t elapsed_s;       /* discharge time */
    int      alarms;          /* active tester alarms */
    int      alarm_id;        /* the first of them (1..48), 0 = none */
    float    v_tester;        /* V */
    float    i_tester;        /* A, magnitude */
    float    i_set;           /* A, programmed discharge current (0 = unknown) */
    uint8_t  rated_v;         /* tester rated voltage (0 = unknown) */

    bool     bms;             /* fresh BMS data */
    float    v_bms;
    int      cells;
    uint16_t cell_min_mv, cell_max_mv;
    int      cell_min_idx;    /* 0-based */
    bool     has_temp;
    float    temp_bat;        /* hottest battery probe, degC */
    bool     has_mos;
    float    temp_mos;

    /* values at the start of the test (has_base false = not known yet) */
    bool     has_base;
    uint16_t delta0_mv;
    float    temp0;

    chem_t   chem;            /* CHEM_AUTO: from rated V per cell, else cell voltages */
} health_in_t;

#define HG_DELTA 0
#define HG_CELL  1
#define HG_CURR  2
#define HG_TEMP  3
#define HG_N     4

typedef struct {
    bool     valid;
    hlevel_t level;
    float    val;              /* marker value */
    float    lo, hi;           /* scale */
    int      nz;               /* zones: [edge[k-1], edge[k]) has level zl[k]; edge[-1]=lo, edge[nz-1]=hi */
    float    edge[5];
    hlevel_t zl[5];
} hgauge_t;

typedef struct {
    hlevel_t  level;
    hreason_t reason;
    int       cell;            /* 1-based, 0 = none */
    float     a, b;
    chem_t    chem;            /* chemistry used */
    hgauge_t  g[HG_N];
} health_out_t;

/* Evaluate one snapshot. */
void health_eval(const health_in_t *in, health_out_t *out);

/* Chemistry from rated voltage / cell count (0 if unknown) or cell voltages. */
chem_t health_guess_chem(uint8_t rated_v, int cells, uint16_t cell_max_mv);

#ifdef __cplusplus
}
#endif

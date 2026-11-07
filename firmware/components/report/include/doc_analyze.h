/*
 * What the protocol concludes from a test: the capacity verdict and the
 * factors that explain it (state of charge before the test, cell balance,
 * temperature, how the discharge ended). Pure C, host-tested; Ukrainian text.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DOC_STOP_RUNNING = 0,  /* still discharging (interim protocol) */
    DOC_STOP_VSTOP,        /* the tester reached its end-of-discharge voltage */
    DOC_STOP_BMS,          /* the battery's BMS opened the load (tester alarm 41) */
    DOC_STOP_ALARM,        /* another tester alarm */
    DOC_STOP_TIME,         /* programmed discharge time limit */
    DOC_STOP_MANUAL,       /* stopped by the operator / unknown */
} doc_stop_t;

typedef struct {
    float ah, rated_ah;        /* discharged / rated capacity */
    int   cells;               /* 0 = unknown */
    bool  lfp;                 /* chemistry: LFP, else NMC */
    float v_before;            /* V at the start, 0 = unknown */
    bool  v_before_loaded;     /* that voltage was measured under load */
    bool  temp_known;          /* temp_start measured (else 20 C assumed) */
    float temp_start;
    bool  temp_max_known;
    float temp_max;
    int   d_start, d_end;      /* imbalance mV at the start / end, -1 = unknown */
    int   low_cell;            /* lowest cell at the end, 1-based, 0 = unknown */
    doc_stop_t stop;
    float v_end;               /* battery voltage where the discharge ended */
    float v_stop;              /* unused (the tester sets it itself) */
    const char *alarm_name;    /* DOC_STOP_ALARM */
} doc_facts_t;

typedef enum { DOC_F_OK = 0, DOC_F_NOTE, DOC_F_CAUSE } doc_flevel_t;

typedef struct {
    doc_flevel_t level;        /* CAUSE = lowers the measured capacity */
    char         text[400];      /* UTF-8 */
} doc_finding_t;

#define DOC_FINDINGS_MAX 6

/* Fills verdict (one sentence) and up to max findings; returns their count. */
int doc_analyze(const doc_facts_t *f, char *verdict, int vn, doc_finding_t *out, int max);

#ifdef __cplusplus
}
#endif

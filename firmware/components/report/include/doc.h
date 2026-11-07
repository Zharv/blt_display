/*
 * Printed documents (Ukrainian), laid out on an A4 page raster at 300 dpi
 * (2480 x 3508; other sizes are scaled). Text uses rtext (call rtext_init first).
 * Pure C, host-testable: tests/host can render the same page to a PNG.
 */
#pragma once

#include "raster.h"
#include "doc_analyze.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOC_A4_W 2480
#define DOC_A4_H 3508

typedef struct {
    const char *label;
    char value[72];
} doc_kv_t;

/* "ПРОТОКОЛ ВИПРОБУВАНЬ" */
typedef struct {
    /* fixed texts (company, signer, equipment) */
    const char *company;          /* ТОВ "ЗАВОД АКУМУЛЯТОРНИХ СИСТЕМ ЖАРВ" */
    const char *company_code;     /* 45745311 */
    const char *address[2];
    const char *signer_title;     /* Директор ТОВ "ЗАС ЖАРВ" */
    const char *signer_name;      /* Жалінський Роман */
    const char *equipment;        /* Електронне навантаження BLT160 */

    char     number[32];          /* 18092026-1 */
    char     date[16];            /* 18.09.2026 (signature date) */
    char     serial[24];
    float    v_before;            /* <= 0: not known (left blank) */
    float    temp_start;          /* < -100: not known (printed as 20) */
    int      discharge_a;
    float    capacity_ah;
    uint32_t discharge_min;
    bool     in_progress;         /* test still running: marked as an interim protocol */
    const char *stop_reason;      /* unused (kept for the host preview) */
    const char *battery_desc;     /* "Li-NMC, 20S, номінальна напруга 74 В, номінальна ємність 300 А·год" */
    char        start_str[24];    /* "06.11.2026 12:53" */
    /* conclusion: verdict + what influenced the result (doc_analyze) */
    const char          *verdict;
    const doc_finding_t *findings;
    int                  findings_n;

    /* Малюнок №1: test results table */
    const doc_kv_t *info;
    int             info_n;

    /* Малюнок №2: discharge diagram from the tester recorder */
    const uint16_t *t_min;        /* minutes since discharge start */
    const float    *v;            /* V */
    const float    *i;            /* A (magnitude); NULL = no current curve */
    int             n;
    int             start_h, start_m;   /* clock at t = 0 (axis labels) */
    float           v_stop;       /* discharge stop voltage line, <= 0: none */

    /* cell imbalance during the test (max - min cell, from the panel's BMS log),
     * drawn under the discharge diagram on the same time axis; dn < 2 = none */
    const float    *dt_min;       /* minutes since discharge start */
    const float    *dmv;          /* mV */
    int             dn;
} doc_protocol_t;

void doc_protocol_render(const doc_protocol_t *d, raster_t *r);

/* "3 години 3 хвилини" (Ukrainian plural forms) */
void doc_duration_ua(uint32_t minutes, char *out, int n);

#ifdef __cplusplus
}
#endif

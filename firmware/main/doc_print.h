/*
 * Test protocol ("ПРОТОКОЛ ВИПРОБУВАНЬ") for the tester's current test:
 * downloads the cycle + voltage diagram from the tester (read-only), adds the
 * BMS snapshot from the panel, renders an A4 page at 300 dpi and prints it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t cycle;             /* tester record index, BLT_ARCH_CURRENT (0xFFFF) = running/last test */
    bool     bms;               /* BMS data valid */
    char     bms_name[32];
    uint8_t  cells;
    uint16_t cell_min_mv, cell_max_mv;
    uint16_t imbalance_mv;
    uint16_t imbalance_max_mv;  /* worst during the recorded history, 0 = n/a */
    uint8_t  soc_pct;
    float    temp_start;        /* battery temperature at the start, < -100 = unknown */
    float    live_ah;           /* panel's interpolated Ah (0 = use the tester's) */
    uint8_t  chem;              /* chem_t setting: 0 auto, 1 NMC, 2 LFP */
} doc_extra_t;

/* Start in a background task. print=false only logs the page data (DOC* lines)
 * for the host preview. done (may be NULL) is called from that task. */
bool doc_print_protocol(const doc_extra_t *x, bool print, void (*done)(esp_err_t));

/* dev console: print a test page (no tester data needed) */
void doc_print_test_page(void);

#ifdef __cplusplus
}
#endif

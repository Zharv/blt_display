/*
 * Download a test from the tester's recorder (docs/BLT_RECORDER.md), READ-ONLY:
 * RI for the live block (= the current cycle record), RD for the tester serial
 * and the graph pages. Nothing is written and no RE/other command is sent.
 */
#pragma once

#include "blt_rec.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLT_ARCH_MAX 5000      /* 250 pages x 20 samples */

typedef struct {
    blt_rec_t rec;             /* the cycle (live block of the running / last test) */
    char      tester_sn[17];
    int       n;               /* samples of this cycle, oldest first */
    int32_t   t0_abs;          /* blt_minutes() of sample 0 */
    uint8_t   t0_h, t0_mi, t0_d, t0_mo, t0_y;
    uint16_t  t_min[BLT_ARCH_MAX];
    float     v[BLT_ARCH_MAX];
    float     i[BLT_ARCH_MAX];  /* A, magnitude */
} blt_test_t;

/* Fetch the current cycle with its voltage diagram. Blocks (one 64-byte frame
 * at a time, interleaved with the live poll). t should be in PSRAM. */
esp_err_t blt_archive_fetch_current(blt_test_t *t);

#ifdef __cplusplus
}
#endif

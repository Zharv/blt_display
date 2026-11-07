/*
 * Tests from the tester's recorder (docs/BLT_RECORDER.md), READ-ONLY:
 * RI for the live block (= the current cycle record), RD for the tester
 * serial, the cycle records and the graph pages. Nothing is written and no
 * RE/other command is sent.
 */
#pragma once

#include "blt_rec.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLT_ARCH_MAX 5000      /* 250 pages x 20 samples */

typedef struct {
    blt_rec_t rec;             /* the cycle record */
    bool      current;         /* it is the tester's current cycle */
    char      tester_sn[17];
    int       n;               /* samples of this cycle, oldest first */
    int32_t   t0_abs;          /* blt_minutes() of sample 0 */
    uint8_t   t0_h, t0_mi, t0_d, t0_mo, t0_y;
    uint16_t  t_min[BLT_ARCH_MAX];
    float     v[BLT_ARCH_MAX];
    float     i[BLT_ARCH_MAX];  /* A, magnitude */
} blt_test_t;

/* one recorded cycle that has a discharge result */
typedef struct {
    uint16_t  n;               /* absolute record index (= rec.pr_cycle) */
    bool      current;         /* the tester's current cycle (may be running) */
    blt_rec_t rec;
} blt_arch_entry_t;

#define BLT_ARCH_CURRENT 0xFFFF

/* Fetch cycle n (BLT_ARCH_CURRENT = the current one) with its voltage diagram.
 * Blocks; t should be in PSRAM. */
esp_err_t blt_archive_fetch(uint16_t n, blt_test_t *t);
static inline esp_err_t blt_archive_fetch_current(blt_test_t *t) { return blt_archive_fetch(BLT_ARCH_CURRENT, t); }

/* Cycle records that hold a discharge result (start date, time and Ah > 0),
 * newest first. Finished records already in cache[0..*cache_n) are not read
 * again; the list is rebuilt into cache. progress(done, total) may be NULL. */
esp_err_t blt_archive_list(blt_arch_entry_t *cache, int *cache_n, int max,
                           void (*progress)(int done, int total));

/* the record has a discharge result worth a protocol */
bool blt_rec_has_result(const blt_rec_t *r);

#ifdef __cplusplus
}
#endif

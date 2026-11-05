/*
 * Measurement history for one test, kept in PSRAM. When full it is thinned
 * by half (and the sampling stride doubled), so the whole test always fits.
 * Accessed only from the LVGL context (timer callbacks, UI handlers).
 */
#pragma once

#include <stdint.h>
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HIST_MAX 720

typedef struct {
    uint32_t t_s;          /* test time */
    float    v_t, i_t;     /* tester */
    float    v_b, i_b;     /* BMS pack */
    uint8_t  n;            /* cells recorded */
    uint16_t cell[UI_MAX_CELLS];
} hist_pt_t;

void hist_init(void);
void hist_reset(void);
void hist_add(const online_sample_t *s);
int  hist_count(void);
const hist_pt_t *hist_at(int i);

#ifdef __cplusplus
}
#endif

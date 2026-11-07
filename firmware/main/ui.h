/*
 * OnLine screen: live test view (tester + BMS) with a discharge / imbalance
 * chart drawn from the measurement history (hist.h).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_MAX_CELLS 32

typedef enum { TS_IDLE = 0, TS_DISCHARGE, TS_REST, TS_DONE } test_state_t;

typedef struct {
    /* tester */
    bool     tester_live;   /* false while the tester side is simulated */
    bool     tester_usb;    /* tester enumerated on USB (link up, values not read yet) */
    char     battery_id[24];/* battery serial set on the tester, e.g. "202611070001" */
    uint8_t  state;         /* test_state_t */
    float    voltage_v;
    float    current_a;     /* negative = discharge */
    float    capacity_ah;
    float    energy_wh;
    uint32_t elapsed_s;
    float    temp_c;

    /* BMS */
    bool     bms_present;
    bool     bms_live;      /* real BLE data (vs simulated) */
    char     bms_name[32];
    uint8_t  soc_pct;
    float    bms_pack_v;
    float    bms_current_a;
    uint8_t  cell_count;
    uint16_t cell_mv[UI_MAX_CELLS];
    uint16_t cell_min_mv, cell_max_mv, imbalance_mv;
    uint8_t  cell_min_idx, cell_max_idx;   /* 0-based */
    uint32_t balance_bits;
    uint8_t  temp_count;
    int16_t  temp_dc[4];    /* 0.1 degC; JK: T1, T2, MOS */
} online_sample_t;

/* Build the OnLine screen (its own screen object). Call with the display locked. */
lv_obj_t *ui_online_create(void);

/* Readouts + cell grid. */
void ui_online_update(const online_sample_t *s);

/* Redraw the chart from the history (cheap enough every ~1 s). */
void ui_online_chart_refresh(void);

/* 0 = discharge (U/I), 1 = cell imbalance. */
void ui_online_set_chart_mode(int mode);

/* Highlight cell i (0-based) on the imbalance plot; same i again or -1 clears. */
void ui_online_select_cell(int i);

/* Fill min/max indices and the imbalance from cell_mv[]. */
void ui_sample_finish_cells(online_sample_t *s);

#ifdef __cplusplus
}
#endif

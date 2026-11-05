/*
 * OnLine screen: live test view (tester + BMS) with a discharge graph.
 * Fed by a sample struct so the data source (synthetic now, real tester/BMS
 * later) stays separate from the UI.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_MAX_CELLS 24

typedef struct {
    /* tester */
    float    voltage_v;     /* pack voltage under test            */
    float    current_a;     /* negative = discharge               */
    float    capacity_ah;   /* capacity moved so far              */
    uint32_t elapsed_s;     /* test elapsed time                  */
    float    temp_c;        /* tester temperature                 */
    const char *state;      /* "Discharge" / "Rest" / "Done" ...  */

    /* BMS (over BLE) */
    bool     bms_present;
    uint8_t  soc_pct;
    float    bms_pack_v;
    uint8_t  cell_count;
    uint16_t cell_mv[UI_MAX_CELLS];
    uint16_t cell_min_mv;
    uint16_t cell_max_mv;
    uint16_t imbalance_mv;
    float    bms_temp_c;
} online_sample_t;

/* Build the OnLine screen (its own screen object). Call with the display locked. */
lv_obj_t *ui_online_create(void);

/* Push a new sample: updates readouts, cell bars and advances the chart. */
void ui_online_update(const online_sample_t *s);

/* Register the Print-button handler (called from the LVGL context on tap). */
void ui_online_set_print_cb(void (*cb)(void));

/* Register the Settings-button handler. */
void ui_online_set_settings_cb(void (*cb)(void));

/* Show a short status line (e.g. print result). */
void ui_online_set_status(const char *text);

#ifdef __cplusplus
}
#endif

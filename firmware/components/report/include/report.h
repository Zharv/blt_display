/*
 * Battery-test report layout: renders a summary + discharge graph + BMS section
 * into a grayscale raster. Pure C; the same raster feeds the on-screen preview
 * and the printer (via urf.h). Resolution-independent: scales to the raster size.
 */
#pragma once

#include "raster.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *title;          /* NULL -> "BATTERY TEST REPORT" */
    const char *date;           /* "2026-11-05 14:30"           */
    const char *operator_name;
    const char *battery_id;
    float nominal_v;
    float nominal_ah;
    float discharge_a;

    /* result */
    float    discharged_ah;
    uint32_t duration_s;
    float    min_voltage_v;
    float    max_temp_c;
    float    cutoff_v;

    /* discharge curve (voltage + current magnitude vs sample index) */
    const float *curve_v;
    const float *curve_i;
    int    curve_n;
    float  curve_vmin, curve_vmax;
    float  curve_imax;

    /* BMS snapshot at end of test (optional) */
    bool            bms_present;
    const uint16_t *cell_mv;
    int             cell_count;
    uint16_t        cell_min_mv, cell_max_mv, imbalance_mv;
    uint8_t         soc_pct;
} report_data_t;

/* Render into r (any size). Caller clears r to white first (raster_clear 255). */
void report_render(const report_data_t *d, raster_t *r);

#ifdef __cplusplus
}
#endif

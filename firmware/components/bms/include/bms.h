/*
 * Vendor-neutral BMS reading. Each vendor driver (JBD/JK/Daly) decodes its own
 * wire format into this normalized structure so the UI and test-session logic
 * stay vendor-independent. Plain C, host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BMS_MAX_CELLS 32
#define BMS_MAX_TEMPS 8

typedef enum {
    BMS_VENDOR_JBD = 0,
    BMS_VENDOR_JK,
    BMS_VENDOR_DALY,
} bms_vendor_t;

typedef struct {
    bms_vendor_t vendor;

    uint16_t cell_mv[BMS_MAX_CELLS];
    uint8_t  cell_count;

    int32_t  pack_mv;        /* total pack voltage, mV                      */
    int32_t  pack_ma;        /* pack current, mA (negative = discharge)     */
    uint32_t remain_mah;     /* remaining capacity                          */
    uint32_t full_mah;       /* full/nominal capacity                       */
    uint16_t cycles;
    uint8_t  soc_pct;        /* state of charge, %                          */

    int16_t  temp_dc[BMS_MAX_TEMPS]; /* temperatures in 0.1 degC            */
    uint8_t  temp_count;

    uint32_t balance_bits;   /* cells 1..32, bit set = balancing            */
    uint16_t protection;     /* vendor protection bitfield (raw)            */

    /* derived */
    uint16_t cell_min_mv;
    uint16_t cell_max_mv;
    uint16_t imbalance_mv;   /* cell_max_mv - cell_min_mv                   */

    /* which groups of fields are valid in this snapshot */
    bool have_summary;       /* pack/soc/cycles/temps                       */
    bool have_cells;         /* per-cell voltages                           */
} bms_data_t;

/* Recompute cell_min/max/imbalance from cell_mv[0..cell_count-1]. */
void bms_recalc_cells(bms_data_t *d);

#ifdef __cplusplus
}
#endif

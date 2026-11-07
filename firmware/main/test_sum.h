/*
 * What the panel itself saw of a test (BMS data the tester does not record),
 * kept in NVS per tester test number so archive protocols can include it.
 * The last TEST_SUM_SLOTS tests are kept (slot = test number mod slots).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEST_SUM_SLOTS 32

typedef struct {
    uint16_t test;            /* tester test number (record 0x70) */
    uint8_t  cells;
    uint8_t  at_start;        /* start values taken in the first minutes */
    uint16_t delta0_mv;       /* imbalance after 1 min of discharge */
    uint16_t delta_max_mv;    /* worst imbalance seen */
    uint16_t cell_min_mv;     /* last seen (end of discharge) */
    uint16_t cell_max_mv;
    int16_t  temp0_dc;        /* battery temperature at the start, 0.1 C (INT16_MIN = n/a) */
    int16_t  temp_max_dc;
    char     bms[20];         /* BMS name */
    int16_t  room_dc;         /* room temperature entered on the Test tab, 0.1 C (INT16_MIN = n/a)
                                 - added later: shorter records load with it unset */
} test_sum_t;

/* load the NVS copy into RAM (call once from an internal-RAM task) */
void test_sum_load(void);
bool test_sum_get(uint16_t test, test_sum_t *out);
void test_sum_put(const test_sum_t *s);

#ifdef __cplusplus
}
#endif

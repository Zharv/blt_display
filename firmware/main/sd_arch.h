/*
 * Test archive on the SD card, one folder per tester cycle:
 *   /sdcard/blt/T<test>_<cycle>/
 *     record.bin  the tester's 192-byte cycle record (as read)
 *     graph.bin   the tester's voltage/current diagram (for the panel)
 *     graph.csv   same, for Excel (';' separated, decimal comma)
 *     bms.csv     what the panel logged during the test (every 10 s):
 *                 tester V/A, BMS V/A/SOC, temperatures, every cell
 *     summary.bin test_sum_t, info.txt  human-readable summary
 * Finished tests are copied from the tester (sync), so they outlive the
 * tester's 150-record / ~250-page rings.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "blt_archive.h"
#include "hist.h"
#include "ui.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool      sd_arch_has(uint16_t test, uint16_t cycle);
esp_err_t sd_arch_save(const blt_test_t *t);
esp_err_t sd_arch_load(uint16_t cycle, blt_test_t *t);
/* the panel's BMS summary saved with a test */
#include "test_sum.h"
bool      sd_arch_get_sum(uint16_t cycle, test_sum_t *out);
/* tests on the card, newest first */
int       sd_arch_list(blt_arch_entry_t *out, int max);

/* append one line of the running test to its bms.csv */
void      sd_arch_log(const online_sample_t *o, uint16_t cycle);
/* read bms.csv back as history points (oldest first) */
int       sd_arch_read_log(uint16_t test, uint16_t cycle, hist_pt_t *out, int max);

/* copy finished tests from the tester to the card (background task) */
void      sd_arch_request_sync(void);
bool      sd_arch_sync_busy(int *done, int *total);

#ifdef __cplusplus
}
#endif

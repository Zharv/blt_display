/*
 * Prepare the BLT tester for the next test: write the battery serial (ID
 * block 0x40) and the planned capacity (rated Ah, settings block 0x100).
 * Read - modify only those fields - write - read back and compare.
 * Refused unless the tester reports state START (no test running).
 * Blocking (~1 s); call from a worker task, not the LVGL task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Current values on the tester (serial from the ID block, rated Ah from the
 * settings block). Read-only. */
esp_err_t blt_prepare_read(char *serial, size_t n, uint16_t *rated_ah);

/* Write serial and/or rated Ah (pass NULL / 0 to keep). msg gets a short
 * human-readable result (UA/EN via tr()). */
esp_err_t blt_prepare_write(const char *serial, uint16_t rated_ah, char *msg, size_t n);

#ifdef __cplusplus
}
#endif

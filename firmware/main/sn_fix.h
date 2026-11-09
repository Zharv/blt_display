/*
 * Battery serial corrected on the panel for a recorded test (e.g. the tester
 * kept the previous serial). Keyed by the tester's cycle index; the tester
 * itself is not changed. NVS "snfix"/"tab", RAM copy for PSRAM-stack readers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SN_FIX_MAX 32

/* load from NVS (call once from an internal-RAM task, e.g. at boot) */
void sn_fix_load(void);
/* the corrected serial of a cycle; false if none */
bool sn_fix_get(uint16_t cycle, char *out, size_t n);
/* set (sn "" or NULL removes); LVGL / internal-RAM context only (writes NVS) */
void sn_fix_set(uint16_t cycle, const char *sn);
/* the serial to show / print: the corrected one, else the tester's */
const char *sn_fix_apply(uint16_t cycle, const char *tester_sn, char *buf, size_t n);

#ifdef __cplusplus
}
#endif

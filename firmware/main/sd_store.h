/*
 * SD card (SDMMC slot 0, FAT) for the test archive: mounted at boot, never
 * formatted by the panel. Our files live under /sdcard/blt/.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_DIR "/sdcard/blt"

/* Mount (call once from a task; non-fatal). */
bool sd_store_start(void);
bool sd_store_ready(void);
/* card size / free space in MB (0 if not mounted) */
void sd_store_space(uint32_t *total_mb, uint32_t *free_mb);
/* dev console: log card info, write/read/delete a probe file in SD_DIR */
void sd_store_selftest(void);

#ifdef __cplusplus
}
#endif

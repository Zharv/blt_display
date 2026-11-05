/*
 * UI fonts: Montserrat (Latin + Ukrainian Cyrillic + °·×Δ−) rendered at runtime
 * by LVGL Tiny TTF from a TTF embedded in flash. Each size falls back to the
 * built-in bitmap Montserrat of a similar size for LV_SYMBOL_* icons.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the fonts. Call once after LVGL is up, with the display locked. */
void fonts_init(void);

/* px: 16, 20, 24, 32 or 48 (nearest is used for other values). */
const lv_font_t *font_ua(int px);

#ifdef __cplusplus
}
#endif

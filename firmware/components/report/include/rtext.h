/*
 * TrueType text for the printed documents (Cyrillic + Latin), drawn into the
 * 8-bpp raster with anti-aliasing. Pure C on top of stb_truetype; the font is
 * the Montserrat TTF that the firmware already embeds for the UI.
 * Sizes are in pixels (at 300 dpi: 10 pt ~ 42 px, 12 pt ~ 50 px).
 */
#pragma once

#include "raster.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bind the TTF data (kept by pointer, must stay valid). */
bool rtext_init(const uint8_t *ttf, size_t len);

/* Pixel width of a UTF-8 string. */
int rtext_width(const char *s, int px, bool bold);

/* Draw s with its top at y (the line box is px*1.25 high). Returns the x after it. */
int rtext_draw(raster_t *r, int x, int y, const char *s, int px, uint8_t gray, bool bold);

/* Right-aligned / centred helpers (x = right edge / centre). */
void rtext_draw_right(raster_t *r, int x, int y, const char *s, int px, uint8_t gray, bool bold);
void rtext_draw_center(raster_t *r, int cx, int y, const char *s, int px, uint8_t gray, bool bold);

/* A paragraph made of runs (e.g. a bold label then regular text), word-wrapped
 * to [x, x + w). Returns the y below the last line. */
typedef struct {
    const char *text;
    bool bold;
} rtext_run_t;
int rtext_para(raster_t *r, int x, int y, int w, const rtext_run_t *runs, int n,
               int px, int line_h, uint8_t gray);

#ifdef __cplusplus
}
#endif

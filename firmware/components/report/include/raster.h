/*
 * Tiny 8-bpp grayscale rasterizer for the printed report. Pure C, no deps,
 * host-testable. 0 = black, 255 = white. Origin top-left.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      w;
    int      h;
    uint8_t *px;   /* w*h bytes, row-major */
} raster_t;

/* Long renders call this now and then so the caller can let other tasks run
 * (firmware: a short vTaskDelay). NULL = never. */
extern void (*raster_yield)(void);
#define RASTER_YIELD() do { if (raster_yield) raster_yield(); } while (0)

/* Bind an existing buffer (w*h bytes) as a raster. */
void raster_init(raster_t *r, uint8_t *buf, int w, int h);
void raster_clear(raster_t *r, uint8_t gray);

void raster_set(raster_t *r, int x, int y, uint8_t gray);
void raster_hline(raster_t *r, int x0, int x1, int y, uint8_t gray);
void raster_vline(raster_t *r, int x, int y0, int y1, uint8_t gray);
void raster_rect(raster_t *r, int x, int y, int w, int h, uint8_t gray);       /* outline */
void raster_fill(raster_t *r, int x, int y, int w, int h, uint8_t gray);       /* filled  */
void raster_line(raster_t *r, int x0, int y0, int x1, int y1, uint8_t gray);   /* Bresenham */

/* 8x8 font. scale >= 1 pixel-doubles. Returns the x after the string. */
int  raster_text(raster_t *r, int x, int y, const char *s, int scale, uint8_t gray);
int  raster_text_w(const char *s, int scale);   /* pixel width of a string */

#ifdef __cplusplus
}
#endif

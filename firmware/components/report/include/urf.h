/*
 * Minimal Apple URF (AirPrint raster) encoder for an 8-bpp grayscale raster.
 * Emits 24-bpp sRGB (gray replicated to R=G=B) which AirPrint printers accept.
 * Rows use repeat-only run packets (valid URF/PackBits, simple and lossless).
 *
 * Confirm colorspace/quality against the real HP Smart Tank 581 once the print
 * path is live; the framing here follows the public URF layout.
 */
#pragma once

#include "raster.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Encode r into URF. If out is NULL, returns the number of bytes the output
 * needs; otherwise writes at most cap bytes and returns the total size (nothing
 * is written when cap < total). dpi is tagged in the page header (e.g. 300).
 */
size_t urf_encode(const raster_t *r, uint32_t dpi, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

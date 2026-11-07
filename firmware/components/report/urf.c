#include "urf.h"

#include <stdbool.h>

/* append helper: counts needed bytes, writes only while within cap */
typedef struct { uint8_t *out; size_t cap; size_t n; } sink_t;

static void put(sink_t *s, uint8_t b)
{
    if (s->out && s->n < s->cap) s->out[s->n] = b;
    s->n++;
}

static void put_be32(sink_t *s, uint32_t v)
{
    put(s, (uint8_t)(v >> 24));
    put(s, (uint8_t)(v >> 16));
    put(s, (uint8_t)(v >> 8));
    put(s, (uint8_t)v);
}

static bool same_row(const uint8_t *a, const uint8_t *b, int w)
{
    for (int x = 0; x < w; x++) if (a[x] != b[x]) return false;
    return true;
}

/* One line, PackBits-style: n = 0..127 -> next pixel repeated n+1 times;
 * n = 129..255 -> 257-n literal pixels follow. */
static void put_row(sink_t *s, const uint8_t *row, int w)
{
    int x = 0;
    while (x < w) {
        int run = 1;
        while (x + run < w && row[x + run] == row[x] && run < 128) run++;
        if (run >= 2) {
            put(s, (uint8_t)(run - 1));
            put(s, row[x]);
            x += run;
            continue;
        }
        /* literal: up to the next pair of equal pixels */
        int n = 1;
        while (x + n < w && n < 128 && !(x + n + 1 < w && row[x + n] == row[x + n + 1])) n++;
        put(s, n == 1 ? 0 : (uint8_t)(257 - n));
        for (int k = 0; k < n; k++) put(s, row[x + k]);
        x += n;
    }
}

size_t urf_encode(const raster_t *r, uint32_t dpi, uint8_t *out, size_t cap)
{
    sink_t s = { out, cap, 0 };

    /* file header: "UNIRAST\0" + page count */
    const char magic[8] = { 'U','N','I','R','A','S','T', 0 };
    for (int i = 0; i < 8; i++) put(&s, (uint8_t)magic[i]);
    put_be32(&s, 1);

    /* page header (32 bytes, big-endian): 8-bpp sGray ("W8" in urf-supported) */
    put(&s, 8);             /* bpp                */
    put(&s, 0);             /* colorSpace: sGray  */
    put(&s, 1);             /* duplex: none       */
    put(&s, 4);             /* quality: normal    */
    put_be32(&s, 0);        /* reserved           */
    put_be32(&s, 0);        /* reserved           */
    put_be32(&s, (uint32_t)r->w);
    put_be32(&s, (uint32_t)r->h);
    put_be32(&s, dpi);
    put_be32(&s, 0);        /* reserved           */
    put_be32(&s, 0);        /* reserved           */

    /* lines: a repeat count (identical following lines, e.g. blank margins),
     * then the PackBits-coded pixels */
    for (int y = 0; y < r->h;) {
        const uint8_t *row = &r->px[(size_t)y * r->w];
        int rep = 0;
        while (y + rep + 1 < r->h && rep < 255 && same_row(row, row + (size_t)(rep + 1) * r->w, r->w)) rep++;
        put(&s, (uint8_t)rep);
        put_row(&s, row, r->w);
        y += rep + 1;
        if ((y & 63) == 0) RASTER_YIELD();
    }

    return s.n;
}

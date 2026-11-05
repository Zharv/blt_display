#include "urf.h"

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

size_t urf_encode(const raster_t *r, uint32_t dpi, uint8_t *out, size_t cap)
{
    sink_t s = { out, cap, 0 };

    /* file header: "UNIRAST\0" + page count */
    const char magic[8] = { 'U','N','I','R','A','S','T', 0 };
    for (int i = 0; i < 8; i++) put(&s, (uint8_t)magic[i]);
    put_be32(&s, 1);

    /* page header (32 bytes, big-endian): 24-bpp sRGB */
    put(&s, 24);            /* bpp                */
    put(&s, 1);             /* colorSpace: sRGB   */
    put(&s, 1);             /* duplex: none       */
    put(&s, 4);             /* quality: normal    */
    put_be32(&s, 0);        /* reserved           */
    put_be32(&s, 0);        /* reserved           */
    put_be32(&s, (uint32_t)r->w);
    put_be32(&s, (uint32_t)r->h);
    put_be32(&s, dpi);
    put_be32(&s, 0);        /* reserved           */
    put_be32(&s, 0);        /* reserved           */

    /* lines: each is "repeat=0" then repeat-run packets of RGB pixels */
    for (int y = 0; y < r->h; y++) {
        const uint8_t *row = &r->px[(size_t)y * r->w];
        put(&s, 0);         /* this line appears once */
        int x = 0;
        while (x < r->w) {
            uint8_t g = row[x];
            int run = 1;
            while (x + run < r->w && row[x + run] == g && run < 128) run++;
            put(&s, (uint8_t)(run - 1)); /* 0..127 => repeat (run) times */
            put(&s, g); put(&s, g); put(&s, g);
            x += run;
        }
    }

    return s.n;
}

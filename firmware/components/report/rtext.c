#include "rtext.h"

#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype_htcw.h"

static stbtt_fontinfo s_font;
static bool s_ok;

bool rtext_init(const uint8_t *ttf, size_t len)
{
    (void)len;
    s_ok = stbtt_InitFont(&s_font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0)) != 0;
    return s_ok;
}

/* next code point of a UTF-8 string; advances *s */
static uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) {
        c = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
    } else if (c >= 0xE0 && p[0] && p[1]) {
        c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2;
    } else if (c >= 0xC0 && p[0]) {
        c = ((c & 0x1F) << 6) | (p[0] & 0x3F); p += 1;
    }
    *s = (const char *)p;
    return c;
}

static int bold_w(int px) { return px >= 30 ? px / 30 : 1; }

/* width of [s, end) (end NULL = whole string) */
static int width_n(const char *s, const char *end, int px, bool bold)
{
    if (!s_ok || !s) return 0;
    float sc = stbtt_ScaleForPixelHeight(&s_font, (float)px);
    float x = 0;
    uint32_t prev = 0;
    while (*s && (!end || s < end)) {
        uint32_t c = utf8_next(&s);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&s_font, (int)c, &adv, &lsb);
        if (prev) x += sc * stbtt_GetCodepointKernAdvance(&s_font, (int)prev, (int)c);
        x += sc * adv;
        prev = c;
    }
    return (int)(x + 0.5f) + (bold ? bold_w(px) : 0);
}

int rtext_width(const char *s, int px, bool bold)
{
    return width_n(s, NULL, px, bold);
}

static void blit(raster_t *r, const uint8_t *a, int w, int h, int x0, int y0, uint8_t gray)
{
    for (int y = 0; y < h; y++) {
        int ry = y0 + y;
        if (ry < 0 || ry >= r->h) continue;
        uint8_t *row = &r->px[(size_t)ry * r->w];
        for (int x = 0; x < w; x++) {
            int rx = x0 + x;
            uint8_t al = a[y * w + x];
            if (!al || rx < 0 || rx >= r->w) continue;
            int c = 255 - al * (255 - gray) / 255;
            if (c < row[rx]) row[rx] = (uint8_t)c;
        }
    }
}

static int draw_n(raster_t *r, int x, int y, const char *s, const char *end, int px, uint8_t gray, bool bold)
{
    if (!s_ok || !s) return x;
    float sc = stbtt_ScaleForPixelHeight(&s_font, (float)px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);
    int base = y + (int)(asc * sc + 0.5f) + px / 8;
    float fx = (float)x;
    uint32_t prev = 0;
    int bw = bold ? bold_w(px) : 0;
    static uint8_t *buf;
    static int buf_cap;
    while (*s && (!end || s < end)) {
        uint32_t c = utf8_next(&s);
        RASTER_YIELD();
        if (prev) fx += sc * stbtt_GetCodepointKernAdvance(&s_font, (int)prev, (int)c);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&s_font, (int)c, &adv, &lsb);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(&s_font, (int)c, sc, sc, fx - (int)fx, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            if (gw * gh > buf_cap) {
                free(buf);
                buf_cap = gw * gh;
                buf = malloc((size_t)buf_cap);
                if (!buf) { buf_cap = 0; return (int)fx; }
            }
            stbtt_MakeCodepointBitmapSubpixel(&s_font, buf, gw, gh, gw, sc, sc, fx - (int)fx, 0, (int)c);
            for (int b = 0; b <= bw; b++) blit(r, buf, gw, gh, (int)fx + x0 + b, base + y0, gray);
        }
        fx += sc * adv;
        prev = c;
    }
    return (int)(fx + 0.5f) + bw;
}

int rtext_draw(raster_t *r, int x, int y, const char *s, int px, uint8_t gray, bool bold)
{
    return draw_n(r, x, y, s, NULL, px, gray, bold);
}

void rtext_draw_right(raster_t *r, int x, int y, const char *s, int px, uint8_t gray, bool bold)
{
    rtext_draw(r, x - rtext_width(s, px, bold), y, s, px, gray, bold);
}

void rtext_draw_center(raster_t *r, int cx, int y, const char *s, int px, uint8_t gray, bool bold)
{
    rtext_draw(r, cx - rtext_width(s, px, bold) / 2, y, s, px, gray, bold);
}

int rtext_para(raster_t *r, int x, int y, int w, const rtext_run_t *runs, int n,
               int px, int line_h, uint8_t gray)
{
    int cx = x;
    int sp = width_n(" ", NULL, px, false);
    for (int i = 0; i < n; i++) {
        const char *s = runs[i].text;
        bool bold = runs[i].bold;
        while (s && *s) {
            if (*s == ' ') {                      /* a space: advance, never at a line start */
                if (cx > x) cx += sp;
                s++;
                continue;
            }
            const char *e = s;
            while (*e && *e != ' ') e++;
            int ww = width_n(s, e, px, bold);
            if (cx + ww > x + w && cx > x) {     /* wrap */
                y += line_h;
                cx = x;
            }
            cx = draw_n(r, cx, y, s, e, px, gray, bold);
            s = e;
        }
    }
    return y + line_h;
}

/* Host test for the URF encoder: encode rasters and decode them back
 * (8-bpp sGray, PackBits repeat/literal runs, line repeats). */
#define _CRT_SECURE_NO_WARNINGS
#include "urf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* decode an encoded page into dst (W*H); returns bytes consumed or 0 */
static size_t decode(const uint8_t *buf, size_t len, uint8_t *dst, int W, int H)
{
    size_t p = 44;
    int y = 0;
    while (y < H) {
        if (p >= len) return 0;
        int rep = buf[p++];
        uint8_t *row = dst + (size_t)y * W;
        int x = 0;
        while (x < W) {
            if (p >= len) return 0;
            int c = buf[p++];
            if (c <= 127) {
                for (int k = 0; k <= c && x < W; k++) row[x++] = buf[p];
                p++;
            } else if (c >= 129) {
                int n = 257 - c;
                for (int k = 0; k < n && x < W; k++) row[x++] = buf[p++];
            } else {
                return 0;           /* 128 is not produced */
            }
        }
        for (int k = 1; k <= rep && y + k < H; k++) memcpy(dst + (size_t)(y + k) * W, row, (size_t)W);
        y += rep + 1;
    }
    return p;
}

static void roundtrip(const char *name, const uint8_t *src, int W, int H, size_t max_size)
{
    raster_t r;
    raster_init(&r, (uint8_t *)src, W, H);
    size_t need = urf_encode(&r, 300, NULL, 0);
    uint8_t *buf = (uint8_t *)malloc(need);
    size_t got = urf_encode(&r, 300, buf, need);
    CHECK(got == need, "%s: two-pass size mismatch %zu vs %zu", name, got, need);
    CHECK(memcmp(buf, "UNIRAST", 7) == 0 && buf[7] == 0, "%s: magic", name);
    CHECK(be32(buf + 8) == 1, "%s: page count", name);
    CHECK(buf[12] == 8 && buf[13] == 0, "%s: 8-bpp sGray", name);
    CHECK(be32(buf + 24) == (uint32_t)W && be32(buf + 28) == (uint32_t)H, "%s: size", name);
    CHECK(be32(buf + 32) == 300, "%s: dpi", name);
    uint8_t *dst = (uint8_t *)calloc((size_t)W * H, 1);
    size_t used = decode(buf, need, dst, W, H);
    CHECK(used == need, "%s: consumed %zu of %zu", name, used, need);
    CHECK(memcmp(src, dst, (size_t)W * H) == 0, "%s: pixels differ", name);
    if (max_size) CHECK(need <= max_size, "%s: %zu bytes > %zu", name, need, max_size);
    printf("  %s: %dx%d -> %zu bytes\n", name, W, H, need);
    free(dst);
    free(buf);
}

int main(void)
{
    /* noisy pixels (literal runs) */
    enum { W = 300, H = 7 };
    static uint8_t a[W * H];
    for (int i = 0; i < W * H; i++) a[i] = (uint8_t)((i * 37) & 0xFF);
    roundtrip("noise", a, W, H, 0);

    /* mixed: runs, singles, pairs, a literal of exactly 128 and 129 */
    static uint8_t b[W * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t v = 255;
            if (x < 128) v = (uint8_t)(x * 2);                  /* 128 different */
            else if (x < 140) v = 7;                            /* run */
            else if (x < 269) v = (uint8_t)(x * 3 + y);         /* 129 different */
            else if (x % 3 == 0) v = 0;                         /* singles + pairs */
            b[y * W + x] = v;
        }
    roundtrip("mixed", b, W, H, 0);

    /* a blank page with one text-like band: line repeats keep it small */
    enum { PW = 2480, PH = 300 };
    uint8_t *pg = (uint8_t *)malloc((size_t)PW * PH);
    memset(pg, 255, (size_t)PW * PH);
    for (int y = 140; y < 160; y++)
        for (int x = 300; x < 2000; x++) pg[y * PW + x] = (uint8_t)((x / 7 + y) % 5 == 0 ? 0 : 255);
    roundtrip("page", pg, PW, PH, 30000);
    free(pg);

    /* single pixel and single column edge cases */
    uint8_t one[1] = { 42 };
    roundtrip("1x1", one, 1, 1, 0);
    uint8_t col[5] = { 1, 1, 2, 3, 3 };
    roundtrip("1x5", col, 1, 5, 0);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

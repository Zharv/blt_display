/* Host test for the URF encoder: encode a small raster and decode it back. */
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

int main(void)
{
    enum { W = 7, H = 4 };
    uint8_t src[W * H];
    for (int i = 0; i < W * H; i++) src[i] = (uint8_t)((i * 37) & 0xFF);
    raster_t r; raster_init(&r, src, W, H);

    size_t need = urf_encode(&r, 300, NULL, 0);
    CHECK(need > 44, "size %zu", need);
    uint8_t *buf = (uint8_t *)malloc(need);
    size_t got = urf_encode(&r, 300, buf, need);
    CHECK(got == need, "two-pass size mismatch %zu vs %zu", got, need);

    CHECK(memcmp(buf, "UNIRAST", 7) == 0 && buf[7] == 0, "magic");
    CHECK(be32(buf + 8) == 1, "page count");
    CHECK(buf[12] == 24, "bpp");
    CHECK(be32(buf + 24) == W, "width %u", be32(buf + 24));
    CHECK(be32(buf + 28) == H, "height %u", be32(buf + 28));
    CHECK(be32(buf + 32) == 300, "dpi %u", be32(buf + 32));

    /* decode lines and compare to src (R channel == gray) */
    size_t p = 44;
    int mismatches = 0;
    for (int y = 0; y < H; y++) {
        CHECK(buf[p] == 0, "line repeat byte");
        p++;
        int x = 0;
        while (x < W) {
            int run = buf[p++] + 1;
            uint8_t rr = buf[p], gg = buf[p + 1], bb = buf[p + 2];
            p += 3;
            CHECK(rr == gg && gg == bb, "rgb equal");
            for (int k = 0; k < run && x < W; k++, x++) {
                if (rr != src[y * W + x]) mismatches++;
            }
        }
    }
    CHECK(mismatches == 0, "%d pixel mismatches", mismatches);
    CHECK(p == need, "consumed %zu of %zu", p, need);

    free(buf);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

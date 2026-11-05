/* Host test for the report rasterizer: render a sample report, sanity-check it,
 * and write a PGM preview (path from argv[1]). */
#define _CRT_SECURE_NO_WARNINGS
#include "report.h"
#include "urf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define NPT 90

int main(int argc, char **argv)
{
    static float cv[NPT], ci[NPT], cd[NPT];
    for (int i = 0; i < NPT; i++) {
        float ah = i * 0.13f;
        cv[i] = 53.3f - ah * 0.22f;            /* sagging voltage */
        ci[i] = (i < NPT - 3) ? 12.0f : 0.0f;  /* flat discharge then stop */
        cd[i] = 4.0f + i * i * 0.006f;         /* imbalance grows near the end */
    }

    /* graph top rounding */
    CHECK(report_nice_ceil(0) == 5, "nice 0");
    CHECK(report_nice_ceil(5) == 5, "nice 5");
    CHECK(report_nice_ceil(5.1f) == 10, "nice 5.1");
    CHECK(report_nice_ceil(52) == 100, "nice 52");
    CHECK(report_nice_ceil(1500) == 1500, "nice 1500");
    uint16_t cells[14];
    uint16_t lo = 0xFFFF, hi = 0;
    for (int c = 0; c < 14; c++) {
        cells[c] = (uint16_t)(3320 + ((c % 5) - 2) * 6 - (c == 13 ? 40 : 0));
        if (cells[c] < lo) lo = cells[c];
        if (cells[c] > hi) hi = cells[c];
    }

    report_data_t d = {
        .title = "BATTERY TEST REPORT",
        .date = "2026-11-05 14:30",
        .operator_name = "ROMAN",
        .battery_id = "LFP-14S-001",
        .nominal_v = 51.8f, .nominal_ah = 50.0f, .discharge_a = 12.0f,
        .discharged_ah = 11.7f, .duration_s = 58u * 60u + 30u,
        .min_voltage_v = 42.0f, .max_temp_c = 31.5f, .cutoff_v = 42.0f,
        .curve_v = cv, .curve_i = ci, .curve_n = NPT,
        .curve_vmin = 40.0f, .curve_vmax = 55.0f, .curve_imax = 20.0f,
        .curve_delta_mv = cd,
        .bms_present = true, .cell_mv = cells, .cell_count = 14,
        .cell_min_mv = lo, .cell_max_mv = hi, .imbalance_mv = (uint16_t)(hi - lo),
        .soc_pct = 8,
    };

    int W = 1000, H = 1400;
    uint8_t *buf = (uint8_t *)malloc((size_t)W * H);
    CHECK(buf != NULL, "alloc");
    raster_t r; raster_init(&r, buf, W, H);
    raster_clear(&r, 255);
    report_render(&d, &r);

    /* some ink was laid down */
    long dark = 0;
    for (long i = 0; i < (long)W * H; i++) if (buf[i] < 128) dark++;
    CHECK(dark > 2000, "too few dark pixels: %ld", dark);

    /* the imbalance graph adds ink; without the curve it is skipped */
    {
        uint8_t *b2 = (uint8_t *)malloc((size_t)W * H);
        raster_t r2; raster_init(&r2, b2, W, H);
        raster_clear(&r2, 255);
        report_data_t d2 = d;
        d2.curve_delta_mv = NULL;
        report_render(&d2, &r2);
        long dark2 = 0;
        for (long i = 0; i < (long)W * H; i++) if (b2[i] < 128) dark2++;
        CHECK(dark > dark2 + 500, "imbalance graph ink: %ld vs %ld", dark, dark2);
        /* footer stays inside the page */
        long tail = 0;
        for (long i = (long)W * (H - 4); i < (long)W * H; i++) if (b2[i] < 128) tail++;
        CHECK(tail == 0, "ink in the last rows: %ld", tail);
        free(b2);
    }

    /* URF encodes and round-trips in size */
    size_t need = urf_encode(&r, 300, NULL, 0);
    CHECK(need > 44, "urf size %zu", need);

    if (argc > 1) {
        FILE *fp = fopen(argv[1], "wb");
        if (fp) {
            fprintf(fp, "P5\n%d %d\n255\n", W, H);
            fwrite(buf, 1, (size_t)W * H, fp);
            fclose(fp);
            printf("wrote preview %s (%dx%d), %ld dark px\n", argv[1], W, H, dark);
        }
    }
    free(buf);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

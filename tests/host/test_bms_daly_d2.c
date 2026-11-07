/* Host test for the Daly BLE "D2" (Modbus) status decoder. */
#include "bms_daly_d2.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(uint8_t *f, int off, unsigned v) { f[off] = (uint8_t)(v >> 8); f[off + 1] = (uint8_t)v; }

int main(void)
{
    /* CRC-16/MODBUS reference vector: "123456789" -> 0x4B37 */
    CHECK(daly_d2_crc16((const uint8_t *)"123456789", 9) == 0x4B37, "crc check value 0x%04x",
          daly_d2_crc16((const uint8_t *)"123456789", 9));

    /* status request; ESPHome/Daly logs show D2 03 00 00 00 3E D7 B9 */
    uint8_t req[8];
    CHECK(daly_d2_build_read(req, sizeof req, 0x0000, 0x3E) == 8, "build");
    const uint8_t want[8] = { 0xD2, 0x03, 0x00, 0x00, 0x00, 0x3E, 0xD7, 0xB9 };
    CHECK(memcmp(req, want, 8) == 0, "status request %02X %02X %02X %02X %02X %02X %02X %02X",
          req[0], req[1], req[2], req[3], req[4], req[5], req[6], req[7]);
    CHECK(daly_d2_build_read(req, 7, 0, 1) == 0, "cap check");

    /* synthetic 62-register status frame: 16 cells, 2 temps */
    uint8_t f[DALY_D2_STATUS_LEN];
    memset(f, 0, sizeof f);
    f[0] = 0xD2; f[1] = 0x03; f[2] = 124;
    for (int i = 0; i < 16; i++) put(f, 3 + 2 * i, 3300 + i);   /* 3300..3315 mV */
    put(f, 3 + 2 * 5, 3290);                                    /* cell 6 lowest */
    put(f, 67, 65);  put(f, 69, 38);       /* 25 C, -2 C */
    put(f, 83, 529);                        /* 52.9 V */
    put(f, 85, 30000 - 125);                /* -12.5 A (discharging) */
    put(f, 87, 876);                        /* 87.6 % -> 88 */
    put(f, 99, 1234);                       /* 123.4 Ah */
    put(f, 101, 16);                        /* cells */
    put(f, 103, 2);                         /* temps */
    put(f, 105, 42);                        /* cycles */
    uint16_t crc = daly_d2_crc16(f, sizeof f - 2);
    f[sizeof f - 2] = (uint8_t)crc; f[sizeof f - 1] = (uint8_t)(crc >> 8);

    CHECK(daly_d2_frame_len(f, 3) == DALY_D2_STATUS_LEN, "frame len %u", (unsigned)daly_d2_frame_len(f, 3));
    CHECK(daly_d2_check(f, 60) == DALY_D2_INCOMPLETE, "partial frame");
    CHECK(daly_d2_check(f, sizeof f) == DALY_D2_OK, "crc ok");

    bms_data_t d;
    CHECK(daly_d2_parse_status(f, sizeof f, &d) == DALY_D2_OK, "parse");
    CHECK(d.vendor == BMS_VENDOR_DALY, "vendor");
    CHECK(d.cell_count == 16, "cells %u", d.cell_count);
    CHECK(d.cell_mv[0] == 3300 && d.cell_mv[15] == 3315, "cell values");
    CHECK(d.cell_min_mv == 3290 && d.cell_max_mv == 3315 && d.imbalance_mv == 25, "min/max %u %u %u",
          d.cell_min_mv, d.cell_max_mv, d.imbalance_mv);
    CHECK(d.temp_count == 2 && d.temp_dc[0] == 250 && d.temp_dc[1] == -20, "temps %d %d", d.temp_dc[0], d.temp_dc[1]);
    CHECK(d.pack_mv == 52900, "pack %ld", (long)d.pack_mv);
    CHECK(d.pack_ma == -12500, "current %ld", (long)d.pack_ma);
    CHECK(d.soc_pct == 88, "soc %u", d.soc_pct);
    CHECK(d.remain_mah == 123400, "remain %lu", (unsigned long)d.remain_mah);
    CHECK(d.cycles == 42, "cycles %u", d.cycles);

    f[50] ^= 1;
    CHECK(daly_d2_parse_status(f, sizeof f, &d) == DALY_D2_ERR_CRC, "corrupted -> crc error");
    CHECK(daly_d2_parse_status(f, 100, &d) == DALY_D2_ERR_LEN, "wrong length");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

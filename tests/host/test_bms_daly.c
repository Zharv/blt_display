/* Host unit test for the Daly BMS driver. */
#define _CRT_SECURE_NO_WARNINGS
#include "bms_daly.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                   \
    do { if (cond) { g_pass++; }                                           \
         else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
                printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* build a 13-byte frame with D0..D7 and a valid checksum */
static void make(uint8_t *f, uint8_t cmd, const uint8_t d[8])
{
    f[0] = DALY_START; f[1] = DALY_ADDR_BMS; f[2] = cmd; f[3] = DALY_DATA_LEN;
    memcpy(&f[4], d, 8);
    f[12] = daly_checksum(f, 12);
}

int main(void)
{
    /* request frame */
    uint8_t req[13];
    size_t rn = daly_build_read(req, sizeof req, DALY_CMD_SOC);
    CHECK(rn == 13 && req[0] == 0xA5 && req[1] == 0x40 && req[2] == 0x90 &&
          req[3] == 0x08 && req[12] == daly_checksum(req, 12), "soc req frame");

    bms_data_t b;
    memset(&b, 0, sizeof b);
    uint8_t f[13], d[8];

    /* 0x90 SOC: 53.3 V, current 12.0 A discharge, SOC 87.5% */
    memset(d, 0, 8);
    put_be16(&d[0], 533);           /* 53.3 V */
    put_be16(&d[4], 30000 - 120);   /* -12.0 A (offset 30000, 0.1 A) */
    put_be16(&d[6], 875);           /* 87.5 % */
    make(f, DALY_CMD_SOC, d);
    daly_err_t e = daly_parse(f, 13, &b);
    CHECK(e == DALY_OK, "0x90 err=%d", e);
    CHECK(b.pack_mv == 53300, "pack_mv=%ld", (long)b.pack_mv);
    CHECK(b.pack_ma == -12000, "pack_ma=%ld", (long)b.pack_ma);
    CHECK(b.soc_pct == 87, "soc=%u", b.soc_pct);

    /* 0x93 remaining capacity 20000 mAh */
    memset(d, 0, 8);
    put_be32(&d[4], 20000);
    make(f, DALY_CMD_MOS, d);
    e = daly_parse(f, 13, &b);
    CHECK(e == DALY_OK && b.remain_mah == 20000u, "remain=%lu", (unsigned long)b.remain_mah);

    /* 0x95 cells, frame 1: 3300/3310/3290 mV */
    memset(d, 0, 8);
    d[0] = 1;
    put_be16(&d[1], 3300); put_be16(&d[3], 3310); put_be16(&d[5], 3290);
    make(f, DALY_CMD_CELLS, d);
    e = daly_parse(f, 13, &b);
    CHECK(e == DALY_OK, "0x95 err=%d", e);
    /* 0x95 cells, frame 2: 3305/0/0 -> 4th cell only */
    memset(d, 0, 8);
    d[0] = 2;
    put_be16(&d[1], 3305);
    make(f, DALY_CMD_CELLS, d);
    e = daly_parse(f, 13, &b);
    CHECK(e == DALY_OK, "0x95#2 err=%d", e);
    CHECK(b.cell_count == 4, "cells=%u", b.cell_count);
    CHECK(b.cell_mv[0] == 3300 && b.cell_mv[3] == 3305, "cell values");
    CHECK(b.cell_min_mv == 3290 && b.cell_max_mv == 3310, "min/max %u/%u",
          b.cell_min_mv, b.cell_max_mv);
    CHECK(b.imbalance_mv == 20, "imbalance=%u", b.imbalance_mv);

    /* 0x96 temps, frame 1: first two 25C and 26C (raw = C + 40) */
    memset(d, 0, 8);
    d[0] = 1; d[1] = 65; d[2] = 66;
    make(f, DALY_CMD_TEMPS, d);
    e = daly_parse(f, 13, &b);
    CHECK(e == DALY_OK, "0x96 err=%d", e);
    CHECK(b.temp_dc[0] == 250 && b.temp_dc[1] == 260, "temps %d/%d",
          b.temp_dc[0], b.temp_dc[1]);

    /* corrupt checksum */
    make(f, DALY_CMD_SOC, d);
    f[12] ^= 0xFF;
    e = daly_parse(f, 13, &b);
    CHECK(e == DALY_ERR_CHECKSUM, "corrupt -> %d", e);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/*
 * Host unit test for firmware/components/bms (JBD driver).
 *   - request frame anchored to the public `DD A5 03 00 FF FD 77`
 *   - 0x03 basic-info and 0x04 cell-voltage decoding (self-consistent checksum)
 *   - checksum rejection
 * Response-side field offsets follow the public JBD spec; see bms_jbd.h.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "bms_jbd.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);       \
               printf(__VA_ARGS__); printf("\n"); }                        \
    } while (0)

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* Build DD reg status len [data] chkH chkL 77 with a valid checksum. */
static size_t make_jbd(uint8_t *buf, uint8_t reg, uint8_t status,
                       const uint8_t *data, uint8_t plen)
{
    buf[0] = JBD_START;
    buf[1] = reg;
    buf[2] = status;
    buf[3] = plen;
    if (plen && data) memcpy(&buf[4], data, plen);
    uint16_t chk = jbd_checksum(&buf[2], (size_t)plen + 2u);
    put_be16(&buf[4 + plen], chk);
    buf[6 + plen] = JBD_END;
    return (size_t)plen + 7u;
}

int main(void)
{
    /* request frame */
    uint8_t req[8];
    size_t rn = jbd_build_read(req, sizeof req, JBD_REG_BASIC);
    const uint8_t expect_req[7] = { 0xDD, 0xA5, 0x03, 0x00, 0xFF, 0xFD, 0x77 };
    CHECK(rn == 7 && memcmp(req, expect_req, 7) == 0,
          "read-basic req != DD A5 03 00 FF FD 77");

    /* 0x03 basic info */
    uint8_t d[64];
    memset(d, 0, sizeof d);
    put_be16(&d[0], 5328);       /* total voltage 53.28 V        */
    put_be16(&d[2], (uint16_t)(int16_t)-1234); /* current -12.34 A */
    put_be16(&d[4], 2000);       /* remain 20000 mAh             */
    put_be16(&d[6], 3000);       /* full   30000 mAh             */
    put_be16(&d[8], 12);         /* cycles                       */
    put_be16(&d[12], 0x0005);    /* balance low: cells 1 and 3   */
    put_be16(&d[16], 0x0000);    /* protection                   */
    d[18] = 0x20;                /* sw version                   */
    d[19] = 85;                  /* soc %                        */
    d[20] = 0x03;                /* fet                          */
    d[21] = 4;                   /* cell count                   */
    d[22] = 2;                   /* ntc count                    */
    put_be16(&d[23], 3031);      /* 30.0 C                       */
    put_be16(&d[25], 3041);      /* 31.0 C                       */

    uint8_t frame[80];
    size_t n = make_jbd(frame, JBD_REG_BASIC, 0x00, d, 27);
    bms_data_t bms;
    memset(&bms, 0, sizeof bms);
    jbd_err_t e = jbd_parse(frame, n, &bms);
    CHECK(e == JBD_OK, "0x03 parse err=%d", e);
    CHECK(bms.pack_mv == 53280, "pack_mv=%ld", (long)bms.pack_mv);
    CHECK(bms.pack_ma == -12340, "pack_ma=%ld", (long)bms.pack_ma);
    CHECK(bms.remain_mah == 20000u, "remain=%lu", (unsigned long)bms.remain_mah);
    CHECK(bms.cycles == 12, "cycles=%u", bms.cycles);
    CHECK(bms.soc_pct == 85, "soc=%u", bms.soc_pct);
    CHECK(bms.balance_bits == 0x5u, "balance=%lu", (unsigned long)bms.balance_bits);
    CHECK(bms.temp_count == 2, "temps=%u", bms.temp_count);
    CHECK(bms.temp_dc[0] == 300, "t0=%d", bms.temp_dc[0]);
    CHECK(bms.temp_dc[1] == 310, "t1=%d", bms.temp_dc[1]);
    CHECK(bms.have_summary, "have_summary");

    /* 0x04 cell voltages, same snapshot */
    uint8_t c[8];
    put_be16(&c[0], 3300);
    put_be16(&c[2], 3310);
    put_be16(&c[4], 3290);
    put_be16(&c[6], 3305);
    n = make_jbd(frame, JBD_REG_CELLS, 0x00, c, 8);
    e = jbd_parse(frame, n, &bms);
    CHECK(e == JBD_OK, "0x04 parse err=%d", e);
    CHECK(bms.cell_count == 4, "cells=%u", bms.cell_count);
    CHECK(bms.cell_min_mv == 3290, "min=%u", bms.cell_min_mv);
    CHECK(bms.cell_max_mv == 3310, "max=%u", bms.cell_max_mv);
    CHECK(bms.imbalance_mv == 20, "imbalance=%u", bms.imbalance_mv);
    CHECK(bms.have_cells, "have_cells");

    /* corrupt checksum */
    n = make_jbd(frame, JBD_REG_CELLS, 0x00, c, 8);
    frame[4] ^= 0xFF;
    e = jbd_parse(frame, n, &bms);
    CHECK(e == JBD_ERR_CHECKSUM, "corrupt -> %d", e);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

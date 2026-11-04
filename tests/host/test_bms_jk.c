/* Host unit test for the JK BMS driver (new protocol, 24S cell-info frame). */
#define _CRT_SECURE_NO_WARNINGS
#include "bms_jk.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                   \
    do { if (cond) { g_pass++; }                                           \
         else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
                printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int main(void)
{
    /* command frame: request cell info (0x96) */
    uint8_t cmd[20];
    size_t cn = jk_build_command(cmd, sizeof cmd, JK_CMD_CELLINFO, 0, 0);
    CHECK(cn == 20 && cmd[0] == 0xAA && cmd[1] == 0x55 && cmd[2] == 0x90 &&
          cmd[3] == 0xEB && cmd[4] == 0x96 && cmd[19] == jk_checksum(cmd, 19),
          "cellinfo command frame");

    /* build a 24S cell-info notification frame */
    uint8_t f[JK_FRAME_LEN];
    memset(f, 0, sizeof f);
    f[0] = JK_HDR0; f[1] = JK_HDR1; f[2] = JK_HDR2; f[3] = JK_HDR3;
    f[4] = JK_TYPE_CELLINFO;
    f[5] = 0x01;                      /* counter */
    put_le16(&f[6 + 0 * 2], 3300);
    put_le16(&f[6 + 1 * 2], 3310);
    put_le16(&f[6 + 2 * 2], 3290);
    put_le16(&f[6 + 3 * 2], 3305);
    put_le32(&f[118], 53300);         /* pack mV */
    put_le32(&f[126], (uint32_t)(int32_t)-12000); /* current mA */
    put_le16(&f[130], 250);           /* temp1 25.0C */
    put_le16(&f[132], 260);           /* temp2 26.0C */
    put_le16(&f[134], 300);           /* mos  30.0C */
    f[141] = 85;                      /* soc % */
    put_le32(&f[142], 20000);         /* remain mAh */
    put_le32(&f[146], 30000);         /* full mAh */
    put_le32(&f[150], 12);            /* cycles */
    f[JK_FRAME_LEN - 1] = jk_checksum(f, JK_FRAME_LEN - 1);

    bms_data_t b;
    jk_err_t e = jk_parse_cellinfo(f, sizeof f, JK_VARIANT_24S, &b);
    CHECK(e == JK_OK, "parse err=%d", e);
    CHECK(b.cell_count == 4, "cells=%u", b.cell_count);
    CHECK(b.cell_mv[0] == 3300 && b.cell_mv[3] == 3305, "cell values");
    CHECK(b.cell_min_mv == 3290 && b.cell_max_mv == 3310, "min/max %u/%u",
          b.cell_min_mv, b.cell_max_mv);
    CHECK(b.imbalance_mv == 20, "imbalance=%u", b.imbalance_mv);
    CHECK(b.pack_mv == 53300, "pack_mv=%ld", (long)b.pack_mv);
    CHECK(b.pack_ma == -12000, "pack_ma=%ld", (long)b.pack_ma);
    CHECK(b.soc_pct == 85, "soc=%u", b.soc_pct);
    CHECK(b.remain_mah == 20000u, "remain=%lu", (unsigned long)b.remain_mah);
    CHECK(b.full_mah == 30000u, "full=%lu", (unsigned long)b.full_mah);
    CHECK(b.cycles == 12, "cycles=%u", b.cycles);
    CHECK(b.temp_count == 3 && b.temp_dc[0] == 250 && b.temp_dc[2] == 300,
          "temps %d/%d/%d", b.temp_dc[0], b.temp_dc[1], b.temp_dc[2]);

    /* corrupt checksum */
    f[JK_FRAME_LEN - 1] ^= 0xFF;
    e = jk_parse_cellinfo(f, sizeof f, JK_VARIANT_24S, &b);
    CHECK(e == JK_ERR_CHECKSUM, "corrupt -> %d", e);
    f[JK_FRAME_LEN - 1] = jk_checksum(f, JK_FRAME_LEN - 1);

    /* wrong header */
    f[0] = 0x00;
    e = jk_parse_cellinfo(f, sizeof f, JK_VARIANT_24S, &b);
    CHECK(e == JK_ERR_FRAMING, "bad header -> %d", e);
    f[0] = JK_HDR0;

    /* wrong type (settings) — recompute checksum so only type differs */
    f[4] = JK_TYPE_SETTINGS;
    f[JK_FRAME_LEN - 1] = jk_checksum(f, JK_FRAME_LEN - 1);
    e = jk_parse_cellinfo(f, sizeof f, JK_VARIANT_24S, &b);
    CHECK(e == JK_ERR_TYPE, "wrong type -> %d", e);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

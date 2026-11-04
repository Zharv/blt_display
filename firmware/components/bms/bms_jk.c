#include "bms_jk.h"

#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int16_t  le16s(const uint8_t *p) { return (int16_t)le16(p); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint8_t jk_checksum(const uint8_t *bytes, size_t len)
{
    uint8_t s = 0;
    for (size_t i = 0; i < len; i++) {
        s = (uint8_t)(s + bytes[i]);
    }
    return s;
}

size_t jk_build_command(uint8_t *out, size_t cap, uint8_t cmd, uint32_t value, uint8_t vlen)
{
    if (out == NULL || cap < 20) {
        return 0;
    }
    memset(out, 0, 20);
    out[0] = 0xAA; out[1] = 0x55; out[2] = 0x90; out[3] = 0xEB;
    out[4] = cmd;
    out[5] = vlen;
    out[6] = (uint8_t)(value & 0xFF);
    out[7] = (uint8_t)((value >> 8) & 0xFF);
    out[8] = (uint8_t)((value >> 16) & 0xFF);
    out[9] = (uint8_t)((value >> 24) & 0xFF);
    out[19] = jk_checksum(out, 19);
    return 20;
}

jk_err_t jk_parse_cellinfo(const uint8_t *frame, size_t len, jk_variant_t variant,
                           bms_data_t *out)
{
    if (frame == NULL || out == NULL || len < JK_FRAME_LEN) {
        return JK_ERR_FRAMING;
    }
    if (frame[0] != JK_HDR0 || frame[1] != JK_HDR1 ||
        frame[2] != JK_HDR2 || frame[3] != JK_HDR3) {
        return JK_ERR_FRAMING;
    }
    if (jk_checksum(frame, JK_FRAME_LEN - 1) != frame[JK_FRAME_LEN - 1]) {
        return JK_ERR_CHECKSUM;
    }
    if (frame[4] != JK_TYPE_CELLINFO) {
        return JK_ERR_TYPE;
    }

    memset(out, 0, sizeof(*out));
    out->vendor = BMS_VENDOR_JK;

    bool s32 = (variant == JK_VARIANT_32S);
    uint8_t ncells = s32 ? 32u : 24u;
    uint16_t L = s32 ? 32u : 0u;   /* shift of the "late" field block */

    /* per-cell voltages: offset 6 + 2*i, LE, already in mV (0.001 V unit) */
    for (uint8_t i = 0; i < ncells; i++) {
        uint16_t mv = le16(&frame[6 + i * 2]);
        out->cell_mv[i] = mv;
        if (mv > 0) {
            out->cell_count = (uint8_t)(i + 1);
        }
    }

    out->pack_mv    = (int32_t)le32(&frame[118 + L]);          /* 0.001 V -> mV */
    out->pack_ma    = (int32_t)le32(&frame[126 + L]);          /* 0.001 A -> mA, +chg/-dis */
    out->temp_dc[0] = le16s(&frame[130 + L]);                  /* 0.1 degC */
    out->temp_dc[1] = le16s(&frame[132 + L]);
    out->temp_dc[2] = le16s(&frame[s32 ? (112u + 32u) : 134u]);/* MOS temp */
    out->temp_count = 3;
    out->soc_pct    = frame[141 + L];
    out->remain_mah = le32(&frame[142 + L]);                   /* 0.001 Ah -> mAh */
    out->full_mah   = le32(&frame[146 + L]);
    out->cycles     = (uint16_t)le32(&frame[150 + L]);

    bms_recalc_cells(out);
    out->have_summary = true;
    out->have_cells = true;
    return JK_OK;
}

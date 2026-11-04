#include "bms_jbd.h"

#include <string.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int16_t be16s(const uint8_t *p)
{
    return (int16_t)be16(p);
}

void bms_recalc_cells(bms_data_t *d)
{
    if (d->cell_count == 0) {
        d->cell_min_mv = d->cell_max_mv = d->imbalance_mv = 0;
        return;
    }
    uint16_t lo = 0xFFFF, hi = 0;
    for (uint8_t i = 0; i < d->cell_count && i < BMS_MAX_CELLS; i++) {
        uint16_t v = d->cell_mv[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    d->cell_min_mv = lo;
    d->cell_max_mv = hi;
    d->imbalance_mv = (uint16_t)(hi - lo);
}

uint16_t jbd_checksum(const uint8_t *bytes, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += bytes[i];
    }
    return (uint16_t)((0x10000u - (sum & 0xFFFFu)) & 0xFFFFu);
}

size_t jbd_build_read(uint8_t *out, size_t cap, uint8_t reg)
{
    if (out == NULL || cap < 7) {
        return 0;
    }
    /* checksum covers <reg><len> with len = 0 (payload empty) */
    uint8_t cl[2] = { reg, 0x00 };
    uint16_t chk = jbd_checksum(cl, sizeof cl);
    out[0] = JBD_START;
    out[1] = JBD_READ;
    out[2] = reg;
    out[3] = 0x00;
    out[4] = (uint8_t)(chk >> 8);
    out[5] = (uint8_t)(chk & 0xFF);
    out[6] = JBD_END;
    return 7;
}

jbd_err_t jbd_parse(const uint8_t *frame, size_t len, bms_data_t *out)
{
    if (frame == NULL || out == NULL) {
        return JBD_ERR_FRAMING;
    }
    if (len < 7 || frame[0] != JBD_START || frame[len - 1] != JBD_END) {
        return JBD_ERR_FRAMING;
    }

    uint8_t reg    = frame[1];
    uint8_t status = frame[2];
    uint8_t plen   = frame[3];

    /* frame = DD reg status len [data(plen)] chkH chkL 77 */
    if ((size_t)plen + 7u != len) {
        return JBD_ERR_FRAMING;
    }

    const uint8_t *data = &frame[4];
    uint16_t want = be16(&frame[4 + plen]);
    /* checksum spans <status><len><data> (confirm vs capture) */
    uint16_t got = jbd_checksum(&frame[2], (size_t)plen + 2u);
    if (got != want) {
        return JBD_ERR_CHECKSUM;
    }
    if (status != 0x00) {
        return JBD_ERR_STATUS;
    }

    out->vendor = BMS_VENDOR_JBD;

    if (reg == JBD_REG_BASIC) {
        if (plen < 23) {
            return JBD_ERR_SHORT;
        }
        out->pack_mv    = (int32_t)be16(&data[0]) * 10;
        out->pack_ma    = (int32_t)be16s(&data[2]) * 10;
        out->remain_mah = (uint32_t)be16(&data[4]) * 10u;
        out->full_mah   = (uint32_t)be16(&data[6]) * 10u;
        out->cycles     = be16(&data[8]);
        /* data[10..11] production date */
        out->balance_bits = (uint32_t)be16(&data[12]) |
                            ((uint32_t)be16(&data[14]) << 16);
        out->protection = be16(&data[16]);
        /* data[18] sw version */
        out->soc_pct    = data[19];
        /* data[20] FET status */
        uint8_t ncell   = data[21];
        uint8_t ntc     = data[22];
        (void)ncell;
        if (ntc > BMS_MAX_TEMPS) {
            ntc = BMS_MAX_TEMPS;
        }
        if ((size_t)23 + (size_t)ntc * 2u > plen) {
            return JBD_ERR_SHORT;
        }
        for (uint8_t i = 0; i < ntc; i++) {
            uint16_t raw = be16(&data[23 + i * 2]);  /* 0.1 K */
            out->temp_dc[i] = (int16_t)((int32_t)raw - 2731); /* -> 0.1 degC */
        }
        out->temp_count = ntc;
        out->have_summary = true;
        return JBD_OK;
    }

    if (reg == JBD_REG_CELLS) {
        uint8_t n = (uint8_t)(plen / 2u);
        if (n > BMS_MAX_CELLS) {
            n = BMS_MAX_CELLS;
        }
        for (uint8_t i = 0; i < n; i++) {
            out->cell_mv[i] = be16(&data[i * 2]);
        }
        out->cell_count = n;
        bms_recalc_cells(out);
        out->have_cells = true;
        return JBD_OK;
    }

    return JBD_ERR_UNSUPPORTED;
}

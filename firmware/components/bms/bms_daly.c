#include "bms_daly.h"

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint8_t daly_checksum(const uint8_t *bytes, size_t len)
{
    uint8_t s = 0;
    for (size_t i = 0; i < len; i++) {
        s = (uint8_t)(s + bytes[i]);
    }
    return s;
}

size_t daly_build_read(uint8_t *out, size_t cap, uint8_t cmd)
{
    if (out == NULL || cap < DALY_FRAME_LEN) {
        return 0;
    }
    out[0] = DALY_START;
    out[1] = DALY_ADDR_HOST;
    out[2] = cmd;
    out[3] = DALY_DATA_LEN;
    for (int i = 4; i < 12; i++) {
        out[i] = 0x00;
    }
    out[12] = daly_checksum(out, 12);
    return DALY_FRAME_LEN;
}

daly_err_t daly_parse(const uint8_t *frame, size_t len, bms_data_t *out)
{
    if (frame == NULL || out == NULL || len < DALY_FRAME_LEN) {
        return DALY_ERR_FRAMING;
    }
    if (frame[0] != DALY_START || frame[3] != DALY_DATA_LEN) {
        return DALY_ERR_FRAMING;
    }
    if (daly_checksum(frame, 12) != frame[12]) {
        return DALY_ERR_CHECKSUM;
    }

    out->vendor = BMS_VENDOR_DALY;
    const uint8_t *d = &frame[4];   /* D0..D7 */
    uint8_t cmd = frame[2];

    switch (cmd) {
    case DALY_CMD_SOC:
        out->pack_mv = (int32_t)be16(&d[0]) * 100;          /* 0.1 V -> mV */
        out->pack_ma = ((int32_t)be16(&d[4]) - 30000) * 100; /* offset, 0.1 A -> mA */
        out->soc_pct = (uint8_t)(be16(&d[6]) / 10u);         /* 0.1 % -> % */
        out->have_summary = true;
        break;

    case DALY_CMD_MINMAXV:
        out->cell_max_mv = be16(&d[0]);
        out->cell_min_mv = be16(&d[3]);
        out->imbalance_mv = (uint16_t)(out->cell_max_mv - out->cell_min_mv);
        break;

    case DALY_CMD_MINMAXT:
        out->temp_dc[0] = (int16_t)(((int)d[0] - 40) * 10); /* max temp, 0.1 degC */
        out->temp_dc[1] = (int16_t)(((int)d[2] - 40) * 10); /* min temp */
        if (out->temp_count < 2) out->temp_count = 2;
        break;

    case DALY_CMD_MOS:
        out->remain_mah = be32(&d[4]);
        out->have_summary = true;
        break;

    case DALY_CMD_STATUS: {
        uint8_t ncell = d[0];
        if (ncell > BMS_MAX_CELLS) ncell = BMS_MAX_CELLS;
        if (ncell > out->cell_count) out->cell_count = ncell;
        break;
    }

    case DALY_CMD_CELLS: {
        uint8_t fno = d[0];              /* 1-based frame number */
        if (fno == 0) return DALY_ERR_FRAMING;
        uint8_t base = (uint8_t)((fno - 1) * 3);
        for (int k = 0; k < 3; k++) {
            uint8_t idx = (uint8_t)(base + k);
            if (idx >= BMS_MAX_CELLS) break;
            uint16_t mv = be16(&d[1 + k * 2]);
            out->cell_mv[idx] = mv;
            if (mv > 0 && (idx + 1) > out->cell_count) {
                out->cell_count = (uint8_t)(idx + 1);
            }
        }
        bms_recalc_cells(out);
        out->have_cells = true;
        break;
    }

    case DALY_CMD_TEMPS: {
        uint8_t fno = d[0];
        if (fno == 0) return DALY_ERR_FRAMING;
        uint8_t base = (uint8_t)((fno - 1) * 7);
        for (int k = 0; k < 7; k++) {
            uint8_t idx = (uint8_t)(base + k);
            if (idx >= BMS_MAX_TEMPS) break;
            out->temp_dc[idx] = (int16_t)(((int)d[1 + k] - 40) * 10);
            if ((idx + 1) > out->temp_count) out->temp_count = (uint8_t)(idx + 1);
        }
        break;
    }

    default:
        break;
    }

    return DALY_OK;
}

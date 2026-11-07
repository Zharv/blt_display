#include "bms_daly_d2.h"

#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

uint16_t daly_d2_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

size_t daly_d2_build_read(uint8_t *out, size_t cap, uint16_t addr, uint16_t count)
{
    if (!out || cap < 8) return 0;
    out[0] = DALY_D2_START;
    out[1] = DALY_D2_FN_READ;
    out[2] = (uint8_t)(addr >> 8);
    out[3] = (uint8_t)addr;
    out[4] = (uint8_t)(count >> 8);
    out[5] = (uint8_t)count;
    uint16_t crc = daly_d2_crc16(out, 6);
    out[6] = (uint8_t)crc;          /* low byte first */
    out[7] = (uint8_t)(crc >> 8);
    return 8;
}

size_t daly_d2_frame_len(const uint8_t *b, size_t have)
{
    if (have < 3 || b[0] != DALY_D2_START) return 0;
    if (b[1] == DALY_D2_FN_READ) return 3u + b[2] + 2u;
    if (b[1] == 0x06) return 8;            /* write echo */
    return 0;
}

daly_d2_err_t daly_d2_check(const uint8_t *f, size_t len)
{
    if (!f || len < 5 || f[0] != DALY_D2_START) return DALY_D2_ERR_FRAMING;
    size_t want = daly_d2_frame_len(f, len);
    if (!want) return DALY_D2_ERR_FRAMING;
    if (len < want) return DALY_D2_INCOMPLETE;
    if (len != want) return DALY_D2_ERR_LEN;
    uint16_t crc = (uint16_t)(f[len - 2] | (f[len - 1] << 8));
    return daly_d2_crc16(f, len - 2) == crc ? DALY_D2_OK : DALY_D2_ERR_CRC;
}

daly_d2_err_t daly_d2_parse_status(const uint8_t *f, size_t len, bms_data_t *o)
{
    if (len != DALY_D2_STATUS_LEN && len != DALY_D2_STATUS80_LEN) return DALY_D2_ERR_LEN;
    daly_d2_err_t e = daly_d2_check(f, len);
    if (e != DALY_D2_OK) return e;
    if (f[1] != DALY_D2_FN_READ) return DALY_D2_ERR_FRAMING;

    memset(o, 0, sizeof *o);
    o->vendor = BMS_VENDOR_DALY;

    int cells = f[102];                                   /* low byte of the word at 101 */
    if (cells > 32) cells = 32;
    if (cells > BMS_MAX_CELLS) cells = BMS_MAX_CELLS;
    o->cell_count = (uint8_t)cells;
    uint16_t lo = 0xFFFF, hi = 0;
    for (int i = 0; i < cells; i++) {
        uint16_t mv = be16(f + 3 + 2 * i);
        o->cell_mv[i] = mv;
        if (mv == 0) continue;
        if (mv < lo) lo = mv;
        if (mv > hi) hi = mv;
    }
    if (hi) {
        o->cell_min_mv = lo;
        o->cell_max_mv = hi;
        o->imbalance_mv = (uint16_t)(hi - lo);
    }

    int temps = f[104];                                   /* low byte of the word at 103 */
    if (temps > 8) temps = 8;
    if (temps > BMS_MAX_TEMPS) temps = BMS_MAX_TEMPS;
    o->temp_count = (uint8_t)temps;
    for (int i = 0; i < temps; i++) o->temp_dc[i] = (int16_t)(((int)be16(f + 67 + 2 * i) - 40) * 10);

    o->pack_mv = (int32_t)be16(f + 83) * 100;             /* 0.1 V */
    o->pack_ma = ((int32_t)be16(f + 85) - 30000) * 100;   /* 0.1 A, offset 30000; + = charging */
    uint16_t soc = be16(f + 87);                           /* 0.1 % */
    o->soc_pct = (uint8_t)(soc >= 1000 ? 100 : (soc + 5) / 10);
    o->remain_mah = (uint32_t)be16(f + 99) * 100;          /* 0.1 Ah */
    o->cycles = be16(f + 105);
    if (len == DALY_D2_STATUS80_LEN)
        o->balance_bits = (uint32_t)be16(f + 127) | ((uint32_t)be16(f + 129) << 16);   /* order unverified */
    o->protection = be16(f + 125);                        /* lowest alarm word */
    o->have_summary = true;
    o->have_cells = cells > 0;
    return DALY_D2_OK;
}

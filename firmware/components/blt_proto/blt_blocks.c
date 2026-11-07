#include "blt_blocks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t xor_of(const uint8_t *b, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= b[i];
    return x;
}

bool blt_block_valid(const uint8_t *b, size_t len)
{
    return b && len >= 2 && b[0] == BLT_BLK_MARK && xor_of(b, len - 1) == b[len - 1];
}

void blt_block_seal(uint8_t *b, size_t len)
{
    if (!b || len < 2) return;
    b[0] = BLT_BLK_MARK;
    b[len - 1] = xor_of(b, len - 1);
}

void blt_id_get_serial(const uint8_t *id, char *out, size_t n)
{
    if (!out || !n) return;
    size_t k = 0;
    for (size_t i = 0; i < BLT_ID_SERIAL_LEN && k + 1 < n; i++) {
        uint8_t c = id[BLT_ID_SERIAL_OFF + i];
        if (c == 0) break;
        out[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = '\0';
}

bool blt_id_set_serial(uint8_t *id, const char *s)
{
    if (!id || !s) return false;
    size_t len = strlen(s);
    if (len == 0 || len > BLT_ID_SERIAL_LEN) return false;
    for (size_t i = 0; i < len; i++)
        if ((uint8_t)s[i] < 0x21 || (uint8_t)s[i] > 0x7E) return false;   /* printable, no spaces */
    memset(id + BLT_ID_SERIAL_OFF, ' ', BLT_ID_SERIAL_LEN);
    memcpy(id + BLT_ID_SERIAL_OFF, s, len);
    blt_block_seal(id, BLT_ID_LEN);
    return true;
}

uint16_t blt_cfg_get_rated_ah(const uint8_t *cfg)
{
    return (uint16_t)(cfg[BLT_CFG_RATED_AH] | (cfg[BLT_CFG_RATED_AH + 1] << 8));
}

bool blt_cfg_set_rated_ah(uint8_t *cfg, uint16_t ah)
{
    if (!cfg || ah == 0 || ah > 9999) return false;
    cfg[BLT_CFG_RATED_AH] = (uint8_t)ah;
    cfg[BLT_CFG_RATED_AH + 1] = (uint8_t)(ah >> 8);
    blt_block_seal(cfg, BLT_CFG_LEN);
    return true;
}

void blt_next_serial(char *out, size_t n, int y, int m, int d, const char *last)
{
    char prefix[16];
    unsigned uy = (unsigned)y % 10000u, um = (unsigned)m % 100u, ud = (unsigned)d % 100u;
    snprintf(prefix, sizeof prefix, "%04u%02u%02u", uy, um, ud);
    int num = 1;
    if (last && strlen(last) == 12 && strncmp(last, prefix, 8) == 0) {
        int prev = atoi(last + 8);
        if (prev >= 0 && prev < 9999) num = prev + 1;
    }
    snprintf(out, n, "%s%04d", prefix, num);
}

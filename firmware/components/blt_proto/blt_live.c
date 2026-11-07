#include "blt_live.h"

#include <string.h>

static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int16_t s16le(const uint8_t *p) { return (int16_t)u16le(p); }
static uint8_t bcd(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }

bool blt_live_decode(const uint8_t *b, size_t len, blt_live_t *o)
{
    if (!b || !o || len < BLT_LIVE_MIN_LEN) return false;
    memset(o, 0, sizeof *o);
    o->v_batt = u16le(b + 0x02) / 10.0f;
    o->v_load = u16le(b + 0x04) / 10.0f;
    o->i_charge = u16le(b + 0x06) / 10.0f;
    o->i_discharge = u16le(b + 0x08) / 10.0f;
    for (int i = 0; i < 4; i++) o->temp_c[i] = s16le(b + 0x0A + 2 * i) / 10.0f;
    o->flags = u16le(b + 0x18);
    o->outputs1 = b[0x1A];
    o->outputs2 = b[0x1B];
    o->rtc_s = bcd(b[0x26]);
    o->rtc_m = bcd(b[0x27]);
    o->rtc_h = bcd(b[0x28]);
    o->rtc_d = bcd(b[0x29]);
    o->rtc_mo = bcd(b[0x2A]);
    o->rtc_y = bcd(b[0x2B]);
    o->t_charge_min = u16le(b + 0x38);
    o->t_discharge_min = u16le(b + 0x3A);
    o->t_charge_pause_min = u16le(b + 0x3C);
    o->t_discharge_pause_min = u16le(b + 0x3E);
    o->n_charges = b[0x40];
    o->n_discharges = b[0x41];
    o->ah_present = u16le(b + 0x48);
    o->ah_charged = u16le(b + 0x4C);
    o->ah_discharged = u16le(b + 0x4E);
    o->efficiency_pct = b[0x52];
    o->v_min = u16le(b + 0x53) / 100.0f;
    o->v_max = u16le(b + 0x55) / 100.0f;
    for (int i = 0; i < 4; i++) o->tmax_c[i] = (int8_t)b[0x5B + i];
    memcpy(o->alarms, b + 0x5F, 6);
    o->state = b[0x6B];
    o->test_no = u16le(b + 0x70);
    o->pr_cycle = u16le(b + 0x6E);
    o->rated_ah = u16le(b + 0x1F);
    o->rated_v = b[0x22];
    o->i_set = b[0x23];
    if (len >= BLT_LIVE_ID_OFF + BLT_LIVE_ID_LEN) {
        int n = 0;
        for (int i = 0; i < 16; i++) {          /* serial = first 16 chars; then 32 chars of text */
            uint8_t c = b[BLT_LIVE_ID_OFF + i];
            if (c == 0) break;
            o->battery_id[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
        while (n > 0 && o->battery_id[n - 1] == ' ') n--;
        o->battery_id[n] = '\0';
    }
    return true;
}

const char *blt_state_name_it(uint8_t s)
{
    static const char *const n[] = {
        "START", "BATTERIA IN SCARICA", "PAUSA DI SCARICA", "BATTERIA IN CARICA",
        "PAUSA DI CARICA", "FINE TEST", "***", "TEST", "COLLAUDO", "IN PROGRAMMAZIONE",
    };
    return s < sizeof n / sizeof n[0] ? n[s] : "?";
}

int blt_live_alarm_count(const blt_live_t *l, int *first)
{
    int cnt = 0;
    if (first) *first = 0;
    for (int byte = 0; byte < 6; byte++)
        for (int bit = 0; bit < 8; bit++)
            if (l->alarms[byte] & (1u << bit)) {
                if (!cnt && first) *first = 8 * byte + bit + 1;
                cnt++;
            }
    return cnt;
}

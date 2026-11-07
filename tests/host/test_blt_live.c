/* Host test for the BLT160 live-block (RI 0x00, 192 B) decoder. */
#include "blt_live.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-3)

static void put16(uint8_t *b, int off, int v) { b[off] = (uint8_t)(v & 0xFF); b[off + 1] = (uint8_t)((v >> 8) & 0xFF); }

int main(void)
{
    uint8_t b[BLT_LIVE_LEN];
    memset(b, 0, sizeof b);
    put16(b, 0x02, 512);      /* 51.2 V */
    put16(b, 0x04, 498);      /* 49.8 V */
    put16(b, 0x06, 0);
    put16(b, 0x08, 1205);     /* 120.5 A discharge */
    put16(b, 0x0A, 253);      /* 25.3 C */
    put16(b, 0x0C, -15);      /* -1.5 C (signed) */
    put16(b, 0x18, 0x2000);   /* discharging */
    b[0x26] = 0x59; b[0x27] = 0x07; b[0x28] = 0x14;      /* 14:07:59 BCD */
    b[0x29] = 0x06; b[0x2A] = 0x11; b[0x2B] = 0x26;      /* 06/11/26     */
    put16(b, 0x3A, 125);      /* 2 h 05 min discharging */
    b[0x41] = 3;              /* cycle 3 */
    put16(b, 0x48, 410);      /* present Ah */
    put16(b, 0x4E, 233);      /* Ah discharged */
    b[0x52] = 0xFB;           /* efficiency n/a */
    put16(b, 0x53, 175);      /* 1.75 */
    b[0x5B] = (uint8_t)-3;    /* -3 C */
    b[0x5F] = 0x00; b[0x61] = 0x08;   /* byte 2 bit 3 -> anomaly id 20 */
    b[0x6B] = BLT_ST_DISCHARGE;
    put16(b, 0x70, 0x1234);
    memset(b + 0x76, ' ', 48);
    memcpy(b + 0x76, "202611070001", 12);

    blt_live_t l;
    CHECK(!blt_live_decode(b, BLT_LIVE_MIN_LEN - 1, &l), "short block must fail");
    CHECK(blt_live_decode(b, sizeof b, &l), "decode");
    CHECK(NEAR(l.v_batt, 51.2), "v_batt %f", l.v_batt);
    CHECK(NEAR(l.v_load, 49.8), "v_load %f", l.v_load);
    CHECK(NEAR(l.i_discharge, 120.5), "i_dis %f", l.i_discharge);
    CHECK(NEAR(l.i_charge, 0), "i_chg %f", l.i_charge);
    CHECK(NEAR(l.temp_c[0], 25.3), "t1 %f", l.temp_c[0]);
    CHECK(NEAR(l.temp_c[1], -1.5), "t2 %f (signed)", l.temp_c[1]);
    CHECK(l.flags & BLT_FLAG_DISCHARGING, "flags %04x", l.flags);
    CHECK(l.rtc_h == 14 && l.rtc_m == 7 && l.rtc_s == 59, "rtc %u:%u:%u", l.rtc_h, l.rtc_m, l.rtc_s);
    CHECK(l.rtc_d == 6 && l.rtc_mo == 11 && l.rtc_y == 26, "date %u/%u/%u", l.rtc_d, l.rtc_mo, l.rtc_y);
    CHECK(l.t_discharge_min == 125, "t_dis %u", l.t_discharge_min);
    CHECK(l.n_discharges == 3, "cycle %u", l.n_discharges);
    CHECK(l.ah_present == 410 && l.ah_discharged == 233, "ah %u %u", l.ah_present, l.ah_discharged);
    CHECK(l.efficiency_pct == 0xFB, "eff");
    CHECK(NEAR(l.v_min, 1.75), "vmin %f", l.v_min);
    CHECK(l.tmax_c[0] == -3, "tmax %d", l.tmax_c[0]);
    CHECK(l.state == BLT_ST_DISCHARGE, "state %u", l.state);
    CHECK(strcmp(blt_state_name_it(l.state), "BATTERIA IN SCARICA") == 0, "state name");
    CHECK(strcmp(blt_state_name_it(42), "?") == 0, "state out of range");
    CHECK(l.test_no == 0x1234, "test %u", l.test_no);
    CHECK(strcmp(l.battery_id, "202611070001") == 0, "battery id '%s'", l.battery_id);
    blt_live_t s2;
    CHECK(blt_live_decode(b, BLT_LIVE_MIN_LEN, &s2) && s2.battery_id[0] == 0, "short block: no id");
    int first = -1;
    CHECK(blt_live_alarm_count(&l, &first) == 1 && first == 20, "alarm first %d", first);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

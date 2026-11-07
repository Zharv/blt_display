/* Host test for the battery-status verdict (components/health). */
#include "health.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* today's battery: 72 V NMC 20S at 100 A, mid-discharge, all normal */
static health_in_t nmc_ok(void)
{
    health_in_t in;
    memset(&in, 0, sizeof in);
    in.discharging = true; in.elapsed_s = 3600;
    in.v_tester = 75.0f; in.i_tester = 100.2f; in.i_set = 100; in.rated_v = 72;
    in.bms = true; in.v_bms = 74.9f; in.cells = 20;
    in.cell_min_mv = 3740; in.cell_max_mv = 3754; in.cell_min_idx = 11;
    in.has_temp = true; in.temp_bat = 26; in.has_mos = true; in.temp_mos = 36;
    in.has_base = true; in.delta0_mv = 10; in.temp0 = 22;
    return in;
}

int main(void)
{
    health_out_t o;
    health_in_t in = nmc_ok();

    health_eval(&in, &o);
    CHECK(o.chem == CHEM_NMC, "72 V / 20S is NMC (%d)", o.chem);
    CHECK(o.level == HL_OK && o.reason == HR_OK, "normal discharge OK (%d/%d)", o.level, o.reason);
    CHECK(o.g[HG_DELTA].valid && o.g[HG_DELTA].val == 14, "delta gauge %f", o.g[HG_DELTA].val);
    CHECK(o.g[HG_CURR].valid && o.g[HG_CURR].level == HL_OK, "current gauge");

    /* the user's NMC example: 10 mV at the start, 40 mV now -> stop */
    in = nmc_ok(); in.cell_min_mv = 3714; in.cell_max_mv = 3754;
    health_eval(&in, &o);
    CHECK(o.level == HL_STOP && o.reason == HR_DELTA_GROW, "NMC 10->40 mV stops (%d/%d)", o.level, o.reason);
    CHECK(o.cell == 12, "names the lowest cell (%d)", o.cell);

    /* moderate growth 10 -> 30: look closer */
    in = nmc_ok(); in.cell_min_mv = 3724; in.cell_max_mv = 3754;
    health_eval(&in, &o);
    CHECK(o.level == HL_WARN, "NMC 10->30 mV warns (%d/%d)", o.level, o.reason);

    /* end of discharge (cells < 3.30 V): imbalance alone only warns */
    in = nmc_ok(); in.cell_min_mv = 3200; in.cell_max_mv = 3260;
    health_eval(&in, &o);
    CHECK(o.level == HL_WARN, "NMC knee caps imbalance at WARN (%d/%d)", o.level, o.reason);
    in.cell_min_mv = 2850;
    health_eval(&in, &o);
    CHECK(o.level == HL_WARN && o.reason == HR_CELL_LOW, "NMC cell < 2.9 V warns (%d/%d)", o.level, o.reason);
    in.cell_min_mv = 2700; in.cell_max_mv = 2740;   /* normal end (tester stop 54 V / 20S) */
    health_eval(&in, &o);
    CHECK(o.level == HL_WARN, "NMC normal end of discharge is not STOP (%d/%d)", o.level, o.reason);
    in.cell_min_mv = 2550;
    health_eval(&in, &o);
    CHECK(o.level == HL_STOP && o.reason == HR_CELL_LOW, "NMC cell < 2.6 V stops (%d/%d)", o.level, o.reason);

    /* the user's LFP examples: 30 -> 60 mV still ok-ish, 100 mV stop */
    in = nmc_ok(); in.rated_v = 51; in.cells = 16; in.v_tester = 52.5f; in.v_bms = 52.4f;
    in.delta0_mv = 30; in.cell_min_mv = 3240; in.cell_max_mv = 3270;
    health_eval(&in, &o);
    CHECK(o.chem == CHEM_LFP, "51 V / 16S is LFP");
    CHECK(o.level == HL_OK, "LFP 30 mV ok (%d/%d)", o.level, o.reason);
    in.cell_max_mv = 3300;                      /* 60 mV, +30 */
    health_eval(&in, &o);
    CHECK(o.level == HL_WARN, "LFP 30->60 mV look closer (%d/%d)", o.level, o.reason);
    in.cell_max_mv = 3340;                      /* 100 mV */
    health_eval(&in, &o);
    CHECK(o.level == HL_STOP, "LFP 100 mV stops (%d/%d)", o.level, o.reason);

    /* current off the set value */
    in = nmc_ok(); in.i_tester = 93;
    health_eval(&in, &o);
    CHECK(o.level == HL_WARN && o.reason == HR_CURRENT, "-7%% current warns (%d/%d)", o.level, o.reason);
    in.i_tester = 85;
    health_eval(&in, &o);
    CHECK(o.level == HL_STOP && o.reason == HR_CURRENT, "-15%% current stops");
    in.elapsed_s = 20;
    health_eval(&in, &o);
    CHECK(o.reason != HR_CURRENT, "current ramp in the first minute is ignored");

    /* temperature, MOS, cable drop, alarms, no BMS */
    in = nmc_ok(); in.temp_bat = 48;
    health_eval(&in, &o);
    CHECK(o.level == HL_WARN && o.reason == HR_TEMP, "48 C warns");
    in.temp_bat = 57;
    health_eval(&in, &o);
    CHECK(o.level == HL_STOP && o.reason == HR_TEMP, "57 C stops");
    in = nmc_ok(); in.temp_bat = 38; in.temp0 = 20;
    health_eval(&in, &o);
    CHECK(o.level == HL_WARN && o.reason == HR_TEMP, "+18 C rise warns");
    in = nmc_ok(); in.temp_mos = 90;
    health_eval(&in, &o);
    CHECK(o.level == HL_STOP && o.reason == HR_MOS, "MOS 90 C stops");
    in = nmc_ok(); in.v_bms = 73.6f;
    health_eval(&in, &o);
    CHECK(o.level == HL_WARN && o.reason == HR_VDROP, "1.4 V tester/BMS gap warns");
    in = nmc_ok(); in.alarms = 1;
    health_eval(&in, &o);
    CHECK(o.level == HL_STOP && o.reason == HR_ALARM, "tester alarm stops");
    in = nmc_ok(); in.bms = false;
    health_eval(&in, &o);
    CHECK(o.level == HL_WARN && o.reason == HR_NO_BMS, "no BMS data warns");

    /* priority: a low cell beats a big imbalance at the same level */
    in = nmc_ok(); in.cell_min_mv = 2550; in.cell_max_mv = 2610; in.delta0_mv = 10;
    health_eval(&in, &o);
    CHECK(o.reason == HR_CELL_LOW, "cell low first (%d)", o.reason);

    /* not discharging: no verdict, gauges still filled */
    in = nmc_ok(); in.discharging = false;
    health_eval(&in, &o);
    CHECK(o.level == HL_NONE && o.reason == HR_IDLE, "idle");
    CHECK(o.g[HG_CELL].valid, "cell gauge while idle");

    /* forced chemistry */
    in = nmc_ok(); in.chem = CHEM_LFP;
    health_eval(&in, &o);
    CHECK(o.chem == CHEM_LFP, "manual LFP");

    printf("test_health: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

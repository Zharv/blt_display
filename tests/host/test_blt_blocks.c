/* Host test for the BLT ID / CFG block helpers (written before a test). */
#include "blt_blocks.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* the real ID block read from the tester on 2026-11-07 (RD 0x40, 50 B):
     * A5 "202611070001" + 36 spaces + checksum A5 */
    uint8_t id[BLT_ID_LEN];
    id[0] = 0xA5;
    memset(id + 1, ' ', 48);
    memcpy(id + 1, "202611070001", 12);
    id[49] = 0xA5;
    CHECK(blt_block_valid(id, sizeof id), "real ID block must validate");

    char s[20];
    blt_id_get_serial(id, s, sizeof s);
    CHECK(strcmp(s, "202611070001") == 0, "serial '%s'", s);

    memcpy(id + 0x11, "CUSTOMER TEST", 13);       /* text field must survive */
    blt_block_seal(id, sizeof id);
    CHECK(blt_id_set_serial(id, "202611070002"), "set serial");
    CHECK(blt_block_valid(id, sizeof id), "resealed");
    blt_id_get_serial(id, s, sizeof s);
    CHECK(strcmp(s, "202611070002") == 0, "new serial '%s'", s);
    CHECK(memcmp(id + 0x11, "CUSTOMER TEST", 13) == 0, "text untouched");
    CHECK(id[1 + 12] == ' ' && id[16] == ' ', "padded with spaces");

    CHECK(!blt_id_set_serial(id, ""), "empty rejected");
    CHECK(!blt_id_set_serial(id, "12345678901234567"), "17 chars rejected");
    CHECK(!blt_id_set_serial(id, "2026 1107"), "space rejected");

    id[5] ^= 1;
    CHECK(!blt_block_valid(id, sizeof id), "corrupted block rejected");

    /* CFG: only the rated capacity changes */
    uint8_t cfg[BLT_CFG_LEN], before[BLT_CFG_LEN];
    for (unsigned i = 0; i < sizeof cfg; i++) cfg[i] = (uint8_t)(i * 7 + 3);
    cfg[3] = 0x2C; cfg[4] = 0x01;                 /* 300 Ah */
    blt_block_seal(cfg, sizeof cfg);
    memcpy(before, cfg, sizeof cfg);
    CHECK(blt_cfg_get_rated_ah(cfg) == 300, "rated %u", blt_cfg_get_rated_ah(cfg));
    CHECK(blt_cfg_set_rated_ah(cfg, 280), "set 280");
    CHECK(blt_cfg_get_rated_ah(cfg) == 280, "rated now %u", blt_cfg_get_rated_ah(cfg));
    CHECK(blt_block_valid(cfg, sizeof cfg), "cfg resealed");
    int diff = 0;
    for (unsigned i = 0; i < sizeof cfg - 1; i++) if (i != 3 && i != 4 && cfg[i] != before[i]) diff++;
    CHECK(diff == 0, "%d other bytes changed", diff);
    CHECK(!blt_cfg_set_rated_ah(cfg, 0) && !blt_cfg_set_rated_ah(cfg, 10000), "range");

    char nx[16];
    blt_next_serial(nx, sizeof nx, 2026, 11, 7, "202611070001");
    CHECK(strcmp(nx, "202611070002") == 0, "next same day '%s'", nx);
    blt_next_serial(nx, sizeof nx, 2026, 11, 8, "202611070004");
    CHECK(strcmp(nx, "202611080001") == 0, "next day '%s'", nx);
    blt_next_serial(nx, sizeof nx, 2026, 11, 8, "");
    CHECK(strcmp(nx, "202611080001") == 0, "no last '%s'", nx);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#include "test_sum.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "freertos/FreeRTOS.h"

/* RAM copy of the NVS slots, loaded on first use (from an internal-RAM task:
 * the LVGL tick), so readers on PSRAM-stack tasks never touch the flash. */
static test_sum_t s_slot[TEST_SUM_SLOTS];
static bool s_loaded;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void key_of(uint16_t test, char *k)
{
    snprintf(k, 8, "s%02u", (unsigned)(test % TEST_SUM_SLOTS));
}

void test_sum_load(void)
{
    if (s_loaded) return;
    nvs_handle_t h;
    if (nvs_open("tsum", NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < TEST_SUM_SLOTS; i++) {
            char k[8];
            snprintf(k, sizeof k, "s%02d", i);
            size_t n = sizeof s_slot[i];
            if (nvs_get_blob(h, k, &s_slot[i], &n) != ESP_OK || n != sizeof s_slot[i]) memset(&s_slot[i], 0, sizeof s_slot[i]);
        }
        nvs_close(h);
    }
    s_loaded = true;
}

bool test_sum_get(uint16_t test, test_sum_t *out)
{
    if (!s_loaded) return false;
    taskENTER_CRITICAL(&s_mux);
    *out = s_slot[test % TEST_SUM_SLOTS];
    taskEXIT_CRITICAL(&s_mux);
    return out->test == test && out->cells;
}

void test_sum_put(const test_sum_t *s)
{
    test_sum_load();
    taskENTER_CRITICAL(&s_mux);
    s_slot[s->test % TEST_SUM_SLOTS] = *s;
    taskEXIT_CRITICAL(&s_mux);
    char k[8];
    key_of(s->test, k);
    nvs_handle_t h;
    if (nvs_open("tsum", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, k, s, sizeof *s);
    nvs_commit(h);
    nvs_close(h);
}

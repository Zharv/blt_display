#include "test_sum.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"

static void key_of(uint16_t test, char *k)
{
    snprintf(k, 8, "s%02u", (unsigned)(test % TEST_SUM_SLOTS));
}

bool test_sum_get(uint16_t test, test_sum_t *out)
{
    char k[8];
    key_of(test, k);
    nvs_handle_t h;
    if (nvs_open("tsum", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sizeof *out;
    bool ok = nvs_get_blob(h, k, out, &n) == ESP_OK && n == sizeof *out && out->test == test;
    nvs_close(h);
    return ok;
}

void test_sum_put(const test_sum_t *s)
{
    char k[8];
    key_of(s->test, k);
    nvs_handle_t h;
    if (nvs_open("tsum", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, k, s, sizeof *s);
    nvs_commit(h);
    nvs_close(h);
}

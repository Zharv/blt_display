#include "sn_fix.h"

#include <string.h>

#include "nvs.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    uint16_t cycle;
    char     sn[18];
} fix_t;

static fix_t s_tab[SN_FIX_MAX];      /* cycle 0 = free; oldest first */
static bool s_loaded;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void sn_fix_load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open("snfix", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s_tab;
    if (nvs_get_blob(h, "tab", s_tab, &n) != ESP_OK || n != sizeof s_tab) memset(s_tab, 0, sizeof s_tab);
    nvs_close(h);
}

bool sn_fix_get(uint16_t cycle, char *out, size_t n)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < SN_FIX_MAX; i++)
        if (s_tab[i].cycle == cycle && cycle && s_tab[i].sn[0]) {
            strlcpy(out, s_tab[i].sn, n);
            ok = true;
            break;
        }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

void sn_fix_set(uint16_t cycle, const char *sn)
{
    sn_fix_load();
    taskENTER_CRITICAL(&s_mux);
    int slot = -1;
    for (int i = 0; i < SN_FIX_MAX; i++) if (s_tab[i].cycle == cycle) { slot = i; break; }
    if (slot >= 0) {                                   /* drop the old entry */
        memmove(&s_tab[slot], &s_tab[slot + 1], (SN_FIX_MAX - slot - 1) * sizeof s_tab[0]);
        memset(&s_tab[SN_FIX_MAX - 1], 0, sizeof s_tab[0]);
    }
    if (sn && sn[0]) {                                 /* append, the oldest falls out when full */
        int k = 0;
        while (k < SN_FIX_MAX && s_tab[k].cycle) k++;
        if (k == SN_FIX_MAX) {
            memmove(&s_tab[0], &s_tab[1], (SN_FIX_MAX - 1) * sizeof s_tab[0]);
            k = SN_FIX_MAX - 1;
        }
        s_tab[k].cycle = cycle;
        strlcpy(s_tab[k].sn, sn, sizeof s_tab[k].sn);
    }
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open("snfix", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "tab", s_tab, sizeof s_tab);
        nvs_commit(h);
        nvs_close(h);
    }
}

const char *sn_fix_apply(uint16_t cycle, const char *tester_sn, char *buf, size_t n)
{
    return sn_fix_get(cycle, buf, n) ? buf : tester_sn;
}

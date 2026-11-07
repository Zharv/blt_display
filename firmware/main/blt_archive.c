#include "blt_archive.h"
#include "blt_usb.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "blt_arch";

esp_err_t blt_archive_fetch_current(blt_test_t *t)
{
    static uint8_t buf[BLT_REC_LEN];
    memset(t, 0, sizeof *t);

    /* the live block is the current cycle record */
    esp_err_t e = blt_usb_read_block(BLT_CMD_RI, 0, buf, BLT_REC_LEN);
    if (e != ESP_OK) return e;
    blt_rec_decode(buf, BLT_REC_LEN, &t->rec);
    if (blt_usb_read_block(BLT_CMD_RD, 0x10, buf, 16) == ESP_OK) {
        int k = 0;
        for (int i = 0; i < 16 && buf[i] >= 0x20 && buf[i] < 0x7F; i++) t->tester_sn[k++] = (char)buf[i];
        while (k && t->tester_sn[k - 1] == ' ') k--;
        t->tester_sn[k] = 0;
    }
    const uint16_t cyc = t->rec.pr_cycle;
    const uint32_t top = t->rec.pr_page;
    const uint32_t bottom = top > 249 ? top - 249 : 0;
    ESP_LOGI(TAG, "cycle %u test %u state %u, pages %lu..%lu",
             cyc, t->rec.pr_test, t->rec.state, (unsigned long)bottom, (unsigned long)top);

    /* pages newest -> oldest until a page holds only older cycles */
    blt_page_t *pg = heap_caps_malloc(250 * sizeof(blt_page_t), MALLOC_CAP_SPIRAM);
    if (!pg) return ESP_ERR_NO_MEM;
    int np = 0;
    for (uint32_t p = top + 1; p-- > bottom;) {
        e = blt_usb_read_block(BLT_CMD_RD, BLT_PAGE_ADDR(p), buf, BLT_PAGE_LEN);
        if (e != ESP_OK) { ESP_LOGW(TAG, "page %lu: %s", (unsigned long)p, esp_err_to_name(e)); break; }
        blt_page_t *g = &pg[np];
        blt_page_decode(buf, BLT_PAGE_LEN, g);
        bool mine = false, newer = false;
        for (int k = 0; k < g->n; k++) {
            if (g->tag[k] == cyc) mine = true;
            if (g->tag[k] > cyc) newer = true;
        }
        if (mine) np++;
        else if (p != top && !newer) break;     /* older cycle (or empty) reached */
    }
    /* oldest first */
    int n = 0;
    for (int j = np - 1; j >= 0 && n < BLT_ARCH_MAX; j--) {
        const blt_page_t *g = &pg[j];
        int32_t base = blt_minutes(g->y, g->mo, g->d, g->h, g->mi);
        for (int k = 0; k < g->n && n < BLT_ARCH_MAX; k++) {
            if (g->tag[k] != cyc) continue;
            int32_t abs_t = base + k * (g->interval_min ? g->interval_min : 1);
            if (n == 0) {
                t->t0_abs = abs_t;
                int32_t m = abs_t % 1440;
                t->t0_h = (uint8_t)(m / 60); t->t0_mi = (uint8_t)(m % 60);
                t->t0_d = g->d; t->t0_mo = g->mo; t->t0_y = g->y;
            }
            int32_t dt = abs_t - t->t0_abs;
            t->t_min[n] = (uint16_t)(dt < 0 ? 0 : dt > 65535 ? 65535 : dt);
            t->v[n] = g->v[k];
            t->i[n] = g->i[k] < 0 ? -g->i[k] : g->i[k];
            n++;
        }
    }
    t->n = n;
    heap_caps_free(pg);
    ESP_LOGI(TAG, "%d pages, %d samples%s", np, n, n ? "" : " (no graph yet)");
    return ESP_OK;
}

#include "blt_archive.h"
#include "blt_usb.h"
#include "sd_arch.h"
#include "sd_store.h"

#include <stdio.h>

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "blt_arch";

bool blt_rec_has_result(const blt_rec_t *r)
{
    /* a real discharge: start date, at least 5 minutes and some Ah (a start
     * stopped at once by the battery is not a test result) */
    return r->dis_d && r->dis_mo && r->t_dis_min >= 5 && r->ah_dis > 0;
}

static bool all_ff(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) if (b[i] != 0xFF) return false;
    return true;
}

/* live block = the current cycle record */
static uint8_t s_raw[BLT_REC_LEN];     /* last record read (read_live / read_rec) */

static esp_err_t read_live(blt_rec_t *rec)
{
    esp_err_t e = blt_usb_read_block(BLT_CMD_RI, 0, s_raw, BLT_REC_LEN);
    if (e == ESP_OK) blt_rec_decode(s_raw, BLT_REC_LEN, rec);
    return e;
}

/* stored cycle record n; ESP_ERR_NOT_FOUND if the slot holds another one */
static esp_err_t read_rec(uint16_t n, blt_rec_t *rec)
{
    esp_err_t e = blt_usb_read_block(BLT_CMD_RD, BLT_REC_ADDR(n), s_raw, BLT_REC_LEN);
    if (e != ESP_OK) return e;
    if (all_ff(s_raw, BLT_REC_LEN)) return ESP_ERR_NOT_FOUND;
    blt_rec_decode(s_raw, BLT_REC_LEN, rec);
    return rec->pr_cycle == n ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t blt_archive_fetch(uint16_t want, blt_test_t *t)
{
    static uint8_t buf[BLT_PAGE_LEN];
    memset(t, 0, sizeof *t);

    blt_rec_t live;
    esp_err_t e = read_live(&live);
    if (e != ESP_OK) return e;
    if (want == BLT_ARCH_CURRENT || want == live.pr_cycle) {
        t->rec = live;
        t->current = true;
    } else {
        e = read_rec(want, &t->rec);
        if (e != ESP_OK) return e;
    }
    memcpy(t->raw, s_raw, BLT_REC_LEN);
    if (blt_usb_read_block(BLT_CMD_RD, 0x10, buf, 16) == ESP_OK) {
        int k = 0;
        for (int i = 0; i < 16 && buf[i] >= 0x20 && buf[i] < 0x7F; i++) t->tester_sn[k++] = (char)buf[i];
        while (k && t->tester_sn[k - 1] == ' ') k--;
        t->tester_sn[k] = 0;
    }
    const uint16_t cyc = t->rec.pr_cycle;
    const uint32_t top = live.pr_page;
    const uint32_t bottom = top > 249 ? top - 249 : 0;
    /* pages of a finished cycle end around its own page pointer (a pause after
     * the discharge may add a page or two) */
    uint32_t start = t->current ? top : (t->rec.pr_page + 2 < top ? t->rec.pr_page + 2 : top);
    ESP_LOGI(TAG, "cycle %u test %u state %u, pages %lu..%lu", cyc, t->rec.pr_test, t->rec.state,
             (unsigned long)bottom, (unsigned long)start);

    /* pages newest -> oldest until a page holds only older cycles */
    blt_page_t *pg = heap_caps_malloc(250 * sizeof(blt_page_t), MALLOC_CAP_SPIRAM);
    if (!pg) return ESP_ERR_NO_MEM;
    int np = 0;
    bool seen = false;
    if (start >= bottom) {
        for (uint32_t p = start + 1; p-- > bottom && np < 250;) {
            e = blt_usb_read_block(BLT_CMD_RD, BLT_PAGE_ADDR(p), buf, BLT_PAGE_LEN);
            if (e != ESP_OK) { ESP_LOGW(TAG, "page %lu: %s", (unsigned long)p, esp_err_to_name(e)); break; }
            blt_page_t *g = &pg[np];
            blt_page_decode(buf, BLT_PAGE_LEN, g);
            bool mine = false, newer = false;
            for (int k = 0; k < g->n; k++) {
                if (g->tag[k] == cyc) mine = true;
                if (g->tag[k] > cyc) newer = true;
            }
            if (mine) { np++; seen = true; }
            else if (newer || (p == top && g->n == 0)) continue;   /* later cycles / page being filled */
            else if (seen || p + 3 < start) break;                 /* older cycle reached */
        }
    }
    e = ESP_OK;
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
    ESP_LOGI(TAG, "%d pages, %d samples%s", np, n, n ? "" : " (graph overwritten or empty)");
    return e;
}

/* records already seen without a result: a finished record never changes, so
 * these are not read again (the current one always is) */
static uint16_t s_empty[150];
static int s_empty_n;

static bool known_empty(uint16_t n)
{
    for (int i = 0; i < s_empty_n; i++) if (s_empty[i] == n) return true;
    return false;
}

static bool s_empty_dirty, s_empty_loaded;

static void mark_empty(uint16_t n)
{
    if (known_empty(n)) return;
    if (s_empty_n < 150) s_empty[s_empty_n++] = n;
    else { memmove(s_empty, s_empty + 1, sizeof s_empty - sizeof s_empty[0]); s_empty[149] = n; }
    s_empty_dirty = true;
}

/* the empty-record list survives restarts on the SD card */
#define EMPTY_FILE SD_DIR "/empty.bin"

static void empty_load(void)
{
    if (s_empty_loaded || !sd_store_ready()) return;
    s_empty_loaded = true;
    FILE *f = fopen(EMPTY_FILE, "rb");
    if (!f) return;
    s_empty_n = (int)fread(s_empty, sizeof s_empty[0], 150, f);
    fclose(f);
}

static void empty_save(void)
{
    if (!s_empty_dirty || !sd_store_ready()) return;
    FILE *f = fopen(EMPTY_FILE, "wb");
    if (!f) return;
    fwrite(s_empty, sizeof s_empty[0], (size_t)s_empty_n, f);
    fclose(f);
    s_empty_dirty = false;
}

esp_err_t blt_archive_list(blt_arch_entry_t *cache, int *cache_n, int max,
                           void (*progress)(int done, int total))
{
    blt_rec_t live;
    esp_err_t e = read_live(&live);
    if (e != ESP_OK) return e;
    const uint16_t top = live.pr_cycle;
    int nact = live.n_active ? live.n_active : 1;
    if (nact > 150) nact = 150;

    blt_arch_entry_t *out = heap_caps_malloc((size_t)max * sizeof *out, MALLOC_CAP_SPIRAM);
    if (!out) return ESP_ERR_NO_MEM;
    /* finished records already on the SD card are taken from there */
    empty_load();
    const int SD_MAX = 600;
    blt_arch_entry_t *sd = heap_caps_malloc(SD_MAX * sizeof *sd, MALLOC_CAP_SPIRAM);
    int sd_n = sd ? sd_arch_list(sd, SD_MAX) : 0;
    int no = 0, reads = 0, from_sd = 0;
    for (int k = 0; k < nact && no < max; k++) {
        uint16_t n = (uint16_t)(top - k);
        blt_arch_entry_t ent = { .n = n, .current = (k == 0), .on_tester = true };
        if (k == 0) {
            ent.rec = live;
        } else {
            int hit = -1;
            for (int c = 0; c < *cache_n; c++)
                if (cache[c].n == n && !cache[c].current) { hit = c; break; }
            int sd_hit = -1;
            if (hit < 0)
                for (int c = 0; c < sd_n; c++)
                    if (sd[c].n == n) { sd_hit = c; break; }
            if (hit >= 0) {
                ent.rec = cache[hit].rec;
            } else if (sd_hit >= 0) {
                ent.rec = sd[sd_hit].rec;
                from_sd++;
            } else if (known_empty(n)) {
                if (progress) progress(k + 1, nact);
                continue;
            } else {
                e = read_rec(n, &ent.rec);
                reads++;
                if (e == ESP_ERR_NOT_FOUND) { e = ESP_OK; mark_empty(n); if (progress) progress(k + 1, nact); continue; }
                if (e != ESP_OK) break;
                if (!blt_rec_has_result(&ent.rec)) mark_empty(n);
            }
        }
        if (blt_rec_has_result(&ent.rec)) {
            out[no++] = ent;
            ESP_LOGI(TAG, "  #%u test %u %02u.%02u.%02u %s %u Ah %u min", n, ent.rec.pr_test, ent.rec.dis_d,
                     ent.rec.dis_mo, ent.rec.dis_y, ent.rec.battery_id, ent.rec.ah_dis, ent.rec.t_dis_min);
        }
        if (progress) progress(k + 1, nact);
        if (n == 0) break;
    }
    if (e == ESP_OK) {
        memcpy(cache, out, (size_t)no * sizeof *out);
        *cache_n = no;
    }
    heap_caps_free(out);
    heap_caps_free(sd);
    empty_save();
    ESP_LOGI(TAG, "list: %d cycles checked (%d read from the tester, %d from SD), %d with results",
             nact, reads, from_sd, no);
    return e;
}

#include "sd_arch.h"
#include "sd_store.h"
#include "test_sum.h"
#include "blt_usb.h"

#include <dirent.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psram_task.h"

static const char *TAG = "sd_arch";

#define GRAPH_MAGIC 0x31524742u      /* "BGR1" */

typedef struct {
    uint32_t magic;
    uint16_t n;
    uint8_t  t0_h, t0_mi, t0_d, t0_mo, t0_y, pad;
    char     tester_sn[17];
    uint8_t  pad2[3];
} graph_hdr_t;

static void dir_of(uint16_t test, uint16_t cycle, char *out, size_t n)
{
    snprintf(out, n, SD_DIR "/T%u_%u", (unsigned)test, (unsigned)cycle);
}

/* saved = a record with a discharge result (an empty one may be overwritten) */
bool sd_arch_has(uint16_t test, uint16_t cycle)
{
    if (!sd_store_ready()) return false;
    char p[64];
    dir_of(test, cycle, p, sizeof p);
    strlcat(p, "/record.bin", sizeof p);
    uint8_t raw[BLT_REC_LEN];
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t r = fread(raw, 1, sizeof raw, f);
    fclose(f);
    if (r != sizeof raw) return false;
    blt_rec_t rec;
    blt_rec_decode(raw, sizeof raw, &rec);
    return blt_rec_has_result(&rec);
}

/* "12.3" with a decimal comma */
static void fcomma(char *b, size_t n, const char *fmt, double v)
{
    snprintf(b, n, fmt, v);
    for (char *c = b; *c; c++) if (*c == '.') *c = ',';
}

static void write_info(const char *dir, const blt_test_t *t, const test_sum_t *s)
{
    char p[80];
    snprintf(p, sizeof p, "%s/info.txt", dir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    const blt_rec_t *r = &t->rec;
    fprintf(f, "Тестер: BLT160 %s\r\nТест: %u, цикл %u\r\nСерійний номер батареї: %s\r\n",
            t->tester_sn, r->pr_test, r->pr_cycle, r->battery_id);
    fprintf(f, "Початок розряду: %02u.%02u.20%02u %02u:%02u\r\nЧас розряду: %u:%02u\r\n",
            r->dis_d, r->dis_mo, r->dis_y, r->dis_h, r->dis_mi, r->t_dis_min / 60, r->t_dis_min % 60);
    fprintf(f, "Знята ємність: %u А·год з %u (номінал %u В)\r\nСтрум розряду: %u А, кінець %.1f В\r\n",
            r->ah_dis, r->rated_ah, r->rated_v, r->discharge_a, blt_rec_stop_v(r));
    fprintf(f, "Точок графіка: %d\r\n", t->n);
    if (s && s->cells)
        fprintf(f, "BMS: %s, %u комірок, розбаланс старт %u мВ, макс. %u мВ, комірки в кінці %.3f..%.3f В\r\n",
                s->bms, s->cells, s->delta0_mv, s->delta_max_mv, s->cell_min_mv / 1000.0, s->cell_max_mv / 1000.0);
    fclose(f);
}

esp_err_t sd_arch_save(const blt_test_t *t)
{
    if (!sd_store_ready()) return ESP_ERR_INVALID_STATE;
    if (!blt_rec_has_result(&t->rec)) return ESP_ERR_INVALID_ARG;   /* nothing to keep */
    char dir[48], p[80];
    dir_of(t->rec.pr_test, t->rec.pr_cycle, dir, sizeof dir);
    mkdir(dir, 0775);

    /* graph first, record last: record.bin present = folder complete */
    snprintf(p, sizeof p, "%s/graph.bin", dir);
    FILE *f = fopen(p, "wb");
    if (!f) { ESP_LOGE(TAG, "create %s failed", p); return ESP_FAIL; }
    graph_hdr_t h = { .magic = GRAPH_MAGIC, .n = (uint16_t)t->n, .t0_h = t->t0_h, .t0_mi = t->t0_mi,
                      .t0_d = t->t0_d, .t0_mo = t->t0_mo, .t0_y = t->t0_y };
    strlcpy(h.tester_sn, t->tester_sn, sizeof h.tester_sn);
    fwrite(&h, sizeof h, 1, f);
    fwrite(t->t_min, sizeof t->t_min[0], (size_t)t->n, f);
    fwrite(t->v, sizeof t->v[0], (size_t)t->n, f);
    fwrite(t->i, sizeof t->i[0], (size_t)t->n, f);
    fclose(f);

    snprintf(p, sizeof p, "%s/graph.csv", dir);
    f = fopen(p, "w");
    if (f) {
        fprintf(f, "хвилина;час;напруга, В;струм, А\r\n");
        int c0 = t->t0_h * 60 + t->t0_mi;
        for (int k = 0; k < t->n; k++) {
            char v[16], a[16];
            fcomma(v, sizeof v, "%.1f", t->v[k]);
            fcomma(a, sizeof a, "%.1f", t->i[k]);
            int m = c0 + t->t_min[k];
            fprintf(f, "%u;%02d:%02d;%s;%s\r\n", t->t_min[k], (m / 60) % 24, m % 60, v, a);
        }
        fclose(f);
    }

    test_sum_t s;
    bool have_s = test_sum_get(t->rec.pr_test, &s);
    if (have_s) {
        snprintf(p, sizeof p, "%s/summary.bin", dir);
        if ((f = fopen(p, "wb"))) { fwrite(&s, sizeof s, 1, f); fclose(f); }
    }
    write_info(dir, t, have_s ? &s : NULL);

    snprintf(p, sizeof p, "%s/record.bin", dir);
    f = fopen(p, "wb");
    if (!f) return ESP_FAIL;
    size_t w = fwrite(t->raw, 1, BLT_REC_LEN, f);
    fclose(f);
    ESP_LOGI(TAG, "saved %s (%d graph points)", dir, t->n);
    return w == BLT_REC_LEN ? ESP_OK : ESP_FAIL;
}

/* folder of a cycle (test number unknown): scan for "_<cycle>" */
static bool find_dir(uint16_t cycle, char *out, size_t n)
{
    DIR *d = opendir(SD_DIR);
    if (!d) return false;
    char suf[12];
    snprintf(suf, sizeof suf, "_%u", (unsigned)cycle);
    size_t sl = strlen(suf);
    bool ok = false;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (de->d_name[0] == 'T' && l > sl && strcmp(de->d_name + l - sl, suf) == 0) {
            snprintf(out, n, SD_DIR "/%.40s", de->d_name);
            ok = true;
            break;
        }
    }
    closedir(d);
    return ok;
}

esp_err_t sd_arch_load(uint16_t cycle, blt_test_t *t)
{
    if (!sd_store_ready()) return ESP_ERR_INVALID_STATE;
    char dir[64], p[96];
    if (!find_dir(cycle, dir, sizeof dir)) return ESP_ERR_NOT_FOUND;
    memset(t, 0, sizeof *t);
    snprintf(p, sizeof p, "%s/record.bin", dir);
    FILE *f = fopen(p, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    size_t r = fread(t->raw, 1, BLT_REC_LEN, f);
    fclose(f);
    if (r != BLT_REC_LEN) return ESP_ERR_INVALID_SIZE;
    blt_rec_decode(t->raw, BLT_REC_LEN, &t->rec);
    snprintf(p, sizeof p, "%s/graph.bin", dir);
    if ((f = fopen(p, "rb"))) {
        graph_hdr_t h;
        if (fread(&h, sizeof h, 1, f) == 1 && h.magic == GRAPH_MAGIC && h.n <= BLT_ARCH_MAX) {
            t->n = h.n;
            t->t0_h = h.t0_h; t->t0_mi = h.t0_mi; t->t0_d = h.t0_d; t->t0_mo = h.t0_mo; t->t0_y = h.t0_y;
            strlcpy(t->tester_sn, h.tester_sn, sizeof t->tester_sn);
            if (fread(t->t_min, sizeof t->t_min[0], h.n, f) != h.n ||
                fread(t->v, sizeof t->v[0], h.n, f) != h.n ||
                fread(t->i, sizeof t->i[0], h.n, f) != h.n) t->n = 0;
        }
        fclose(f);
    }
    return ESP_OK;
}

static int cmp_entry(const void *a, const void *b)
{
    const blt_arch_entry_t *x = a, *y = b;
    return (int)y->rec.pr_cycle - (int)x->rec.pr_cycle;
}

int sd_arch_list(blt_arch_entry_t *out, int max)
{
    if (!sd_store_ready()) return 0;
    DIR *d = opendir(SD_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    uint8_t raw[BLT_REC_LEN];
    char p[96];
    while ((de = readdir(d)) && n < max) {
        if (de->d_name[0] != 'T') continue;
        snprintf(p, sizeof p, SD_DIR "/%.48s/record.bin", de->d_name);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        size_t r = fread(raw, 1, sizeof raw, f);
        fclose(f);
        if (r != sizeof raw) continue;
        memset(&out[n], 0, sizeof out[n]);
        blt_rec_decode(raw, sizeof raw, &out[n].rec);
        if (!blt_rec_has_result(&out[n].rec)) continue;
        out[n].n = out[n].rec.pr_cycle;
        out[n].on_sd = true;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof out[0], cmp_entry);
    return n;
}

/* ------------------------------------------------------------ BMS log */

void sd_arch_log(const online_sample_t *o, uint16_t cycle)
{
    if (!sd_store_ready()) return;
    char dir[48], p[80];
    dir_of(o->test_no, cycle, dir, sizeof dir);
    snprintf(p, sizeof p, "%s/bms.csv", dir);
    struct stat st;
    bool fresh = stat(p, &st) != 0;
    if (fresh) mkdir(dir, 0775);
    FILE *f = fopen(p, "a");
    if (!f) return;
    if (fresh) {
        fprintf(f, "секунда;тестер, В;тестер, А;BMS, В;BMS, А;SOC, %%;T1;T2;MOS");
        for (int c = 0; c < o->cell_count; c++) fprintf(f, ";комірка %d, мВ", c + 1);
        fprintf(f, "\r\n");
    }
    char a[16], b[16], c2[16], d[16];
    fcomma(a, sizeof a, "%.2f", o->voltage_v);
    fcomma(b, sizeof b, "%.1f", o->current_a);
    fcomma(c2, sizeof c2, "%.2f", o->bms_live ? o->bms_pack_v : 0);
    fcomma(d, sizeof d, "%.1f", o->bms_live ? o->bms_current_a : 0);
    fprintf(f, "%lu;%s;%s;%s;%s;%u", (unsigned long)o->elapsed_s, a, b, c2, d, o->bms_live ? o->soc_pct : 0);
    for (int k = 0; k < 3; k++) {
        if (o->bms_live && k < o->temp_count) { fcomma(a, sizeof a, "%.1f", o->temp_dc[k] / 10.0); fprintf(f, ";%s", a); }
        else fprintf(f, ";");
    }
    if (o->bms_live)
        for (int c = 0; c < o->cell_count; c++) fprintf(f, ";%u", o->cell_mv[c]);
    fprintf(f, "\r\n");
    fclose(f);
}

static float pf(char **s)
{
    char *e = strchr(*s, ';');
    if (e) *e = 0;
    for (char *c = *s; *c; c++) if (*c == ',') *c = '.';
    float v = strtof(*s, NULL);
    *s = e ? e + 1 : *s + strlen(*s);
    return v;
}

int sd_arch_read_log(uint16_t test, uint16_t cycle, hist_pt_t *out, int max)
{
    if (!sd_store_ready()) return 0;
    char p[80];
    dir_of(test, cycle, p, sizeof p);
    strlcat(p, "/bms.csv", sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char *line = malloc(1024);
    int n = 0;
    if (line) {
        fgets(line, 1024, f);                       /* header */
        while (n < max && fgets(line, 1024, f)) {
            char *s = line;
            hist_pt_t *h = &out[n];
            memset(h, 0, sizeof *h);
            h->t_s = (uint32_t)pf(&s);
            h->v_t = pf(&s); h->i_t = pf(&s);
            h->v_b = pf(&s); h->i_b = pf(&s);
            pf(&s); pf(&s); pf(&s); pf(&s);         /* SOC, T1, T2, MOS */
            int c = 0;
            while (*s && *s != '\r' && *s != '\n' && c < UI_MAX_CELLS) h->cell[c++] = (uint16_t)pf(&s);
            h->n = (uint8_t)c;
            if (h->v_t > 0) n++;
        }
        free(line);
    }
    fclose(f);
    return n;
}

/* ------------------------------------------------------------ sync */

static volatile bool s_sync_busy;
static volatile int s_sync_done, s_sync_total;
static TaskHandle_t s_sync_task;

static void sync_task(void *arg)
{
    (void)arg;
    blt_arch_entry_t *list = heap_caps_calloc(150, sizeof *list, MALLOC_CAP_SPIRAM);
    blt_test_t *t = heap_caps_malloc(sizeof *t, MALLOC_CAP_SPIRAM);
    int list_n = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!list || !t || !sd_store_ready() || !blt_usb_reads_allowed() ||
            blt_usb_state(NULL, NULL) != BLT_USB_CONNECTED) continue;
        s_sync_busy = true;
        s_sync_done = s_sync_total = 0;
        if (blt_archive_list(list, &list_n, 150, NULL) == ESP_OK) {
            int todo = 0;
            for (int k = 0; k < list_n; k++)
                if (!sd_arch_has(list[k].rec.pr_test, list[k].n)) todo++;
            s_sync_total = todo;
            ESP_LOGI(TAG, "sync: %d of %d tests not on the card yet", todo, list_n);
            for (int k = 0; k < list_n; k++) {
                const blt_arch_entry_t *e = &list[k];
                /* the current cycle only once the tester is done with it */
                if (e->current && e->rec.state >= 1 && e->rec.state <= 4) continue;
                if (sd_arch_has(e->rec.pr_test, e->n)) continue;
                if (blt_archive_fetch(e->current ? BLT_ARCH_CURRENT : e->n, t) == ESP_OK) sd_arch_save(t);
                s_sync_done++;
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        s_sync_busy = false;
        ESP_LOGI(TAG, "sync done");
    }
}

void sd_arch_request_sync(void)
{
    if (!s_sync_task) psram_task(sync_task, "sd_sync", 6144, NULL, 3, &s_sync_task, tskNO_AFFINITY);
    if (s_sync_task) xTaskNotifyGive(s_sync_task);
}

bool sd_arch_sync_busy(int *done, int *total)
{
    if (done) *done = s_sync_done;
    if (total) *total = s_sync_total;
    return s_sync_busy;
}

bool sd_arch_get_sum(uint16_t cycle, test_sum_t *out)
{
    char dir[64], p[96];
    if (!sd_store_ready() || !find_dir(cycle, dir, sizeof dir)) return false;
    snprintf(p, sizeof p, "%s/summary.bin", dir);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    memset(out, 0, sizeof *out);
    out->room_dc = INT16_MIN;
    size_t n = fread(out, 1, sizeof *out, f);
    fclose(f);
    if (n < sizeof *out) out->room_dc = INT16_MIN;
    return n >= offsetof(test_sum_t, room_dc);
}

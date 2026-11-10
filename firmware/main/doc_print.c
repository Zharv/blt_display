#include "doc_print.h"
#include "blt_archive.h"
#include "doc.h"
#include "rtext.h"
#include "print_job.h"
#include "netw.h"
#include "bms_ble.h"
#include "test_sum.h"
#include "sn_fix.h"
#include "health.h"
#include "sd_arch.h"
#include "hist.h"
#include <limits.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psram_task.h"

static const char *TAG = "doc";

extern const uint8_t ttf_start[] asm("_binary_Montserrat_Medium_ttf_start");
extern const uint8_t ttf_end[]   asm("_binary_Montserrat_Medium_ttf_end");

/* company texts (from the protocol template) */
#define CO_NAME   "ТОВ \"ЗАВОД АКУМУЛЯТОРНИХ СИСТЕМ ЖАРВ\""
#define CO_CODE   "45745311"
#define CO_ADDR1  "Україна, 07400, Київська обл.,"
#define CO_ADDR2  "м. Бровари, вул. Петлюри Симона, будинок 11Г, кімната 43"
#define CO_SIGNER_TITLE "Директор ТОВ \"ЗАС ЖАРВ\""
#define CO_SIGNER_NAME  "Жалінський Роман"

static volatile bool s_busy;
static doc_extra_t s_x;
static bool s_print;
static void (*s_done)(esp_err_t);

/* "83.8" -> "83,8" */
static void comma(char *s)
{
    for (char *c = s; *c; c++)
        if (*c == '.' && c > s && c[-1] >= '0' && c[-1] <= '9' && c[1] >= '0' && c[1] <= '9') *c = ',';
}

#define KV(lbl, ...) do { if (ni < (int)(sizeof info / sizeof info[0])) { \
    info[ni].label = (lbl); snprintf(info[ni].value, sizeof info[ni].value, __VA_ARGS__); \
    comma(info[ni].value); ni++; } } while (0)
/* same without the decimal comma (dates, names) */
#define KVR(lbl, ...) do { if (ni < (int)(sizeof info / sizeof info[0])) {     info[ni].label = (lbl); snprintf(info[ni].value, sizeof info[ni].value, __VA_ARGS__); ni++; } } while (0)

static esp_err_t build_and_print(void)
{
    blt_test_t *t = heap_caps_calloc(1, sizeof *t, MALLOC_CAP_SPIRAM);
    if (!t) return ESP_ERR_NO_MEM;
    esp_err_t e = ESP_FAIL;
    bool from_sd = false;
    if (s_x.cycle != BLT_ARCH_CURRENT) from_sd = (e = sd_arch_load(s_x.cycle, t)) == ESP_OK;
    if (!from_sd) {
        e = blt_archive_fetch(s_x.cycle, t);
        /* the tester already holds the next (empty) test: print the last one
         * that has a result */
        if (e == ESP_OK && t->current && !blt_rec_has_result(&t->rec)) {
            uint16_t c = t->rec.pr_cycle;
            esp_err_t e2 = ESP_ERR_NOT_FOUND;
            for (int k = 1; k <= 8 && e2 != ESP_OK; k++) {
                uint16_t n = (uint16_t)(c - k);
                e2 = sd_arch_load(n, t);
                if (e2 == ESP_OK) { from_sd = true; break; }
                e2 = blt_archive_fetch(n, t);
                if (e2 == ESP_OK && !blt_rec_has_result(&t->rec)) e2 = ESP_ERR_NOT_FOUND;
            }
            if (e2 == ESP_OK) ESP_LOGI(TAG, "current cycle %u is empty - printing cycle %u", c, t->rec.pr_cycle);
            else e = ESP_ERR_NOT_FOUND;
        }
        /* a finished test read from the tester: keep it on the card */
        if (e == ESP_OK && !from_sd && blt_rec_has_result(&t->rec)
            && !(t->current && t->rec.state >= 1 && t->rec.state <= 4)
            && !sd_arch_has(t->rec.pr_test, t->rec.pr_cycle))
            sd_arch_save(t);
    }
    /* no tester on the cable: the newest finished test on the card */
    if (e != ESP_OK && !from_sd && s_x.cycle == BLT_ARCH_CURRENT) {
        const int MAXE = 160;
        blt_arch_entry_t *ls = heap_caps_malloc(MAXE * sizeof *ls, MALLOC_CAP_SPIRAM);
        int nl = ls ? sd_arch_list(ls, MAXE) : 0;
        if (nl > 0 && sd_arch_load(ls[0].n, t) == ESP_OK) {
            ESP_LOGI(TAG, "tester: %s - printing cycle %u from the SD card", esp_err_to_name(e), ls[0].n);
            e = ESP_OK;
            from_sd = true;
        }
        heap_caps_free(ls);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "no test to print: %s", esp_err_to_name(e));
        heap_caps_free(t);
        return e == ESP_ERR_NOT_FOUND ? DOC_ERR_NO_TEST : e;   /* NOT_FOUND means "no printer" to the UI */
    }
    const blt_rec_t *rc = &t->rec;

    /* what the panel saw of this test (BMS): live values for the current test,
     * the saved summary for an archive one */
    test_sum_t sum;
    bool have_sum = test_sum_get(rc->pr_test, &sum) || sd_arch_get_sum(rc->pr_cycle, &sum);
    bool live_bms = t->current && s_x.bms;
    if (!t->current) { s_x.bms = false; s_x.live_ah = 0; s_x.temp_start = -999; }
    if (have_sum) {
        if (sum.at_start && sum.temp0_dc != INT16_MIN) s_x.temp_start = sum.temp0_dc / 10.0f;
        if (sum.delta_max_mv > s_x.imbalance_max_mv) s_x.imbalance_max_mv = sum.delta_max_mv;
    }

    /* the panel's BMS log of this test (SD card): imbalance curve, lowest cell */
    static float *d_t, *d_mv;
    int dn = 0, low_cell = 0, log_d_end = -1;
    {
        const int MAXR = 4000;
        hist_pt_t *lg = heap_caps_malloc(MAXR * sizeof(hist_pt_t), MALLOC_CAP_SPIRAM);
        if (!d_t) d_t = heap_caps_malloc(MAXR * sizeof(float), MALLOC_CAP_SPIRAM);
        if (!d_mv) d_mv = heap_caps_malloc(MAXR * sizeof(float), MALLOC_CAP_SPIRAM);
        if (lg && d_t && d_mv) {
            int nl = sd_arch_read_log(rc->pr_test, rc->pr_cycle, lg, MAXR);
            for (int k = 0; k < nl; k++) {
                if (lg[k].n < 2) continue;
                uint16_t lo = 0xFFFF, hi = 0;
                int lo_i = 0;
                for (int c = 0; c < lg[k].n; c++) {
                    if (lg[k].cell[c] < lo) { lo = lg[k].cell[c]; lo_i = c; }
                    if (lg[k].cell[c] > hi) hi = lg[k].cell[c];
                }
                d_t[dn] = lg[k].t_s / 60.0f;
                d_mv[dn] = (float)(hi - lo);
                dn++;
                low_cell = lo_i + 1;
                log_d_end = hi - lo;
            }
        }
        heap_caps_free(lg);
    }

    float vmin = 1e9f, wh = 0;
    for (int k = 0; k < t->n; k++) {
        if (t->v[k] < vmin) vmin = t->v[k];
        if (k) wh += (t->v[k] + t->v[k - 1]) / 2 * (t->i[k] + t->i[k - 1]) / 2
                     * (t->t_min[k] - t->t_min[k - 1]) / 60.0f;
    }
    float ah = rc->ah_dis;
    if (s_x.live_ah > ah && s_x.live_ah < ah + 1.5f) ah = s_x.live_ah;
    bool running = t->current && rc->state == 1;      /* still discharging */

    int cells = live_bms ? s_x.cells : (have_sum ? sum.cells : 0);
    bool sum_ok = have_sum && sum.cells;
    int d_start = sum_ok ? sum.delta0_mv : -1;
    int d_end = live_bms ? s_x.imbalance_mv
              : log_d_end >= 0 ? log_d_end
              : sum_ok ? (int)sum.cell_max_mv - (int)sum.cell_min_mv : -1;
    bool t_known = s_x.temp_start > -100;
    float t_start = t_known ? s_x.temp_start : 20.0f;   /* not measured: 20 C */
    bool tmax_known = sum_ok && sum.temp_max_dc != INT16_MIN;
    float t_max = tmax_known ? sum.temp_max_dc / 10.0f : 0;

    chem_t ch = s_x.chem == CHEM_NMC || s_x.chem == CHEM_LFP ? (chem_t)s_x.chem
              : health_guess_chem(rc->rated_v, cells, sum_ok ? sum.cell_max_mv : 0);
    float v_cell = ch == CHEM_LFP ? 3.2f : 3.7f;
    /* no BMS data (older tests): the cell count from the tester's rated
     * voltage (programmed as 3.6 V/cell NMC, 3.2 V/cell LFP) */
    if (!cells && rc->rated_v)
        cells = (int)lroundf(rc->rated_v / (ch == CHEM_LFP ? 3.2f : 3.6f));

    /* the object: chemistry, cells, nominal values */
    static char bdesc[160];
    if (cells && !s_x.detailed)
        snprintf(bdesc, sizeof bdesc, "%s %dS, номінальна напруга %.0f В, номінальна ємність %u А·год",
                 ch == CHEM_LFP ? "Li-FePO4" : "Li-NMC", cells, cells * v_cell, rc->rated_ah);
    else if (cells)
        snprintf(bdesc, sizeof bdesc, "%s, %dS, номінальна напруга %.0f В, номінальна ємність %u А·год",
                 ch == CHEM_LFP ? "Li-FePO4" : "Li-NMC", cells, cells * v_cell, rc->rated_ah);
    else
        snprintf(bdesc, sizeof bdesc, "номінальна напруга %u В, номінальна ємність %u А·год", rc->rated_v, rc->rated_ah);

    /* results table: only what matters to the customer */
    static doc_kv_t info[16];
    int ni = 0;
    char dur[64];
    doc_duration_ua(rc->t_dis_min, dur, sizeof dur);
    if (rc->rated_ah) KV(running ? "Знято на момент друку" : "Встановлена ємність", "%.0f А·год (%.1f %%)", ah, ah * 100.0f / rc->rated_ah);
    else KV("Встановлена ємність", "%.0f А·год", ah);
    KV("Енергія", "%.1f кВт·год", wh / 1000.0f);
    KVR("Тривалість розряду", "%s", dur);
    if (ah > 0 && wh > 0) KV("Середня напруга розряду", "%.1f В", wh / ah); else KVR("Середня напруга розряду", "—");
    if (t->n && t->v[0] > 0) {
        if (cells) KV("Напруга до тестування", "%.1f В (%.2f В/ком.)", t->v[0], t->v[0] / cells);
        else KV("Напруга до тестування", "%.1f В", t->v[0]);
    } else KVR("Напруга до тестування", "—");
    if (t->n) KV(running ? "Напруга зараз" : "Напруга в кінці", "%.1f В", t->v[t->n - 1]); else KVR("Напруга в кінці", "—");
    if (d_start >= 0 || d_end >= 0) {
        char a1[12] = "—", a2[12] = "—";
        if (d_start >= 0) snprintf(a1, sizeof a1, "%d", d_start);
        if (d_end >= 0) snprintf(a2, sizeof a2, "%d", d_end);
        KVR("Розбаланс: початок / кінець", "%s / %s мВ", a1, a2);
    } else KVR("Розбаланс: початок / кінець", "немає даних BMS");
    if (tmax_known) KV("Температура батареї", "%.0f → %.0f °C", t_start, t_max);
    else KV("Температура батареї", "%.0f °C", t_start);

    /* conclusion */
    doc_stop_t stop;
    static char alarm_nm[48];
    {
        const uint8_t *m = rc->mem_alarms;
        blt_rec_t ra = *rc;
        ra.mem_alarms[3] &= (uint8_t)~0x08;          /* 28 VSTOP IN SCARICA */
        ra.mem_alarms[5] &= (uint8_t)~0x01;          /* 41 STOP TEST DA BATT */
        int other = blt_rec_alarm_first(&ra);
        if (running) stop = DOC_STOP_RUNNING;
        else if (m[5] & 0x01) stop = DOC_STOP_BMS;
        else if (other) { stop = DOC_STOP_ALARM; strlcpy(alarm_nm, blt_alarm_name_ua(other), sizeof alarm_nm); }
        else if (m[3] & 0x08) stop = DOC_STOP_VSTOP;
        else if (rc->t_limit_min && rc->t_limit_min != 6000 && rc->t_dis_min >= rc->t_limit_min) stop = DOC_STOP_TIME;
        else if (t->n && t->v[t->n - 1] <= blt_rec_stop_v(rc) + 0.5f) stop = DOC_STOP_VSTOP;
        else stop = DOC_STOP_MANUAL;
    }
    doc_facts_t facts = {
        .ah = ah, .rated_ah = rc->rated_ah, .cells = cells, .lfp = ch == CHEM_LFP,
        .v_before = t->n ? t->v[0] : 0, .v_before_loaded = t->n && t->i[0] > 2.0f,
        .temp_known = t_known, .temp_start = t_start, .temp_max_known = tmax_known, .temp_max = t_max,
        .d_start = d_start, .d_end = d_end, .low_cell = low_cell,
        .stop = stop, .v_end = t->n ? t->v[t->n - 1] : 0, .v_stop = blt_rec_stop_v(rc),
        .alarm_name = alarm_nm,
    };
    /* one line for the simple protocol */
    static char why[96];
    switch (stop) {
    case DOC_STOP_VSTOP:  snprintf(why, sizeof why, "досягнуто напругу кінця розряду"); break;
    case DOC_STOP_BMS:    snprintf(why, sizeof why, "спрацював захист BMS батареї"); break;
    case DOC_STOP_ALARM:  snprintf(why, sizeof why, "аварія тестера - %s", alarm_nm[0] ? alarm_nm : "?"); break;
    case DOC_STOP_TIME:   snprintf(why, sizeof why, "обмеження часу розряду"); break;
    case DOC_STOP_MANUAL: snprintf(why, sizeof why, "зупинка тестування"); break;
    default:              why[0] = 0; break;
    }

    static char verdict[320];
    static doc_finding_t finds[DOC_FINDINGS_MAX];
    int nf = doc_analyze(&facts, verdict, sizeof verdict, finds, DOC_FINDINGS_MAX);

    static doc_protocol_t d;
    d = (doc_protocol_t){
        .company = CO_NAME, .company_code = CO_CODE, .address = { CO_ADDR1, CO_ADDR2 },
        .signer_title = CO_SIGNER_TITLE, .signer_name = CO_SIGNER_NAME,
        .equipment = "Електронне навантаження BLT160",
        .v_before = t->n ? t->v[0] : 0, .temp_start = s_x.temp_start,
        .discharge_a = rc->discharge_a, .capacity_ah = ah, .discharge_min = rc->t_dis_min,
        .in_progress = running,
        .battery_desc = bdesc,
        .detailed = s_x.detailed, .stop_reason = why[0] ? why : NULL,
        .verdict = verdict, .findings = finds, .findings_n = nf,
        .info = info, .info_n = ni,
        .t_min = t->t_min, .v = t->v, .i = t->i, .n = t->n,
        .start_h = t->t0_h, .start_m = t->t0_mi, .v_stop = 0,   /* no stop-voltage line */
        .dt_min = d_t, .dmv = d_mv, .dn = s_x.detailed ? dn : 0,
    };
    /* protocol number: from a serial YYYYMMDDNNNN -> DDMMYYYY-N, else the test date */
    static char snfix[20];
    const char *sn = sn_fix_apply(rc->pr_cycle, rc->battery_id, snfix, sizeof snfix);
    bool digits = strlen(sn) == 12;
    for (int k = 0; digits && k < 12; k++) digits = sn[k] >= '0' && sn[k] <= '9';
    if (digits) snprintf(d.number, sizeof d.number, "%.2s%.2s%.4s-%d", sn + 6, sn + 4, sn, atoi(sn + 8));
    else snprintf(d.number, sizeof d.number, "%02u%02u20%02u-%u", rc->dis_d, rc->dis_mo, rc->dis_y, rc->pr_test);
    snprintf(d.date, sizeof d.date, "%02u.%02u.20%02u", rc->snap_d, rc->snap_mo, rc->snap_y);
    strlcpy(d.serial, sn[0] ? sn : "—", sizeof d.serial);
    if (rc->dis_d)
        snprintf(d.start_str, sizeof d.start_str, "%02u.%02u.20%02u %02u:%02u", rc->dis_d, rc->dis_mo, rc->dis_y, rc->dis_h, rc->dis_mi);

    /* page data for the host preview (tests/preview) */
    ESP_LOGI(TAG, "DOCK number|%s", d.number);
    ESP_LOGI(TAG, "DOCK date|%s", d.date);
    ESP_LOGI(TAG, "DOCK serial|%s", d.serial);
    ESP_LOGI(TAG, "DOCK v_before|%.1f", d.v_before);
    ESP_LOGI(TAG, "DOCK temp_start|%.1f", d.temp_start);
    ESP_LOGI(TAG, "DOCK discharge_a|%d", d.discharge_a);
    ESP_LOGI(TAG, "DOCK capacity_ah|%.2f", d.capacity_ah);
    ESP_LOGI(TAG, "DOCK discharge_min|%u", (unsigned)d.discharge_min);
    ESP_LOGI(TAG, "DOCK in_progress|%d", d.in_progress);
    ESP_LOGI(TAG, "DOCK start|%u:%u", d.start_h, d.start_m);
    ESP_LOGI(TAG, "DOCK v_stop|%.2f", d.v_stop);
    ESP_LOGI(TAG, "DOCK battery_desc|%s", bdesc);
    ESP_LOGI(TAG, "DOCK detailed|%d", d.detailed);
    if (d.stop_reason) ESP_LOGI(TAG, "DOCK stop_reason|%s", d.stop_reason);
    ESP_LOGI(TAG, "DOCK start_str|%s", d.start_str);
    ESP_LOGI(TAG, "DOCK verdict|%s", verdict);
    for (int k = 0; k < nf; k++) ESP_LOGI(TAG, "DOCF %d|%s", finds[k].level, finds[k].text);
    for (int k = 0; k < ni; k++) ESP_LOGI(TAG, "DOCI %s|%s", info[k].label, info[k].value);
    for (int k = 0; !s_print && k < t->n; k++) {
        ESP_LOGI(TAG, "DOCS %u %.1f %.1f", t->t_min[k], t->v[k], t->i[k]);
        if (k % 40 == 39) vTaskDelay(1);
    }
    for (int k = 0; !s_print && k < dn; k++) {
        ESP_LOGI(TAG, "DOCD %.2f %.0f", d_t[k], d_mv[k]);
        if (k % 40 == 39) vTaskDelay(1);
    }
    ESP_LOGI(TAG, "DOCEND");

    if (s_print) {
        uint8_t *px = heap_caps_malloc((size_t)DOC_A4_W * DOC_A4_H, MALLOC_CAP_SPIRAM);
        if (!px) { heap_caps_free(t); return ESP_ERR_NO_MEM; }
        raster_t r;
        raster_init(&r, px, DOC_A4_W, DOC_A4_H);
        raster_clear(&r, 255);
        uint32_t t0 = esp_log_timestamp();
        doc_protocol_render(&d, &r);
        ESP_LOGI(TAG, "rendered in %lu ms", (unsigned long)(esp_log_timestamp() - t0));
        char job[48];
        snprintf(job, sizeof job, "Protocol %s", d.number);
        bms_ble_hold(true);
        e = print_job_send_raster(&r, 300, job);
        bms_ble_hold(false);
        heap_caps_free(px);
    }
    heap_caps_free(t);
    return e;
}

/* called from the renderer / encoder: sleep a tick every ~30 ms of work */
static void doc_yield(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (now - last > 30000) {
        vTaskDelay(1);
        last = esp_timer_get_time();
    }
}

static bool s_test_page;
static TaskHandle_t s_worker;

static esp_err_t test_page(void);

/* one long-lived worker: page render + URF encoding take seconds of CPU, so
 * it runs on core 1 below the UI (LVGL is pinned to core 0, priority 4), with
 * its stack in PSRAM (internal RAM is kept for the Wi-Fi/BLE transport) */
static void doc_task(void *arg)
{
    (void)arg;
    static bool font_ok;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!font_ok) font_ok = rtext_init(ttf_start, (size_t)(ttf_end - ttf_start));
        raster_yield = doc_yield;
        esp_err_t e;
        bool net = s_test_page || s_print;
        if (net) netw_acquire(0);           /* join the printer's Wi-Fi while the page is built */
        if (s_test_page) {
            e = test_page();
            ESP_LOGI(TAG, "test page: %s", esp_err_to_name(e));
        } else {
            e = font_ok ? build_and_print() : ESP_FAIL;
            ESP_LOGI(TAG, "protocol %s: %s", s_print ? "print" : "dump", esp_err_to_name(e));
            if (s_done) s_done(e);
        }
        if (net) netw_release();
        raster_yield = NULL;
        s_busy = false;
    }
}

static bool kick_worker(void)
{
    if (!s_worker && psram_task(doc_task, "doc", 12288, NULL, 2, &s_worker, 1) != pdPASS) return false;
    xTaskNotifyGive(s_worker);
    return true;
}

bool doc_print_protocol(const doc_extra_t *x, bool print, void (*done)(esp_err_t))
{
    if (s_busy) return false;
    s_busy = true;
    s_x = *x;
    s_print = print;
    s_done = done;
    s_test_page = false;
    if (!kick_worker()) { s_busy = false; return false; }
    return true;
}

/* dev console PRINTTEST: a page with text and a gray gradient (~400 KB of
 * URF), to test the printer path without tester data */
static esp_err_t test_page(void)
{
    uint8_t *px = heap_caps_malloc((size_t)DOC_A4_W * DOC_A4_H, MALLOC_CAP_SPIRAM);
    if (!px) return ESP_ERR_NO_MEM;
    raster_t r;
    raster_init(&r, px, DOC_A4_W, DOC_A4_H);
    raster_clear(&r, 255);
    rtext_draw(&r, 250, 200, "Тестова сторінка BLT-display", 70, 0, true);
    for (int y = 500; y < 1400; y++) {
        for (int x = 250; x < 2230; x++) px[(size_t)y * DOC_A4_W + x] = (uint8_t)(((x - 250) * 255 / 1980 + (y & 3)) & 0xFF);
        RASTER_YIELD();
    }
    bms_ble_hold(true);
    esp_err_t e = print_job_send_raster(&r, 300, "BLT test page");
    bms_ble_hold(false);
    heap_caps_free(px);
    return e;
}

void doc_print_test_page(void)
{
    if (s_busy) return;
    s_busy = true;
    s_test_page = true;
    if (!kick_worker()) s_busy = false;
}

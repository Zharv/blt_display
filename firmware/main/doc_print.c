#include "doc_print.h"
#include "blt_archive.h"
#include "doc.h"
#include "rtext.h"
#include "print_job.h"
#include "bms_ble.h"
#include "test_sum.h"
#include "sd_arch.h"
#include <limits.h>

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
static void comma(char *s) { for (; *s; s++) if (*s == '.') *s = ','; }

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
    if (e != ESP_OK) { heap_caps_free(t); return e; }
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

    static doc_kv_t info[16];
    int ni = 0;
    float vmin = 1e9f, wh = 0;
    for (int k = 0; k < t->n; k++) {
        if (t->v[k] < vmin) vmin = t->v[k];
        if (k) wh += (t->v[k] + t->v[k - 1]) / 2 * (t->i[k] + t->i[k - 1]) / 2
                     * (t->t_min[k] - t->t_min[k - 1]) / 60.0f;
    }
    float ah = rc->ah_dis;
    if (s_x.live_ah > ah && s_x.live_ah < ah + 1.5f) ah = s_x.live_ah;
    bool running = t->current && rc->state == 1;      /* still discharging */

    KVR("Тестер", "BLT160 · тест %u", rc->pr_test);
    KVR("Серійний № батареї", "%s", rc->battery_id[0] ? rc->battery_id : "—");
    KV("Номінальна напруга", "%u В", rc->rated_v);
    KV("Номінальна ємність", "%u А·год", rc->rated_ah);
    KV("Струм розряду", "%u А", rc->discharge_a);
    KV("Напруга кінця розряду", "%.1f В", blt_rec_stop_v(rc));
    if (rc->dis_d)
        KVR("Початок розряду", "%02u.%02u.20%02u %02u:%02u", rc->dis_d, rc->dis_mo, rc->dis_y, rc->dis_h, rc->dis_mi);
    else
        KV("Початок розряду", "—");
    KV("Час розряду", "%u:%02u", rc->t_dis_min / 60, rc->t_dis_min % 60);
    if (rc->rated_ah) KV("Знята ємність", "%.0f А·год (%.1f %%)", ah, ah * 100.0f / rc->rated_ah);
    else KV("Знята ємність", "%.0f А·год", ah);
    KV("Енергія", "%.0f Вт·год", wh);
    if (t->n) KV("Мін. напруга", "%.1f В", vmin); else KV("Мін. напруга", "—");
    KV("Макс. темп. тестера", "%d °C", rc->t_card_max);
    /* id 28 "VSTOP IN SCARICA" is the normal end of a discharge, not a fault */
    blt_rec_t ra = *rc;
    ra.mem_alarms[3] &= (uint8_t)~(1u << 3);
    int na = blt_rec_alarm_count(&ra);
    int a1 = blt_rec_alarm_first(&ra);
    if (na == 1) KVR("Аварії тестера", "%s", blt_alarm_name_ua(a1)[0] ? blt_alarm_name_ua(a1) : "1");
    else if (na) KVR("Аварії тестера", "%d (%s, …)", na, blt_alarm_name_ua(a1));
    else KVR("Аварії тестера", "немає");
    if (live_bms) {
        KVR("BMS", "%.20s · %uS", s_x.bms_name, s_x.cells);
        KV("Комірки (зараз)", "%.3f…%.3f В", s_x.cell_min_mv / 1000.0f, s_x.cell_max_mv / 1000.0f);
        if (s_x.imbalance_max_mv) KVR("Розбаланс", "%u мВ (макс. %u)", s_x.imbalance_mv, s_x.imbalance_max_mv);
        else KV("Розбаланс", "%u мВ", s_x.imbalance_mv);
    } else if (have_sum && sum.cells) {
        KVR("BMS", "%.19s · %uS", sum.bms, sum.cells);
        KV("Комірки (кінець)", "%.3f…%.3f В", sum.cell_min_mv / 1000.0f, sum.cell_max_mv / 1000.0f);
        KVR("Розбаланс", "старт %u, макс. %u мВ", sum.delta0_mv, sum.delta_max_mv);
    }

    static doc_protocol_t d;
    d = (doc_protocol_t){
        .company = CO_NAME, .company_code = CO_CODE, .address = { CO_ADDR1, CO_ADDR2 },
        .signer_title = CO_SIGNER_TITLE, .signer_name = CO_SIGNER_NAME,
        .equipment = "Електронне навантаження BLT160",
        .v_before = t->n ? t->v[0] : 0, .temp_start = s_x.temp_start,
        .discharge_a = rc->discharge_a, .capacity_ah = ah, .discharge_min = rc->t_dis_min,
        .in_progress = running,
        .info = info, .info_n = ni,
        .t_min = t->t_min, .v = t->v, .i = t->i, .n = t->n,
        .start_h = t->t0_h, .start_m = t->t0_mi, .v_stop = blt_rec_stop_v(rc),
    };
    /* protocol number: from a serial YYYYMMDDNNNN -> DDMMYYYY-N, else the test date */
    const char *sn = rc->battery_id;
    bool digits = strlen(sn) == 12;
    for (int k = 0; digits && k < 12; k++) digits = sn[k] >= '0' && sn[k] <= '9';
    if (digits) snprintf(d.number, sizeof d.number, "%.2s%.2s%.4s-%d", sn + 6, sn + 4, sn, atoi(sn + 8));
    else snprintf(d.number, sizeof d.number, "%02u%02u20%02u-%u", rc->dis_d, rc->dis_mo, rc->dis_y, rc->pr_test);
    snprintf(d.date, sizeof d.date, "%02u.%02u.20%02u", rc->snap_d, rc->snap_mo, rc->snap_y);
    strlcpy(d.serial, sn[0] ? sn : "—", sizeof d.serial);

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
    for (int k = 0; k < ni; k++) ESP_LOGI(TAG, "DOCI %s|%s", info[k].label, info[k].value);
    for (int k = 0; !s_print && k < t->n; k++) {
        ESP_LOGI(TAG, "DOCS %u %.1f %.1f", t->t_min[k], t->v[k], t->i[k]);
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
        if (s_test_page) {
            e = test_page();
            ESP_LOGI(TAG, "test page: %s", esp_err_to_name(e));
        } else {
            e = font_ok ? build_and_print() : ESP_FAIL;
            ESP_LOGI(TAG, "protocol %s: %s", s_print ? "print" : "dump", esp_err_to_name(e));
            if (s_done) s_done(e);
        }
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

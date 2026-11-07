#include "doc_print.h"
#include "blt_archive.h"
#include "doc.h"
#include "rtext.h"
#include "print_job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
    esp_err_t e = blt_archive_fetch_current(t);
    if (e != ESP_OK) { heap_caps_free(t); return e; }
    const blt_rec_t *rc = &t->rec;

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
    bool running = rc->state == 1 || rc->state == 2;

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
    if (s_x.bms) {
        KVR("BMS", "%.20s · %uS", s_x.bms_name, s_x.cells);
        KV("Комірки (зараз)", "%.3f…%.3f В", s_x.cell_min_mv / 1000.0f, s_x.cell_max_mv / 1000.0f);
        if (s_x.imbalance_max_mv) KVR("Розбаланс", "%u мВ (макс. %u)", s_x.imbalance_mv, s_x.imbalance_max_mv);
        else KV("Розбаланс", "%u мВ", s_x.imbalance_mv);
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
        e = print_job_send_raster(&r, 300, job);
        heap_caps_free(px);
    }
    heap_caps_free(t);
    return e;
}

static void doc_task(void *arg)
{
    (void)arg;
    static bool font_ok;
    if (!font_ok) font_ok = rtext_init(ttf_start, (size_t)(ttf_end - ttf_start));
    esp_err_t e = font_ok ? build_and_print() : ESP_FAIL;
    ESP_LOGI(TAG, "protocol %s: %s", s_print ? "print" : "dump", esp_err_to_name(e));
    if (s_done) s_done(e);
    s_busy = false;
    vTaskDelete(NULL);
}

bool doc_print_protocol(const doc_extra_t *x, bool print, void (*done)(esp_err_t))
{
    if (s_busy) return false;
    s_busy = true;
    s_x = *x;
    s_print = print;
    s_done = done;
    if (xTaskCreate(doc_task, "doc", 12288, NULL, 4, NULL) != pdPASS) { s_busy = false; return false; }
    return true;
}

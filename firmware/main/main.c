/*
 * blt-esp-display firmware entry.
 * Brings up the board (BSP + LVGL), builds the tabbed UI (OnLine, Archive, BMS,
 * Printer, Settings), drives the tester side with a synthetic discharge (until
 * the USB link to a real BLT tester is wired), takes the BMS side from the BLE
 * link when it is live, and prints the report over IPP from the tab bar.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include <limits.h>
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_lcd_touch.h"
#include "bsp/touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui.h"
#include "ui_frame.h"
#include "ui_common.h"
#include "ui_wifi.h"
#include "ui_printer.h"
#include "ui_bms.h"
#include "ui_test.h"
#include "fonts.h"
#include "i18n.h"
#include "hist.h"
#include "snap.h"
#include "bms_ble.h"
#include "blt_usb.h"
#include "netw.h"
#include "print_job.h"
#include "doc_print.h"
#include "test_sum.h"
#include "sd_store.h"
#include "sd_arch.h"
#include "psram_task.h"
#include "blt_archive.h"
#include "esp_heap_caps.h"

static const char *TAG = "blt";

#define TICK_MS         500

static volatile bool s_printing;
static float s_temp_start = -999;    /* BMS battery temperature when the test started */

static float s_live_wh;              /* energy of the running test, integrated in tick_cb */
/* The RI block has whole Ah and whole minutes. Between two tester updates the
 * fraction is filled in locally (current x time, seconds since the minute
 * changed) and re-synced to the tester whenever its value moves. */
static float s_ah_extra;
static uint32_t s_sec_extra;

/* Live tester data (RI block), filled by blt_poll_task once reads are allowed */
static blt_live_t s_live;
static volatile int64_t s_live_us;
static portMUX_TYPE s_live_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool s_live_poll;   /* continuous polling (off until the user enables it) */

void blt_live_poll_enable(bool on)
{
    s_live_poll = on;
    blt_usb_allow_reads(on);
    ESP_LOGW(TAG, "tester live polling %s", on ? "ON" : "off");
    nvs_handle_t h;
    if (nvs_open("blt", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "poll", on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool blt_live_poll_enabled(void) { return s_live_poll; }

/* boot: restore the user's choice (read-only polling of the tester) */
static void blt_live_poll_restore(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("blt", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "poll", &v);
        nvs_close(h);
    }
    s_live_poll = v != 0;
    blt_usb_allow_reads(s_live_poll);
}

static void blt_poll_task(void *arg)
{
    (void)arg;
    int fails = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(900));          /* BltView polls every 900 ms */
        if (!s_live_poll || !blt_usb_reads_allowed() || blt_usb_state(NULL, NULL) != BLT_USB_CONNECTED) continue;
        blt_live_t l;
        esp_err_t e = blt_usb_read_live(&l);
        if (e != ESP_OK) {
            if (++fails % 10 == 1) ESP_LOGW(TAG, "tester live read: %s", esp_err_to_name(e));
            continue;
        }
        if (fails) ESP_LOGI(TAG, "tester live read OK after %d failures", fails);
        fails = 0;
        /* joined a running test: fill the chart with what the tester recorded
         * so far (read-only RI/RD), once per test, a few reads after it
         * appeared so tick_cb has settled its history first */
        static int live_reads;
        static uint16_t seeded_test = 0xFFFF;
        live_reads++;
        if ((l.state == BLT_ST_DISCHARGE || l.state == BLT_ST_DISCHARGE_PAUSE) &&
            l.test_no != seeded_test && live_reads > 4) {
            seeded_test = l.test_no;
            blt_test_t *bt = heap_caps_malloc(sizeof *bt, MALLOC_CAP_SPIRAM);
            const int LOGMAX = 4000;
            hist_pt_t *lg = heap_caps_malloc((LOGMAX + BLT_ARCH_MAX) * sizeof(hist_pt_t), MALLOC_CAP_SPIRAM);
            if (bt && lg && blt_archive_fetch_current(bt) == ESP_OK) {
                /* the panel's own log on the SD card (has the cells) */
                int nl = sd_arch_read_log(l.test_no, bt->rec.pr_cycle, lg, LOGMAX);
                /* + recorder minutes not covered by the log (merge, time order) */
                hist_pt_t *m = lg + LOGMAX;
                int nm = 0, j = 0;
                hist_pt_t *all = heap_caps_malloc((LOGMAX + BLT_ARCH_MAX) * sizeof(hist_pt_t), MALLOC_CAP_SPIRAM);
                if (all) {
                    for (int k = 0; k < bt->n; k++) {
                        uint32_t ts = (uint32_t)bt->t_min[k] * 60u;
                        bool covered = false;
                        while (j < nl && lg[j].t_s + 60 < ts) j++;
                        if (j < nl && lg[j].t_s <= ts + 60) covered = true;
                        if (covered) continue;
                        memset(&m[nm], 0, sizeof m[nm]);
                        m[nm].t_s = ts; m[nm].v_t = bt->v[k]; m[nm].i_t = -bt->i[k];
                        nm++;
                    }
                    int a = 0, b = 0, na = 0;
                    while (a < nl || b < nm)
                        all[na++] = (b >= nm || (a < nl && lg[a].t_s <= m[b].t_s)) ? lg[a++] : m[b++];
                    bsp_display_lock(0);
                    int k = hist_prepend_points(all, na);
                    bsp_display_unlock();
                    ESP_LOGI(TAG, "chart: %d points (%d from the SD log, %d recorder minutes) for test %u",
                             k, nl, nm, l.test_no);
                    heap_caps_free(all);
                }
            }
            heap_caps_free(lg);
            heap_caps_free(bt);
            sd_arch_request_sync();            /* copy finished tests to the SD card */
        }
        taskENTER_CRITICAL(&s_live_mux);
        s_live = l;
        s_live_us = esp_timer_get_time();
        taskEXIT_CRITICAL(&s_live_mux);
        static int n;
        if (n++ % 20 == 0)
            ESP_LOGI(TAG, "tester: %.1f V  dis %.1f A  chg %.1f A  %u Ah  t %u min  state %u (%s)",
                     l.v_batt, l.i_discharge, l.i_charge, l.ah_discharged, l.t_discharge_min,
                     l.state, blt_state_name_it(l.state));
    }
}

/* latest live block for other modules (blt_prepare, ui_test) */
bool blt_live_latest(blt_live_t *out, uint32_t *age_ms)
{
    if (!s_live_us) return false;
    taskENTER_CRITICAL(&s_live_mux);
    *out = s_live;
    int64_t at = s_live_us;
    taskEXIT_CRITICAL(&s_live_mux);
    if (age_ms) *age_ms = (uint32_t)((esp_timer_get_time() - at) / 1000);
    return true;
}

/* Real data only: the tester from the USB live block, the BMS from BLE, each
 * when it delivered data in the last 5 s; otherwise the field stays "no data". */
static void fill_sample(online_sample_t *o)
{
    o->tester_usb = blt_usb_state(NULL, NULL) == BLT_USB_CONNECTED;
    o->state = TS_IDLE;
    if (s_live_us && esp_timer_get_time() - s_live_us < 5000000) {
        blt_live_t l;
        taskENTER_CRITICAL(&s_live_mux);
        l = s_live;
        taskEXIT_CRITICAL(&s_live_mux);
        o->tester_live = true;
        strlcpy(o->battery_id, l.battery_id, sizeof o->battery_id);
        o->voltage_v = l.v_batt;
        o->current_a = l.i_charge > 0.05f ? l.i_charge : -l.i_discharge;
        o->capacity_ah = l.ah_discharged + s_ah_extra;
        o->energy_wh = s_live_wh;              /* not in the RI block: integrated in tick_cb */
        bool chg = l.state == BLT_ST_CHARGE || l.state == BLT_ST_CHARGE_PAUSE;
        o->elapsed_s = (uint32_t)(chg ? l.t_charge_min : l.t_discharge_min) * 60u + s_sec_extra;
        o->temp_c = l.temp_c[0];
        o->test_no = l.test_no;
        o->rated_v = l.rated_v;
        o->i_set_a = l.i_set;
        /* id 28 "VSTOP IN SCARICA" (voltage threshold reached) is information,
         * not a fault: it is set during a normal discharge */
        blt_live_t la = l;
        la.alarms[3] &= (uint8_t)~(1u << 3);
        int first = 0;
        o->alarms = (uint8_t)blt_live_alarm_count(&la, &first);
        o->alarm_id = (uint8_t)first;
        o->state = l.state == BLT_ST_DISCHARGE || l.state == BLT_ST_CHARGE ? TS_DISCHARGE :
                   l.state == BLT_ST_DISCHARGE_PAUSE || l.state == BLT_ST_CHARGE_PAUSE ? TS_REST :
                   l.state == BLT_ST_END ? TS_DONE : TS_IDLE;
    }
    bms_data_t b;
    uint32_t age = 0;
    if (!bms_ble_get(&b, &age) || age > 5000) return;

    o->bms_present = true;
    o->bms_live = true;
    bms_ble_state(o->bms_name, sizeof o->bms_name);
    o->soc_pct = b.soc_pct;
    o->bms_pack_v = b.pack_mv / 1000.0f;
    o->bms_current_a = b.pack_ma / 1000.0f;
    o->cell_count = b.cell_count > UI_MAX_CELLS ? UI_MAX_CELLS : b.cell_count;
    for (int i = 0; i < o->cell_count; i++) o->cell_mv[i] = b.cell_mv[i];
    o->balance_bits = b.balance_bits;
    o->temp_count = b.temp_count > 4 ? 4 : b.temp_count;
    for (int i = 0; i < o->temp_count; i++) o->temp_dc[i] = b.temp_dc[i];
    o->bms_kind = bms_ble_kind();
    ui_sample_finish_cells(o);
}

/* -------------------------------------------------------------- status -- */

static void fmt_hms(char *buf, size_t n, uint32_t s)
{
    snprintf(buf, n, "%02lu:%02lu:%02lu", (unsigned long)(s / 3600),
             (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
}

static bool clock_valid(struct tm *tm)
{
    time_t now = time(NULL);
    localtime_r(&now, tm);
    return tm->tm_year + 1900 >= 2025;
}

static void frame_refresh(const online_sample_t *o)
{
    static char state[64], wifi[40], bt[40], clk[8];
    static ind_t prn = IND_OFF;
    static int slow;

    char hms[16];
    fmt_hms(hms, sizeof hms, o->elapsed_s);
    static const str_id_t ST[] = { S_ST_IDLE, S_ST_DISCHARGE, S_ST_REST, S_ST_DONE };
    if (o->tester_live) snprintf(state, sizeof state, "%s  ·  %s", tr(ST[o->state & 3]), hms);
    else snprintf(state, sizeof state, "%s", tr(S_NO_TESTER));

    frame_status_t st = {
        .state_text = state,
        .state_ind = !o->tester_live ? IND_OFF :
                     o->state == TS_DISCHARGE ? IND_OK : o->state == TS_IDLE ? IND_OFF : IND_BUSY,
    };

    char ssid[33];
    netw_get_ssid(ssid, sizeof ssid);
    bool wifi_up = netw_is_connected();
    st.wifi = wifi_up ? IND_OK : ssid[0] ? IND_BUSY : IND_OFF;
    snprintf(wifi, sizeof wifi, "%.24s", ssid[0] ? ssid : "—");
    st.wifi_text = wifi;

    char name[32];
    bms_ble_state_t bs = bms_ble_state(name, sizeof name);
    st.bt = bs == BMS_BLE_STREAMING ? IND_OK :
            (bs == BMS_BLE_SCANNING || bs == BMS_BLE_CONNECTING || bs == BMS_BLE_CONNECTED) ? IND_BUSY : IND_OFF;
    snprintf(bt, sizeof bt, "%.24s", name[0] ? name : "—");
    st.bt_text = bt;

    if (--slow <= 0) {             /* NVS read: every ~2 s */
        slow = 4;
        netw_printer_t p;
        bool have = netw_get_printer(&p) || strncmp(ssid, "DIRECT-", 7) == 0;
        prn = !have ? IND_OFF : wifi_up ? IND_OK : IND_BUSY;
    }
    st.printer = prn;

    struct tm tm;
    if (clock_valid(&tm)) strftime(clk, sizeof clk, "%H:%M", &tm);
    else clk[0] = '\0';
    st.clock = clk;

    ui_frame_update(&st);
}

static lv_obj_t *s_scr[TAB__COUNT];

/* ------------------------------------------------------- battery status -- */

/* Values at the start of the test, kept per test number in NVS so a restart
 * of the panel does not lose them. Taken from the first BMS sample after one
 * minute of discharge (the load step itself makes the cells jump). */
typedef struct {
    uint16_t test;
    uint16_t delta0_mv;
    int16_t  temp0_dc;      /* 0.1 degC, INT16_MIN = unknown */
    uint8_t  valid;
    uint8_t  at_start;      /* taken in the first 10 min (not after joining late) */
} health_base_t;

static health_base_t s_hb;
static bool s_hb_loaded;
static uint8_t s_chem;      /* chem_t, user setting */

static void bms_temps(const online_sample_t *o, bool *has_t, float *t, bool *has_mos, float *mos)
{
    *has_t = *has_mos = false;
    int nbat = o->temp_count;
    if (o->bms_kind == BMS_KIND_JK && nbat >= 3) {      /* JK: T1, T2, MOS */
        *has_mos = true;
        *mos = o->temp_dc[2] / 10.0f;
        nbat = 2;
    }
    for (int i = 0; i < nbat; i++) {
        float v = o->temp_dc[i] / 10.0f;
        if (!*has_t || v > *t) *t = v;
        *has_t = true;
    }
}

static void health_base_save(void)
{
    nvs_handle_t h;
    if (nvs_open("health", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "base", &s_hb, sizeof s_hb);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void health_load(void)
{
    if (s_hb_loaded) return;
    s_hb_loaded = true;
    nvs_handle_t h;
    if (nvs_open("health", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof s_hb;
        if (nvs_get_blob(h, "base", &s_hb, &n) != ESP_OK || n != sizeof s_hb) memset(&s_hb, 0, sizeof s_hb);
        nvs_get_u8(h, "chem", &s_chem);
        nvs_close(h);
    }
}

uint8_t health_chem_get(void) { health_load(); return s_chem; }

/* protocol style: simple (default) or detailed - NVS "doc"/"det" */
static int8_t s_doc_detailed = -1;

bool app_protocol_detailed(void)
{
    if (s_doc_detailed < 0) {
        uint8_t v = 0;
        nvs_handle_t h;
        if (nvs_open("doc", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "det", &v); nvs_close(h); }
        s_doc_detailed = v ? 1 : 0;
    }
    return s_doc_detailed == 1;
}

void app_protocol_set_detailed(bool on)
{
    s_doc_detailed = on ? 1 : 0;
    nvs_handle_t h;
    if (nvs_open("doc", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "det", on); nvs_commit(h); nvs_close(h); }
}

void health_chem_set(uint8_t chem)
{
    s_chem = chem > CHEM_LFP ? CHEM_AUTO : chem;
    nvs_handle_t h;
    if (nvs_open("health", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "chem", s_chem);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void health_tick(const online_sample_t *o)
{
    health_load();
    bool has_t, has_mos;
    float tbat = 0, tmos = 0;
    bms_temps(o, &has_t, &tbat, &has_mos, &tmos);

    bool discharging = o->tester_live && o->state == TS_DISCHARGE;
    if (o->tester_live && (o->state == TS_DISCHARGE || o->state == TS_REST)) {
        if (s_hb.test != o->test_no || (!s_hb.valid && s_hb.test == 0)) {
            memset(&s_hb, 0, sizeof s_hb);
            s_hb.test = o->test_no;
        }
        if (!s_hb.valid && o->bms_live && o->cell_count >= 2 && o->elapsed_s >= 60) {
            s_hb.valid = 1;
            s_hb.delta0_mv = o->imbalance_mv;
            s_hb.temp0_dc = has_t ? (int16_t)(tbat * 10) : INT16_MIN;
            s_hb.at_start = o->elapsed_s < 600;
            health_base_save();
            ESP_LOGI(TAG, "health: test %u baseline d=%u mV t=%.1f C%s", s_hb.test, s_hb.delta0_mv,
                     has_t ? tbat : 0.0f, s_hb.at_start ? "" : " (joined late)");
        }
    }
    /* what the panel sees of this test, for later (archive) protocols */
    static test_sum_t sum;
    static int64_t sum_saved_us;
    if (o->tester_live && (o->state == TS_DISCHARGE || o->state == TS_REST) && o->bms_live && o->cell_count >= 2) {
        if (sum.test != o->test_no || sum.cells == 0) {
            if (!test_sum_get(o->test_no, &sum)) {
                memset(&sum, 0, sizeof sum);
                sum.test = o->test_no;
                sum.temp0_dc = sum.temp_max_dc = INT16_MIN;
            }
        }
        sum.cells = o->cell_count;
        strlcpy(sum.bms, o->bms_name, sizeof sum.bms);
        sum.cell_min_mv = o->cell_min_mv;
        sum.cell_max_mv = o->cell_max_mv;
        if (o->imbalance_mv > sum.delta_max_mv && o->elapsed_s >= 60) sum.delta_max_mv = o->imbalance_mv;
        if (has_t && (sum.temp_max_dc == INT16_MIN || tbat * 10 > sum.temp_max_dc)) sum.temp_max_dc = (int16_t)(tbat * 10);
        if (s_hb.valid && s_hb.test == o->test_no) {
            sum.delta0_mv = s_hb.delta0_mv;
            sum.temp0_dc = s_hb.temp0_dc;
            sum.at_start = s_hb.at_start;
        }
        int64_t now = esp_timer_get_time();
        if (now - sum_saved_us > 60 * 1000000LL) {     /* NVS: once a minute */
            sum_saved_us = now;
            test_sum_put(&sum);
        }
    } else if (sum.cells && sum_saved_us) {             /* test over: final values */
        test_sum_put(&sum);
        sum_saved_us = 0;
    }

    s_temp_start = (s_hb.valid && s_hb.at_start && s_hb.temp0_dc != INT16_MIN && s_hb.test == o->test_no)
                   ? s_hb.temp0_dc / 10.0f : -999;

    static health_in_t in;
    static health_out_t out;
    memset(&in, 0, sizeof in);
    in.discharging = discharging;
    in.elapsed_s = o->elapsed_s;
    in.alarms = o->alarms;
    in.alarm_id = o->alarm_id;
    in.v_tester = o->tester_live ? o->voltage_v : 0;
    in.i_tester = o->tester_live ? fabsf(o->current_a) : 0;
    in.i_set = o->tester_live ? o->i_set_a : 0;
    in.rated_v = o->tester_live ? o->rated_v : 0;
    in.bms = o->bms_live && o->cell_count >= 1;
    in.v_bms = o->bms_pack_v;
    in.cells = o->cell_count;
    in.cell_min_mv = o->cell_min_mv;
    in.cell_max_mv = o->cell_max_mv;
    in.cell_min_idx = o->cell_min_idx;
    in.has_temp = has_t; in.temp_bat = tbat;
    in.has_mos = has_mos; in.temp_mos = tmos;
    in.has_base = s_hb.valid && s_hb.test == o->test_no && o->tester_live;
    in.delta0_mv = s_hb.delta0_mv;
    in.temp0 = s_hb.temp0_dc != INT16_MIN ? s_hb.temp0_dc / 10.0f : tbat;
    in.chem = (chem_t)s_chem;

    /* judge the 10-second average, not single readings: a cell value moving by
     * 1-2 mV around a limit must not flip the verdict (20 ticks of 0.5 s) */
    {
        enum { AVG_N = 10000 / TICK_MS };
        static float hv[AVG_N][7];
        static int hn, hi;
        if (!in.bms) hn = 0;                      /* BMS lost: start over */
        else {
            float *r = hv[hi];
            r[0] = in.cell_min_mv; r[1] = in.cell_max_mv; r[2] = in.i_tester;
            r[3] = in.temp_bat; r[4] = in.temp_mos; r[5] = in.v_tester; r[6] = in.v_bms;
            hi = (hi + 1) % AVG_N;
            if (hn < AVG_N) hn++;
            float s[7] = { 0 };
            for (int k = 0; k < hn; k++) for (int j = 0; j < 7; j++) s[j] += hv[k][j];
            in.cell_min_mv = (uint16_t)lroundf(s[0] / hn);
            in.cell_max_mv = (uint16_t)lroundf(s[1] / hn);
            in.i_tester = s[2] / hn;
            in.temp_bat = s[3] / hn; in.temp_mos = s[4] / hn;
            in.v_tester = s[5] / hn; in.v_bms = s[6] / hn;
        }
    }
    health_eval(&in, &out);

    /* debounce the verdict (values near a limit must not make it flicker):
     * a worse level shows after 3 s, a better one after 10 s, "no BMS" after
     * 20 s (it reconnects by itself after a restart); gauges are always live */
    static health_out_t shown;
    static int pend;
    if (out.level == shown.level || out.level == HL_NONE || (shown.level == HL_NONE && out.level == HL_OK)) {
        pend = 0;
        if (out.level != shown.level || out.reason != shown.reason)
            ESP_LOGW(TAG, "health: level %d reason %d cell %d a=%.2f b=%.2f", out.level, out.reason, out.cell, out.a, out.b);
        shown = out;
    } else {
        int need = out.level < shown.level ? 10000 / TICK_MS
                 : out.reason == HR_NO_BMS ? 20000 / TICK_MS : 3000 / TICK_MS;
        if (++pend >= need) {
            pend = 0;
            ESP_LOGW(TAG, "health: level %d reason %d cell %d a=%.2f b=%.2f", out.level, out.reason, out.cell, out.a, out.b);
            shown = out;
        } else {
            memcpy(shown.g, out.g, sizeof shown.g);
            shown.chem = out.chem;
        }
    }
    if (lv_screen_active() == s_scr[TAB_ONLINE]) ui_online_health(&in, &shown);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    int64_t t0 = esp_timer_get_time();
    static int chart_div, last_live = -1, last_tlive = -1, last_tstate = -1;
    online_sample_t o = {0};
    fill_sample(&o);
    (void)last_live;   /* BMS appearing mid-test just starts its own series (no reset) */
    if (o.tester_live != last_tlive) {   /* tester data appeared/vanished: start a fresh curve */
        if (last_tlive >= 0) hist_reset();
        last_tlive = o.tester_live;
        /* joined a test already running: seed the energy with V x Ah */
        s_live_wh = (o.tester_live && o.state != TS_IDLE) ? o.voltage_v * o.capacity_ah : 0;
    }
    if (o.tester_live) {
        /* fill in the Ah / seconds between whole-unit tester updates */
        static int last_ah = -1, last_min = -1;
        static int64_t min_us;
        static bool t_synced;      /* seconds known only after a minute change */
        int ah_int = (int)(o.capacity_ah - s_ah_extra + 0.5f);
        int min_int = (int)((o.elapsed_s - s_sec_extra) / 60u);
        int64_t now = esp_timer_get_time();
        if (ah_int != last_ah) { last_ah = ah_int; s_ah_extra = 0; }
        else if (o.state == TS_DISCHARGE && o.current_a < 0) {
            s_ah_extra += -o.current_a * (TICK_MS / 1000.0f) / 3600.0f;
            if (s_ah_extra > 0.99f) s_ah_extra = 0.99f;
        }
        if (min_int != last_min) {
            t_synced = last_min >= 0;          /* first value seen may be mid-minute */
            last_min = min_int; min_us = now; s_sec_extra = 0;
        }
        else if (o.state == TS_DISCHARGE) {
            int64_t sec = (now - min_us) / 1000000;
            s_sec_extra = (uint32_t)(sec > 59 ? 59 : sec);
        }
        /* o was filled with the previous fractions: refresh it, otherwise the
         * sample at each new minute is stamped ~1 min late and the curve
         * zig-zags back in time */
        o.capacity_ah = (float)ah_int + s_ah_extra;
        o.elapsed_s = (uint32_t)min_int * 60u + s_sec_extra;
        bool running = o.state == TS_DISCHARGE || o.state == TS_REST;
        bool was_running = last_tstate == TS_DISCHARGE || last_tstate == TS_REST;
        if (running && !was_running && o.elapsed_s < 180) {   /* a new test started */
            hist_reset();
            s_live_wh = 0;
        }
        if (o.state == TS_DISCHARGE && o.current_a < 0)
            s_live_wh += o.voltage_v * -o.current_a * (TICK_MS / 1000.0f) / 3600.0f;
        if (running && t_synced) hist_add(&o);

        /* BMS of the running test: re-join it after a restart, forget it when
         * the test is over (the next battery has its own BMS) */
        static bool resume_tried;
        if (running && !o.bms_live && !resume_tried) {
            bms_ble_state_t bs = bms_ble_state(NULL, 0);
            if (bs == BMS_BLE_IDLE || bs == BMS_BLE_ERROR) {
                bms_ble_resume_last();
                resume_tried = true;
            }
        }
        if (was_running && !running && (o.state == TS_DONE || o.state == TS_IDLE)) {
            sd_arch_request_sync();
            bms_ble_forget_last();
            resume_tried = false;
        }
        last_tstate = o.state;
    }
    health_tick(&o);
    if (lv_screen_active() == s_scr[TAB_ONLINE]) ui_online_update(&o);
    if (++chart_div >= 4) { chart_div = 0; ui_online_chart_refresh(); }   /* ~2 s */
    frame_refresh(&o);

    /* cost of our own UI work per tick (render time is on the perf monitor) */
    static int64_t sum, mx;
    static int cnt;
    int64_t dt = esp_timer_get_time() - t0;
    sum += dt;
    if (dt > mx) mx = dt;
    if (++cnt == 20) {
        ESP_LOGD(TAG, "tick avg %lld us, max %lld us", sum / cnt, mx);
        sum = mx = 0;
        cnt = 0;
    }
}

/* --------------------------------------------------------------- print -- */

/* LVGL is driven by the esp_lvgl_port task; any lv_* call from another task
 * must hold the port lock (bsp_display_lock). */
static void toast_locked(const char *msg)
{
    bsp_display_lock(0);
    ui_frame_toast(msg);
    bsp_display_unlock();
}

static void print_done(esp_err_t e)
{
    ESP_LOGI(TAG, "print result: %s", esp_err_to_name(e));
    char msg[64];
    snprintf(msg, sizeof msg, "%s  %s", e == ESP_OK ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
             tr(e == ESP_OK ? S_PR_OK : e == ESP_ERR_NOT_FOUND ? S_PR_NOT_FOUND :
                e == ESP_ERR_INVALID_STATE ? S_PR_NO_WIFI : S_PR_FAILED));
    toast_locked(msg);
    s_printing = false;
}

/* BMS snapshot for the protocol. LVGL context (or display locked). */
static void doc_extra_fill(doc_extra_t *x)
{
    online_sample_t o = {0};
    fill_sample(&o);
    memset(x, 0, sizeof *x);
    x->cycle = BLT_ARCH_CURRENT;
    x->chem = health_chem_get();
    x->detailed = app_protocol_detailed();
    x->temp_start = s_temp_start;
    x->live_ah = o.tester_live ? o.capacity_ah : 0;
    if (o.bms_live && o.cell_count) {
        x->bms = true;
        strlcpy(x->bms_name, o.bms_name, sizeof x->bms_name);
        x->cells = o.cell_count;
        x->cell_min_mv = o.cell_min_mv;
        x->cell_max_mv = o.cell_max_mv;
        x->imbalance_mv = o.imbalance_mv;
        x->soc_pct = o.soc_pct;
        uint16_t worst = 0;
        for (int i = 0; i < hist_count(); i++) {
            const hist_pt_t *p = hist_at(i);
            uint16_t lo = 0xFFFF, hi = 0;
            for (int c = 0; c < p->n; c++) {
                if (p->cell[c] < lo) lo = p->cell[c];
                if (p->cell[c] > hi) hi = p->cell[c];
            }
            if (p->n >= 2 && hi - lo > worst) worst = hi - lo;
        }
        x->imbalance_max_mv = worst;
    }
}

/* Print the protocol of recorder cycle `cycle` (BLT_ARCH_CURRENT = the
 * running / last test). LVGL context. Returns false if a print is running. */
bool app_print_protocol(uint16_t cycle)
{
    if (s_printing) return false;
    doc_extra_t x;
    doc_extra_fill(&x);
    x.cycle = cycle;
    if (!doc_print_protocol(&x, true, print_done)) return false;
    s_printing = true;
    char msg[48];
    snprintf(msg, sizeof msg, "%s  %s", LV_SYMBOL_FILE, tr(S_PR_PRINTING));
    ui_frame_toast(msg);
    return true;
}

/* Runs in the LVGL context (tab-bar button): print the test protocol. */
static void on_print(void)
{
    if (s_printing) return;
    ESP_LOGI(TAG, "PRINT tapped");
    doc_extra_t x;
    doc_extra_fill(&x);
    if (!doc_print_protocol(&x, true, print_done)) return;
    s_printing = true;
    char msg[48];
    snprintf(msg, sizeof msg, "%s  %s", LV_SYMBOL_FILE, tr(S_PR_PRINTING));
    ui_frame_toast(msg);
}

/* dev console: PROTO (log the page data) / PROTOPRINT */
void snap_protocol(bool print, uint16_t cycle)
{
    bsp_display_lock(0);
    if (print) on_print();
    else {
        doc_extra_t x;
        doc_extra_fill(&x);
        x.cycle = cycle;
        doc_print_protocol(&x, false, NULL);
    }
    bsp_display_unlock();
}

/* the running test, every 10 s, to the SD card (tester + BMS + cells) */
static void sd_log_task(void *arg)
{
    (void)arg;
    bool synced_once = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        online_sample_t o = {0};
        fill_sample(&o);
        if (!synced_once && o.tester_live && sd_store_ready()) {   /* first contact: copy finished tests */
            synced_once = true;
            sd_arch_request_sync();
        }
        if (!o.tester_live || !(o.state == TS_DISCHARGE || o.state == TS_REST)) continue;
        uint16_t cycle;
        taskENTER_CRITICAL(&s_live_mux);
        cycle = s_live.pr_cycle;
        taskEXIT_CRITICAL(&s_live_mux);
        sd_arch_log(&o, cycle);
    }
}

/* ----------------------------------------------------------- bring-up -- */

static void netw_task(void *arg)
{
    (void)arg;
    sd_store_start();                       /* SD card for the archive (optional) */
    if (!netw_start()) {   /* C6 link, co-processor update, Wi-Fi */
        vTaskDelete(NULL);
        return;
    }
    bms_ble_start();       /* BLE host over the same C6 */
    if (netw_wait_connected(60000)) {
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sc);
    }
    vTaskDelete(NULL);
}

static void nav_cb(tab_t tab)
{
    if (tab < TAB__COUNT && s_scr[tab] && lv_screen_active() != s_scr[tab]) {
        lv_screen_load(s_scr[tab]);
        if (tab == TAB_ONLINE) ui_online_chart_refresh();
    }
}

void snap_nav(int tab)
{
    if (tab < 0 || tab >= TAB__COUNT) return;
    bsp_display_lock(0);
    nav_cb((tab_t)tab);
    bsp_display_unlock();
}

/* Log where each touch lands (development aid: checks the rotated mapping). */
static void touch_log_cb(lv_timer_t *t)
{
    static bool was;
    lv_indev_t *in = (lv_indev_t *)lv_timer_get_user_data(t);
    bool now = lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED;
    if (now && !was) {
        lv_point_t p;
        lv_indev_get_point(in, &p);
        lv_obj_t *o = lv_indev_search_obj(lv_screen_active(), &p);
        const char *what = "-";
        if (o && lv_obj_check_type(o, &lv_label_class)) what = lv_label_get_text(o);
        else if (o && lv_obj_get_child_count(o) && lv_obj_check_type(lv_obj_get_child(o, 0), &lv_label_class))
            what = lv_label_get_text(lv_obj_get_child(o, 0));
        else if (o) what = "(obj)";
        ESP_LOGD("touch", "press %d,%d -> %.40s", (int)p.x, (int)p.y, what);
    }
    was = now;
}

void snap_bms_connect(const char *name)
{
    bms_ble_dev_t devs[12];
    int n = bms_ble_scan(devs, 12, 5000);
    int pick = -1;
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "console: %-5s %-24s %d dBm", bms_kind_str(devs[i].kind), devs[i].name, devs[i].rssi);
        if (name && *name) {
            if (pick < 0 && strcmp(devs[i].name, name) == 0) pick = i;
            continue;
        }
        if (devs[i].kind == BMS_KIND_DALY && (pick < 0 || devs[pick].kind != BMS_KIND_DALY)) pick = i;
        else if (devs[i].kind != BMS_KIND_UNKNOWN && pick < 0) pick = i;
    }
    for (int i = (pick < 0 ? n : pick); i < n; i++) {   /* Daly preferred, else first JK */
        if (devs[i].kind != BMS_KIND_UNKNOWN) {
            ESP_LOGI(TAG, "console: connecting to %s", devs[i].name);
            bms_ble_connect(&devs[i]);
            return;
        }
    }
    ESP_LOGW(TAG, "console: no JK/Daly found (%d devices)", n);
}

void snap_bms_disconnect(void) { bms_ble_disconnect(); }

void snap_cell(int n)
{
    bsp_display_lock(0);
    ui_online_select_cell(n - 1);
    bsp_display_unlock();
}

void snap_blt_once(void)
{
    if (blt_usb_state(NULL, NULL) != BLT_USB_CONNECTED) { ESP_LOGW(TAG, "BLTONCE: tester not connected"); return; }
    static blt_response_t r;
    static uint8_t blk[BLT_LIVE_LEN];
    bool was = blt_usb_reads_allowed();
    blt_usb_allow_reads(true);
    esp_err_t e = ESP_OK;
    for (int i = 0; i < 3 && e == ESP_OK; i++) {
        blt_err_t be = BLT_OK;
        e = blt_usb_read(BLT_CMD_RI, (uint32_t)(i * 64), 64, &r, &be, 2600);
        if (e == ESP_OK && r.payload_len == 64) memcpy(blk + i * 64, r.payload, 64);
        else if (e == ESP_OK) e = ESP_ERR_INVALID_SIZE;
    }
    blt_usb_allow_reads(was);
    if (e != ESP_OK) { ESP_LOGW(TAG, "BLTONCE failed: %s", esp_err_to_name(e)); return; }
    for (int i = 0; i < BLT_LIVE_LEN; i += 16) ESP_LOG_BUFFER_HEX(TAG, blk + i, 16);
    blt_live_t l;
    blt_live_decode(blk, sizeof blk, &l);
    int first = 0, na = blt_live_alarm_count(&l, &first);
    ESP_LOGI(TAG, "LIVE: Vbat %.1f V  Vload %.1f V  Ichg %.1f A  Idis %.1f A", l.v_batt, l.v_load, l.i_charge, l.i_discharge);
    ESP_LOGI(TAG, "LIVE: T %.1f / %.1f / %.1f / %.1f C  flags %04x  out %02x %02x",
             l.temp_c[0], l.temp_c[1], l.temp_c[2], l.temp_c[3], l.flags, l.outputs1, l.outputs2);
    ESP_LOGI(TAG, "LIVE: clock %02u:%02u:%02u %02u/%02u/%02u  t_dis %u min  t_chg %u min  pause %u/%u",
             l.rtc_h, l.rtc_m, l.rtc_s, l.rtc_d, l.rtc_mo, l.rtc_y, l.t_discharge_min, l.t_charge_min,
             l.t_discharge_pause_min, l.t_charge_pause_min);
    ESP_LOGI(TAG, "LIVE: Ah present %u  charged %u  discharged %u  eff %u  Vmin %.2f Vmax %.2f",
             l.ah_present, l.ah_charged, l.ah_discharged, l.efficiency_pct, l.v_min, l.v_max);
    ESP_LOGI(TAG, "LIVE: state %u (%s)  cycle %u/%u  test %u  alarms %d (first id %d)",
             l.state, blt_state_name_it(l.state), l.n_charges, l.n_discharges, l.test_no, na, first);
}

void snap_chart(int mode)
{
    bsp_display_lock(0);
    ui_online_set_chart_mode(mode);
    bsp_display_unlock();
}

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    i18n_init();
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
    tzset();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = { .buff_dma = true, .buff_spiram = true, .sw_rotate = true }
    };
    /* LVGL owns core 0; heavy background work (printing) runs on core 1 */
    cfg.lvgl_port_cfg.task_affinity = 0;
    /* the panel is 800x1280 portrait; the bench is used in landscape (PPA rotates) */
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    bsp_display_lock(0);
    bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90);
    bsp_display_unlock();
    /* The PPA turns the picture the opposite way to LVGL's ROTATION_90, so
     * touches landed 180 deg off (tap at x,y arrived at 1279-x, 799-y).
     * Flip the touch controller's mapping to match the picture. */
    esp_lcd_touch_handle_t tp = bsp_touch_get_handle();
    if (tp) {
        bool mx = false, my = false;
        esp_lcd_touch_get_mirror_x(tp, &mx);
        esp_lcd_touch_get_mirror_y(tp, &my);
        esp_lcd_touch_set_mirror_x(tp, !mx);
        esp_lcd_touch_set_mirror_y(tp, !my);
    }
    bsp_display_backlight_on();

    hist_init();

    bsp_display_lock(0);
    fonts_init();
    s_scr[TAB_ONLINE]   = ui_online_create();
    s_scr[TAB_TEST]     = ui_test_create();
    s_scr[TAB_ARCHIVE]  = ui_archive_create();
    s_scr[TAB_BMS]      = ui_bms_create();
    s_scr[TAB_PRINTER]  = ui_printer_create();
    s_scr[TAB_SETTINGS] = ui_wifi_create();
    for (int i = 0; i < TAB__COUNT; i++) ui_frame_attach(s_scr[i], (tab_t)i);
    ui_frame_set_nav_cb(nav_cb);
    ui_frame_set_print_cb(on_print);
    lv_screen_load(s_scr[TAB_ONLINE]);
    online_sample_t o = {0};
    fill_sample(&o);
    ui_online_update(&o);
    frame_refresh(&o);
    lv_timer_create(tick_cb, TICK_MS, NULL);
    lv_indev_t *touch = bsp_display_get_input_dev();
    if (touch) lv_timer_create(touch_log_cb, 30, touch);
    bsp_display_unlock();

    snap_start();
    blt_usb_start();     /* tester on USB - passive until reads are allowed */
    blt_live_poll_restore();
    test_sum_load();                     /* NVS -> RAM before any PSRAM-stack task reads it */
    psram_task(blt_poll_task, "blt_poll", 6144, NULL, 4, NULL, tskNO_AFFINITY);
    psram_task(sd_log_task, "sd_log", 6144, NULL, 2, NULL, tskNO_AFFINITY);

    /* Wi-Fi via the C6 runs in its own task so the UI stays live if it stalls */
    xTaskCreate(netw_task, "netw", 6144, NULL, 5, NULL);

    ESP_LOGI(TAG, "UI up (%s); Wi-Fi bring-up started", i18n_lang() == LANG_UA ? "UA" : "EN");
}

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
#include "fonts.h"
#include "i18n.h"
#include "hist.h"
#include "snap.h"
#include "bms_ble.h"
#include "netw.h"
#include "print_job.h"
#include "report.h"

static const char *TAG = "blt";

#define SIM_CELLS       16
#define SIM_NOMINAL_AH  50.0f
#define SIM_DT_S        60.0f
#define TICK_MS         500

typedef struct {
    float v, i, ah, wh, temp;
    uint32_t t;
    bool done;
} sim_state_t;

static sim_state_t s_sim;

/* print snapshot (kept alive for the print task) */
static volatile bool s_printing;
static report_data_t s_rep;
static uint16_t      s_rep_cells[UI_MAX_CELLS];
static float         s_rep_v[HIST_MAX], s_rep_i[HIST_MAX], s_rep_d[HIST_MAX];
static char          s_rep_date[32], s_rep_batt[40];

/* ------------------------------------------------------------------ sim -- */

static void sim_reset(sim_state_t *s)
{
    *s = (sim_state_t){ .v = 53.3f, .i = -12.0f, .temp = 26.5f };
}

static void sim_step(sim_state_t *s, float dt_s)
{
    if (s->done) return;
    float ia = fabsf(s->i);
    s->ah += ia * dt_s / 3600.0f;
    s->wh += s->v * ia * dt_s / 3600.0f;
    s->t += (uint32_t)dt_s;
    s->temp = 26.5f + s->ah * 0.25f;
    s->v = 53.3f - s->ah * 0.22f;
    if (s->v < 44.0f) s->v = 44.0f - (s->ah - (53.3f - 44.0f) / 0.22f) * 0.15f;
    if (s->v <= 42.0f) { s->v = 42.0f; s->i = 0.0f; s->done = true; }
}

static void sim_fill_sample(const sim_state_t *s, online_sample_t *o)
{
    o->tester_live = false;
    o->state = s->done ? TS_DONE : TS_DISCHARGE;
    o->voltage_v = s->v;
    o->current_a = s->i;
    o->capacity_ah = s->ah;
    o->energy_wh = s->wh;
    o->elapsed_s = s->t;
    o->temp_c = s->temp;

    o->bms_present = true;
    o->bms_live = false;
    strlcpy(o->bms_name, "SIM", sizeof o->bms_name);
    float soc = 100.0f - (s->ah / SIM_NOMINAL_AH) * 100.0f;
    if (soc < 0) soc = 0;
    o->soc_pct = (uint8_t)(soc + 0.5f);
    o->bms_pack_v = s->v - 0.05f;
    o->bms_current_a = s->i;
    o->cell_count = SIM_CELLS;
    o->temp_count = 3;
    o->temp_dc[0] = (int16_t)((s->temp + 0.8f) * 10);
    o->temp_dc[1] = (int16_t)((s->temp + 0.3f) * 10);
    o->temp_dc[2] = (int16_t)((s->temp + 4.0f) * 10);

    /* cells drift apart towards the end of the discharge; #7 is the weak one */
    float base_mv = (s->v * 1000.0f) / SIM_CELLS;
    float k = (100.0f - soc) / 100.0f;
    float spread = 3.0f + k * k * 60.0f;
    for (int c = 0; c < SIM_CELLS; c++) {
        float off = ((c * 7) % 5 - 2) * spread * 0.25f;
        if (c == 6) off -= spread;
        if (c == 11) off += spread * 0.5f;
        o->cell_mv[c] = (uint16_t)(base_mv + off);
    }
    o->balance_bits = k > 0.3f ? (1u << 11) : 0;
    ui_sample_finish_cells(o);
}

/* Tester side is still simulated; the BMS side uses the real BLE BMS when it
 * has delivered a frame in the last 5 s. */
static void fill_sample(online_sample_t *o)
{
    sim_fill_sample(&s_sim, o);
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
    snprintf(state, sizeof state, "%s  ·  %s", tr(ST[o->state & 3]), hms);

    frame_status_t st = {
        .state_text = state,
        .state_ind = o->state == TS_DISCHARGE ? IND_OK : o->state == TS_IDLE ? IND_OFF : IND_BUSY,
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

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    int64_t t0 = esp_timer_get_time();
    static int done_hold, chart_div, last_live = -1;
    if (s_sim.done) {
        if (++done_hold > 20) { done_hold = 0; sim_reset(&s_sim); hist_reset(); }
    } else {
        sim_step(&s_sim, SIM_DT_S);
    }
    online_sample_t o = {0};
    fill_sample(&o);
    if (o.bms_live != last_live) {   /* don't mix simulated and real BMS data in one curve */
        if (last_live >= 0) hist_reset();
        last_live = o.bms_live;
    }
    if (!s_sim.done) hist_add(&o);
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

static void print_task(void *arg)
{
    (void)arg;
    esp_err_t e = print_job_send(&s_rep);
    ESP_LOGI(TAG, "print result: %s", esp_err_to_name(e));
    char msg[64];
    snprintf(msg, sizeof msg, "%s  %s", e == ESP_OK ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
             tr(e == ESP_OK ? S_PR_OK : e == ESP_ERR_NOT_FOUND ? S_PR_NOT_FOUND :
                e == ESP_ERR_INVALID_STATE ? S_PR_NO_WIFI : S_PR_FAILED));
    toast_locked(msg);
    s_printing = false;
    vTaskDelete(NULL);
}

/* Runs in the LVGL context (tab-bar button). */
static void on_print(void)
{
    if (s_printing) return;
    ESP_LOGI(TAG, "PRINT tapped");
    online_sample_t o = {0};
    fill_sample(&o);
    for (int i = 0; i < o.cell_count && i < UI_MAX_CELLS; i++) s_rep_cells[i] = o.cell_mv[i];

    /* curves from the history */
    int n = hist_count();
    float vmin = 1e9f, vmax = 0, imax = 1, min_v = o.voltage_v, max_t = o.temp_c;
    for (int i = 0; i < n; i++) {
        const hist_pt_t *p = hist_at(i);
        s_rep_v[i] = p->v_t;
        s_rep_i[i] = fabsf(p->i_t);
        uint16_t lo = 0xFFFF, hi = 0;
        for (int c = 0; c < p->n; c++) {
            if (p->cell[c] < lo) lo = p->cell[c];
            if (p->cell[c] > hi) hi = p->cell[c];
        }
        s_rep_d[i] = p->n ? (float)(hi - lo) : 0;
        if (p->v_t < vmin) vmin = p->v_t;
        if (p->v_t > vmax) vmax = p->v_t;
        if (s_rep_i[i] > imax) imax = s_rep_i[i];
        if (p->v_t < min_v) min_v = p->v_t;
    }
    if (n == 0) { vmin = 40; vmax = 55; }
    vmin = floorf(vmin - 1.0f);
    vmax = ceilf(vmax + 1.0f);

    struct tm tm;
    if (clock_valid(&tm)) strftime(s_rep_date, sizeof s_rep_date, "%Y-%m-%d %H:%M", &tm);
    else strlcpy(s_rep_date, "n/a", sizeof s_rep_date);
    snprintf(s_rep_batt, sizeof s_rep_batt, "%uS  %.20s", o.cell_count, o.bms_live ? o.bms_name : "SIM");

    s_rep = (report_data_t){
        .title = "BATTERY TEST REPORT",
        .date = s_rep_date,
        .operator_name = "-",
        .battery_id = s_rep_batt,
        .nominal_v = 51.2f, .nominal_ah = SIM_NOMINAL_AH, .discharge_a = 12.0f,
        .discharged_ah = o.capacity_ah, .duration_s = o.elapsed_s,
        .min_voltage_v = min_v, .max_temp_c = max_t, .cutoff_v = 42.0f,
        .curve_v = s_rep_v, .curve_i = s_rep_i, .curve_n = n,
        .curve_vmin = vmin, .curve_vmax = vmax, .curve_imax = ceilf(imax * 1.2f),
        .curve_delta_mv = s_rep_d,
        .bms_present = o.bms_present, .cell_mv = s_rep_cells, .cell_count = o.cell_count,
        .cell_min_mv = o.cell_min_mv, .cell_max_mv = o.cell_max_mv,
        .imbalance_mv = o.imbalance_mv, .soc_pct = o.soc_pct,
    };
    s_printing = true;
    char msg[48];
    snprintf(msg, sizeof msg, "%s  %s", LV_SYMBOL_FILE, tr(S_PR_PRINTING));
    ui_frame_toast(msg);
    xTaskCreate(print_task, "print", 8192, NULL, 4, NULL);
}

/* ----------------------------------------------------------- bring-up -- */

static void netw_task(void *arg)
{
    (void)arg;
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

void snap_bms_connect(void)
{
    bms_ble_dev_t devs[12];
    int n = bms_ble_scan(devs, 12, 5000);
    for (int i = 0; i < n; i++) {
        if (devs[i].is_jk) {
            ESP_LOGI(TAG, "console: connecting to %s", devs[i].name);
            bms_ble_connect(&devs[i]);
            return;
        }
    }
    ESP_LOGW(TAG, "console: no JK found (%d devices)", n);
}

void snap_bms_disconnect(void) { bms_ble_disconnect(); }

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

    sim_reset(&s_sim);
    hist_init();

    bsp_display_lock(0);
    fonts_init();
    s_scr[TAB_ONLINE]   = ui_online_create();
    s_scr[TAB_ARCHIVE]  = ui_archive_create();
    s_scr[TAB_BMS]      = ui_bms_create();
    s_scr[TAB_PRINTER]  = ui_printer_create();
    s_scr[TAB_SETTINGS] = ui_wifi_create();
    for (int i = 0; i < TAB__COUNT; i++) ui_frame_attach(s_scr[i], (tab_t)i);
    ui_frame_set_nav_cb(nav_cb);
    ui_frame_set_print_cb(on_print);
    lv_screen_load(s_scr[TAB_ONLINE]);
    online_sample_t o = {0};
    sim_fill_sample(&s_sim, &o);
    ui_online_update(&o);
    frame_refresh(&o);
    lv_timer_create(tick_cb, TICK_MS, NULL);
    lv_indev_t *touch = bsp_display_get_input_dev();
    if (touch) lv_timer_create(touch_log_cb, 30, touch);
    bsp_display_unlock();

    snap_start();

    /* Wi-Fi via the C6 runs in its own task so the UI stays live if it stalls */
    xTaskCreate(netw_task, "netw", 6144, NULL, 5, NULL);

    ESP_LOGI(TAG, "UI up (%s); Wi-Fi bring-up started", i18n_lang() == LANG_UA ? "UA" : "EN");
}

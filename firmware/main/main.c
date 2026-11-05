/*
 * blt-esp-display firmware entry.
 * Brings up the board (BSP + LVGL), shows the OnLine screen driven by a synthetic
 * discharge (until the USB link to a real BLT tester is wired), connects Wi-Fi via
 * the on-board ESP32-C6, and prints the report over IPP on the PRINT button.
 */
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui.h"
#include "netw.h"
#include "print_job.h"
#include "report.h"

static const char *TAG = "blt";

#define SIM_CELLS       14
#define SIM_NOMINAL_AH  50.0f
#define SIM_DT_S        30.0f
#define HISTP           200

typedef struct {
    float v, i, ah, temp;
    uint32_t t;
    bool done;
} sim_state_t;

static sim_state_t s_sim;
static float s_hist_v[HISTP], s_hist_i[HISTP];
static int   s_hist_n;

/* print snapshot (kept alive for the print task) */
static volatile bool s_printing;
static report_data_t s_rep;
static uint16_t      s_rep_cells[UI_MAX_CELLS];

static void sim_reset(sim_state_t *s)
{
    s->v = 53.3f; s->i = -12.0f; s->ah = 0.0f; s->t = 0; s->temp = 26.5f; s->done = false;
    s_hist_n = 0;
}

static void sim_step(sim_state_t *s, float dt_s)
{
    if (s->done) return;
    s->ah += (s->i < 0 ? -s->i : s->i) * dt_s / 3600.0f;
    s->t += (uint32_t)dt_s;
    s->temp = 26.5f + s->ah * 0.25f;
    s->v = 53.3f - s->ah * 0.22f;
    if (s->v < 44.0f) s->v = 44.0f - (s->ah - (53.3f - 44.0f) / 0.22f) * 0.15f;
    if (s->v <= 42.0f) { s->v = 42.0f; s->i = 0.0f; s->done = true; }

    if (s_hist_n < HISTP) {
        s_hist_v[s_hist_n] = s->v;
        s_hist_i[s_hist_n] = (s->i < 0 ? -s->i : s->i);
        s_hist_n++;
    }
}

static void sim_fill_sample(const sim_state_t *s, online_sample_t *o)
{
    o->voltage_v = s->v;
    o->current_a = s->i;
    o->capacity_ah = s->ah;
    o->elapsed_s = s->t;
    o->temp_c = s->temp;
    o->state = s->done ? "DONE" : "DISCHARGE";

    o->bms_present = true;
    float soc = 100.0f - (s->ah / SIM_NOMINAL_AH) * 100.0f;
    if (soc < 0) soc = 0;
    o->soc_pct = (uint8_t)(soc + 0.5f);
    o->bms_pack_v = s->v;
    o->bms_temp_c = s->temp + 1.0f;
    o->cell_count = SIM_CELLS;

    float base_mv = (s->v * 1000.0f) / SIM_CELLS;
    float spread = 4.0f + (100.0f - soc) * 0.6f;
    uint16_t lo = 0xFFFF, hi = 0;
    for (int c = 0; c < SIM_CELLS; c++) {
        float off = ((c % 5) - 2) * (spread / 2.0f);
        if (c == SIM_CELLS - 1) off -= spread;
        uint16_t mv = (uint16_t)(base_mv + off);
        o->cell_mv[c] = mv;
        if (mv < lo) lo = mv;
        if (mv > hi) hi = mv;
    }
    o->cell_min_mv = lo; o->cell_max_mv = hi; o->imbalance_mv = (uint16_t)(hi - lo);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    static int done_hold;
    if (s_sim.done) {
        if (++done_hold > 24) { done_hold = 0; sim_reset(&s_sim); }
    } else {
        sim_step(&s_sim, SIM_DT_S);
    }
    online_sample_t o = {0};
    sim_fill_sample(&s_sim, &o);
    ui_online_update(&o);
}

static void print_task(void *arg)
{
    (void)arg;
    ui_online_set_status("printing...");
    esp_err_t e = print_job_send(&s_rep);
    ESP_LOGI(TAG, "print result: %s", esp_err_to_name(e));
    ui_online_set_status(e == ESP_OK ? "printed OK" :
                         e == ESP_ERR_NOT_FOUND ? "printer not found" :
                         e == ESP_ERR_INVALID_STATE ? "no Wi-Fi" : "print failed");
    s_printing = false;
    vTaskDelete(NULL);
}

static void on_print(void)
{
    if (s_printing) return;
    online_sample_t o = {0};
    sim_fill_sample(&s_sim, &o);
    for (int i = 0; i < o.cell_count && i < UI_MAX_CELLS; i++) s_rep_cells[i] = o.cell_mv[i];

    s_rep = (report_data_t){
        .title = "BATTERY TEST REPORT",
        .date = "sim run",
        .operator_name = "SIM",
        .battery_id = "LFP-14S-001",
        .nominal_v = 51.8f, .nominal_ah = SIM_NOMINAL_AH, .discharge_a = 12.0f,
        .discharged_ah = o.capacity_ah, .duration_s = o.elapsed_s,
        .min_voltage_v = o.voltage_v, .max_temp_c = o.temp_c, .cutoff_v = 42.0f,
        .curve_v = s_hist_v, .curve_i = s_hist_i, .curve_n = s_hist_n,
        .curve_vmin = 40.0f, .curve_vmax = 55.0f, .curve_imax = 20.0f,
        .bms_present = true, .cell_mv = s_rep_cells, .cell_count = o.cell_count,
        .cell_min_mv = o.cell_min_mv, .cell_max_mv = o.cell_max_mv,
        .imbalance_mv = o.imbalance_mv, .soc_pct = o.soc_pct,
    };
    s_printing = true;
    xTaskCreate(print_task, "print", 8192, NULL, 4, NULL);
}

static void netw_task(void *arg)
{
    (void)arg;
    netw_start();
    vTaskDelete(NULL);
}

void app_main(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = { .buff_dma = true, .buff_spiram = true, .sw_rotate = false }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    sim_reset(&s_sim);

    bsp_display_lock(0);
    ui_online_create();
    ui_online_set_print_cb(on_print);
    online_sample_t o = {0};
    sim_fill_sample(&s_sim, &o);
    ui_online_update(&o);
    lv_timer_create(tick_cb, 250, NULL);
    bsp_display_unlock();

    /* Wi-Fi via the C6 runs in its own task so the UI stays live if it stalls */
    xTaskCreate(netw_task, "netw", 6144, NULL, 5, NULL);

    ESP_LOGI(TAG, "online screen up; Wi-Fi bring-up started");
}

/*
 * blt-esp-display firmware entry.
 * Brings up the board (BSP + LVGL) and shows the OnLine screen. Until the USB
 * link to a real BLT tester is wired, a synthetic discharge drives the UI so
 * the screen and graph can be developed and demoed without hardware.
 */
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui.h"

static const char *TAG = "blt";

#define SIM_CELLS       14
#define SIM_NOMINAL_AH  50.0f
#define SIM_DT_S        30.0f   /* simulated seconds advanced per UI tick */

typedef struct {
    float    v;      /* pack V */
    float    i;      /* A, negative = discharge */
    float    ah;     /* discharged capacity */
    uint32_t t;      /* elapsed seconds */
    float    temp;   /* C */
    bool     done;
} sim_state_t;

static sim_state_t s_sim;

static void sim_reset(sim_state_t *s)
{
    s->v = 53.3f;
    s->i = -12.0f;
    s->ah = 0.0f;
    s->t = 0;
    s->temp = 26.5f;
    s->done = false;
}

static void sim_step(sim_state_t *s, float dt_s)
{
    if (s->done) return;
    s->ah += (s->i < 0 ? -s->i : s->i) * dt_s / 3600.0f;
    s->t += (uint32_t)dt_s;
    s->temp = 26.5f + s->ah * 0.25f;
    /* linear sag with a knee near the end */
    s->v = 53.3f - s->ah * 0.22f;
    if (s->v < 44.0f) s->v = 44.0f - (s->ah - (53.3f - 44.0f) / 0.22f) * 0.15f;
    if (s->v <= 42.0f) {
        s->v = 42.0f;
        s->i = 0.0f;
        s->done = true;
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

    /* per-cell voltages around pack/N, with an imbalance that grows as SoC drops */
    float base_mv = (s->v * 1000.0f) / SIM_CELLS;
    float spread = 4.0f + (100.0f - soc) * 0.6f;   /* mV, widens over discharge */
    uint16_t lo = 0xFFFF, hi = 0;
    for (int c = 0; c < SIM_CELLS; c++) {
        float off = ((c % 5) - 2) * (spread / 2.0f);
        if (c == SIM_CELLS - 1) off -= spread;      /* one weak cell */
        uint16_t mv = (uint16_t)(base_mv + off);
        o->cell_mv[c] = mv;
        if (mv < lo) lo = mv;
        if (mv > hi) hi = mv;
    }
    o->cell_min_mv = lo;
    o->cell_max_mv = hi;
    o->imbalance_mv = (uint16_t)(hi - lo);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    static int done_hold;
    if (s_sim.done) {
        if (++done_hold > 24) {     /* short rest, then restart the demo */
            done_hold = 0;
            sim_reset(&s_sim);
        }
    } else {
        sim_step(&s_sim, SIM_DT_S);
    }
    online_sample_t o = {0};
    sim_fill_sample(&s_sim, &o);
    ui_online_update(&o);
}

void app_main(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = false,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    sim_reset(&s_sim);

    bsp_display_lock(0);
    ui_online_create();
    online_sample_t o = {0};
    sim_fill_sample(&s_sim, &o);
    ui_online_update(&o);
    lv_timer_create(tick_cb, 250, NULL);   /* ~250 ms/tick, 30 s sim time each */
    bsp_display_unlock();

    ESP_LOGI(TAG, "online screen up (simulated discharge)");
}

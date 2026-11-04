/*
 * Display bring-up for JC8012P4A1 (ESP32-P4, JD9365 + GSL3680).
 * Initializes the board BSP + LVGL and shows a hello screen with a button,
 * so we can confirm the panel lights up and the touch controller works.
 */
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "blt";

static lv_obj_t *s_touch_label;
static int s_touch_count;

static void btn_event_cb(lv_event_t *e)
{
    (void)e;
    s_touch_count++;
    lv_label_set_text_fmt(s_touch_label, "Touch OK: %d", s_touch_count);
    ESP_LOGI(TAG, "touch %d", s_touch_count);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b1020), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "BLT-ESP-DISPLAY");
    lv_obj_set_style_text_color(title, lv_color_hex(0xf0f3ff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    lv_display_t *disp = lv_display_get_default();
    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text_fmt(sub, "JC8012P4A1  %dx%d  LVGL %d.%d.%d",
                          (int)lv_display_get_horizontal_resolution(disp),
                          (int)lv_display_get_vertical_resolution(disp),
                          lv_version_major(), lv_version_minor(), lv_version_patch());
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9aa4c2), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 72);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 320, 110);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2f6df6), 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "TAP ME");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_24, 0);
    lv_obj_center(bl);

    s_touch_label = lv_label_create(scr);
    lv_label_set_text(s_touch_label, "Touch OK: 0");
    lv_obj_set_style_text_color(s_touch_label, lv_color_hex(0x7CFFB2), 0);
    lv_obj_set_style_text_font(s_touch_label, &lv_font_montserrat_22, 0);
    lv_obj_align(s_touch_label, LV_ALIGN_CENTER, 0, 90);
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

    bsp_display_lock(0);
    build_ui();
    bsp_display_unlock();

    ESP_LOGI(TAG, "display bring-up done");
}

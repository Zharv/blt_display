#include "fonts.h"

#include "esp_log.h"

static const char *TAG = "fonts";

extern const uint8_t ttf_start[] asm("_binary_Montserrat_Medium_ttf_start");
extern const uint8_t ttf_end[]   asm("_binary_Montserrat_Medium_ttf_end");

typedef struct {
    int px;
    const lv_font_t *icons;   /* built-in font that carries LV_SYMBOL_* glyphs */
    lv_font_t *font;
} font_slot_t;

static font_slot_t s_fonts[] = {
    { 16, &lv_font_montserrat_16, NULL },
    { 20, &lv_font_montserrat_20, NULL },
    { 24, &lv_font_montserrat_24, NULL },
    { 32, &lv_font_montserrat_30, NULL },
    { 48, &lv_font_montserrat_48, NULL },
};
#define N_FONTS (sizeof s_fonts / sizeof s_fonts[0])

void fonts_init(void)
{
    size_t len = (size_t)(ttf_end - ttf_start);
    for (size_t i = 0; i < N_FONTS; i++) {
        lv_font_t *f = lv_tiny_ttf_create_data(ttf_start, len, s_fonts[i].px);
        if (!f) {
            ESP_LOGE(TAG, "tiny_ttf %d px failed - using built-in", s_fonts[i].px);
            continue;
        }
        f->fallback = s_fonts[i].icons;
        s_fonts[i].font = f;
    }
    ESP_LOGI(TAG, "Montserrat TTF (%u bytes) at 16/20/24/32/48 px", (unsigned)len);
}

const lv_font_t *font_ua(int px)
{
    size_t best = 0;
    int bestd = 1 << 30;
    for (size_t i = 0; i < N_FONTS; i++) {
        int d = s_fonts[i].px - px;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = i; }
    }
    return s_fonts[best].font ? s_fonts[best].font : s_fonts[best].icons;
}

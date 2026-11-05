/*
 * LVGL heap in PSRAM (LV_USE_CUSTOM_MALLOC).
 * With the C library allocator every small LVGL object landed in internal RAM
 * (SPIRAM_MALLOC_ALWAYSINTERNAL), and the full tabbed UI left too little
 * internal/DMA memory for ESP-Hosted, so Wi-Fi init to the C6 failed.
 */
#include "lvgl.h"
#include "esp_heap_caps.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#define CAPS_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, CAPS_PSRAM);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, CAPS_PSRAM);
    return n ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    multi_heap_info_t i;
    heap_caps_get_info(&i, CAPS_PSRAM);
    mon->total_size = i.total_free_bytes + i.total_allocated_bytes;
    mon->free_size = i.total_free_bytes;
    mon->free_biggest_size = i.largest_free_block;
    mon->used_cnt = i.allocated_blocks;
    mon->free_cnt = i.free_blocks;
    mon->used_pct = mon->total_size ? (uint8_t)(100 - (100ULL * mon->free_size) / mon->total_size) : 0;
    mon->frag_pct = 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

#endif

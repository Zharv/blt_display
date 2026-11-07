/*
 * Background tasks with their stack in PSRAM. Internal RAM is scarce and the
 * ESP-Hosted Wi-Fi/BLE transport needs it for DMA buffers: when it ran short,
 * full-size Wi-Fi frames stopped going out (printing stalled) and the BMS link
 * dropped. Rules for such tasks: never delete themselves (they are long-lived
 * workers) and never touch the internal flash (NVS, partitions) - a flash
 * operation asserts on a PSRAM stack.
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

static inline BaseType_t psram_task(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                    UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    return xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, out, core,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

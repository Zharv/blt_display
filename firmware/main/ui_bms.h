/* BMS screen: scan BLE, pick the battery's BMS, see link status. */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_bms_create(void);
void      ui_bms_set_back_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif

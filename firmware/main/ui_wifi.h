/* Settings tab: Wi-Fi (scan, pick, on-screen keyboard, save to NVS) and the
 * interface language (UA/EN, applied by restarting the UI). */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_wifi_create(void);

#ifdef __cplusplus
}
#endif

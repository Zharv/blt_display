/* Wi-Fi settings screen: scan, pick a network, enter the password on-screen,
 * save to NVS and connect. */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_wifi_create(void);
void      ui_wifi_set_back_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif

/* Printer settings screen: discover IPP printers via mDNS, pick one, save. */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_printer_create(void);
void      ui_printer_set_back_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif

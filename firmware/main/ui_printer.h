/* Printer tab: discover IPP printers via mDNS or enter an IP; saved to NVS. */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_printer_create(void);

#ifdef __cplusplus
}
#endif

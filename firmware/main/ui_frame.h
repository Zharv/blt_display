/*
 * Shared screen frame: status bar (title, test state, Wi-Fi / BT / printer
 * indicators, clock, language) and the bottom tab bar with the print button.
 * Every screen gets its own copy; ui_frame_update() refreshes all of them.
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { TAB_ONLINE = 0, TAB_ARCHIVE, TAB_BMS, TAB_PRINTER, TAB_SETTINGS, TAB__COUNT } tab_t;

typedef enum { IND_OFF = 0, IND_BUSY, IND_OK } ind_t;

typedef struct {
    const char *state_text;   /* "Розряд · 00:58:30" */
    ind_t       state_ind;    /* OK = running (green), BUSY = rest/done (amber), OFF = idle */
    ind_t       wifi;  const char *wifi_text;
    ind_t       bt;    const char *bt_text;
    ind_t       printer;
    const char *clock;        /* "14:32" or "" */
} frame_status_t;

/* Add the frame to scr (call once per screen, display locked). */
void ui_frame_attach(lv_obj_t *scr, tab_t tab);

void ui_frame_set_nav_cb(void (*cb)(tab_t tab));
void ui_frame_set_print_cb(void (*cb)(void));

/* Refresh indicators on every frame (only changed texts are redrawn). */
void ui_frame_update(const frame_status_t *st);

/* Short message above the tab bar for ~4 s (display locked). */
void ui_frame_toast(const char *msg);

#ifdef __cplusplus
}
#endif

/* Shared palette and layout for the 1280x800 UI. */
#pragma once

#include "lvgl.h"

#define C_BG       lv_color_hex(0x0b1020)
#define C_BAR      lv_color_hex(0x0e1428)   /* status + tab bars */
#define C_CARD     lv_color_hex(0x161d33)
#define C_CARD2    lv_color_hex(0x0f1630)   /* inset (cells, segmented control) */
#define C_LINE     lv_color_hex(0x26304f)
#define C_TXT      lv_color_hex(0xf0f3ff)
#define C_SUB      lv_color_hex(0x9aa4c2)
#define C_MUTED    lv_color_hex(0x5b6788)
#define C_ACCENT   lv_color_hex(0x2f6df6)   /* primary action */
#define C_SEL      lv_color_hex(0x1c2a52)   /* selected tab / segment */
#define C_GREEN    lv_color_hex(0x59e6a0)
#define C_BLUE     lv_color_hex(0x4c8dff)
#define C_VIOLET   lv_color_hex(0xc58cff)
#define C_AMBER    lv_color_hex(0xffcc66)
#define C_RED      lv_color_hex(0xff6b6b)
#define C_GRAYLINE lv_color_hex(0x3d4a73)

#define SCR_W      1280
#define SCR_H      800
#define BAR_TOP_H  56
#define BAR_BOT_H  80
#define CONTENT_Y  (BAR_TOP_H + 8)
#define CONTENT_H  (SCR_H - BAR_TOP_H - BAR_BOT_H - 16)
#define PAD        16

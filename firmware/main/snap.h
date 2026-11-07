/*
 * Screenshot over the console UART (development aid).
 * Send "SNAP\n" on the console; the board answers "SNAP-BAUD 921600", switches
 * the UART to that rate, sends "SNAP-BEGIN <w> <h> <len>\n", <len> bytes of
 * RGB565 packets (h&0x80: run of (h&0x7f)+1 copies of the next pixel; else h+1
 * literal pixels; pixels little-endian), "\nSNAP-END\n", then returns to the
 * normal rate. tools/snap.py turns it into a PNG.
 * "TAB n" switches the tab (0..4), "CHART m" the OnLine chart mode.
 * "BLTALLOW 1|0" allows/blocks tester read commands; "BLT RD 000000 0010"
 * sends one read (RI/RD/RB only, <= 64 B) and logs the raw response.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void snap_start(void);

/* Implemented by the app (called from the snap task): show tab n;
 * "CHART m" sets the OnLine chart mode (0 discharge, 1 imbalance). */
void snap_nav(int tab);
void snap_chart(int mode);
/* "BLTONCE": one read of the tester live block (RI 0x00..0xBF), decoded to the
 * log; reads are blocked again afterwards. "BLTPOLL 1|0": continuous polling. */
void snap_blt_once(void);
void blt_live_poll_enable(bool on);
bool blt_live_poll_enabled(void);
/* battery chemistry for the status limits: 0 auto, 1 NMC, 2 LFP (NVS) */
uint8_t health_chem_get(void);
void health_chem_set(uint8_t chem);
/* print the protocol of a recorder cycle (0xFFFF = current test); LVGL context */
bool app_print_protocol(uint16_t cycle);
/* "CELL n": toggle the highlight of cell n (1-based) as a tap would. */
void snap_cell(int n);
void snap_protocol(bool print, uint16_t cycle);   /* PROTO [n] / PROTOPRINT */
/* "BMS": scan and connect to the first JK found; "BMSOFF": disconnect. */
void snap_bms_connect(const char *name);   /* NULL: Daly preferred, else first JK */
void snap_bms_disconnect(void);

#ifdef __cplusplus
}
#endif

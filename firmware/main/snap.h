/*
 * Screenshot over the console UART (development aid).
 * Send "SNAP\n" on the console; the board answers "SNAP-BAUD 921600", switches
 * the UART to that rate, sends "SNAP-BEGIN <w> <h> <len>\n", <len> bytes of
 * RGB565 packets (h&0x80: run of (h&0x7f)+1 copies of the next pixel; else h+1
 * literal pixels; pixels little-endian), "\nSNAP-END\n", then returns to the
 * normal rate. tools/snap.py turns it into a PNG.
 * "TAB n" switches the tab (0..4), "CHART m" the OnLine chart mode.
 * "BLTALLOW 1|0" allows/blocks tester read commands; "BLT RD 000000 0010"
 * sends one read (RI/RD/RW/RB only, <= 64 B) and logs the raw response.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void snap_start(void);

/* Implemented by the app (called from the snap task): show tab n;
 * "CHART m" sets the OnLine chart mode (0 discharge, 1 imbalance). */
void snap_nav(int tab);
void snap_chart(int mode);
/* "CELL n": toggle the highlight of cell n (1-based) as a tap would. */
void snap_cell(int n);
/* "BMS": scan and connect to the first JK found; "BMSOFF": disconnect. */
void snap_bms_connect(void);
void snap_bms_disconnect(void);

#ifdef __cplusplus
}
#endif

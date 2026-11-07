/*
 * Daly BMS over Bluetooth, "D2" protocol (Modbus-RTU style, newer Daly BLE
 * modules, names often "DL-..."). Layout per syssi/esphome-daly-bms
 * (daly_bms_ble): service 0xFFF0, notify 0xFFF1, write 0xFFF2.
 *
 * Request (8 B): D2 <fn> <addr hi> <addr lo> <value hi> <value lo> <crc lo> <crc hi>
 *   fn 0x03 = read <value> registers from <addr>; CRC = CRC-16/MODBUS of bytes 0..5.
 * Response: D2 03 <nbytes> <data...> <crc lo> <crc hi>; registers are big-endian.
 * Status = read 0x3E (62) registers from 0x0000 -> 129-byte frame (165 for 80 regs).
 *
 * Unverified on hardware: the current sign (taken as positive = charging, as
 * ESPHome does) and the balance-bit order of the 80-register frame.
 */
#pragma once

#include "bms.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DALY_D2_START          0xD2u
#define DALY_D2_FN_READ        0x03u
#define DALY_D2_STATUS_REGS    0x3Eu
#define DALY_D2_STATUS_LEN     (3u + 2u * 62u + 2u)   /* 129 */
#define DALY_D2_STATUS80_LEN   (3u + 2u * 80u + 2u)   /* 165 */

typedef enum {
    DALY_D2_OK           = 0,
    DALY_D2_INCOMPLETE   = 1,    /* need more bytes */
    DALY_D2_ERR_FRAMING  = -1,
    DALY_D2_ERR_CRC      = -2,
    DALY_D2_ERR_LEN      = -3,
} daly_d2_err_t;

/* CRC-16/MODBUS (poly 0xA001 reflected, init 0xFFFF). */
uint16_t daly_d2_crc16(const uint8_t *p, size_t n);

/* Build an 8-byte read request. Returns 8, or 0 if cap < 8. */
size_t daly_d2_build_read(uint8_t *out, size_t cap, uint16_t addr, uint16_t count);

/* Length of the complete frame starting at buf (needs >= 3 bytes), 0 if unknown. */
size_t daly_d2_frame_len(const uint8_t *buf, size_t have);

/* Check start/length/CRC of one complete frame. */
daly_d2_err_t daly_d2_check(const uint8_t *f, size_t len);

/* Decode a status response (129 or 165 bytes) into out (cleared first). */
daly_d2_err_t daly_d2_parse_status(const uint8_t *f, size_t len, bms_data_t *out);

#ifdef __cplusplus
}
#endif

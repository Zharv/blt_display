/*
 * Daly BMS protocol (the BLE service carries the same 13-byte UART frames).
 *
 * Frame (13 bytes): A5 <addr> <cmd> 08 <D0..D7> <chk>
 *   addr: 0x40 host->BMS, 0x01 BMS->host
 *   chk : 8-bit sum of bytes 0..11
 *   multi-byte values inside D0..D7 are big-endian.
 *
 * Reads needing more than 8 data bytes (cell voltages, temps) arrive as several
 * frames with a 1-based frame index in D0; feed each to daly_parse with the same
 * bms_data_t to accumulate.
 */
#pragma once

#include "bms.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DALY_START        0xA5u
#define DALY_ADDR_HOST    0x40u
#define DALY_ADDR_BMS     0x01u
#define DALY_DATA_LEN     0x08u
#define DALY_FRAME_LEN    13u

#define DALY_CMD_SOC      0x90u  /* pack V / current / SOC          */
#define DALY_CMD_MINMAXV  0x91u  /* max/min cell voltage            */
#define DALY_CMD_MINMAXT  0x92u  /* max/min temperature             */
#define DALY_CMD_MOS      0x93u  /* charge/discharge state + remain */
#define DALY_CMD_STATUS   0x94u  /* cell/temp counts                */
#define DALY_CMD_CELLS    0x95u  /* per-cell voltages (multi-frame) */
#define DALY_CMD_TEMPS    0x96u  /* per-sensor temps (multi-frame)  */

typedef enum {
    DALY_OK           = 0,
    DALY_ERR_FRAMING  = -1,
    DALY_ERR_CHECKSUM = -2,
} daly_err_t;

/* 8-bit sum of `len` bytes. */
uint8_t daly_checksum(const uint8_t *bytes, size_t len);

/* Build a 13-byte read request (data bytes zero). Returns 13 or 0. */
size_t daly_build_read(uint8_t *out, size_t cap, uint8_t cmd);

/* Decode one 13-byte frame into out (fields depend on cmd; out not cleared). */
daly_err_t daly_parse(const uint8_t *frame, size_t len, bms_data_t *out);

#ifdef __cplusplus
}
#endif

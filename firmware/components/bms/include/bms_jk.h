/*
 * JK (Jikong) BMS "new" BLE protocol (frames starting 0x55 0xAA 0xEB 0x90).
 *
 * Notification frames are 300 bytes: [0..3]=55 AA EB 90, [4]=type, [5]=counter,
 * payload, [299]=checksum (8-bit sum of bytes 0..298).
 * Payload values are little-endian. This driver decodes the cell-info frame
 * (type 0x02). Field offsets follow esphome-jk-bms; the 32S variant shifts the
 * later fields by +32 and the resistance block by +16. Confirm against a capture.
 *
 * GATT: service 0xFFE0, characteristic 0xFFE1. Command frame (to BMS) is 20 bytes
 * starting 0xAA 0x55 0x90 0xEB; request cell info with command 0x96.
 */
#pragma once

#include "bms.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JK_FRAME_LEN      300u
#define JK_HDR0           0x55u
#define JK_HDR1           0xAAu
#define JK_HDR2           0xEBu
#define JK_HDR3           0x90u

#define JK_TYPE_SETTINGS  0x01u
#define JK_TYPE_CELLINFO  0x02u
#define JK_TYPE_DEVINFO   0x03u

#define JK_CMD_CELLINFO   0x96u
#define JK_CMD_DEVINFO    0x97u

typedef enum {
    JK_VARIANT_24S = 0,
    JK_VARIANT_32S = 1,
} jk_variant_t;

typedef enum {
    JK_OK            = 0,
    JK_ERR_FRAMING   = -1,
    JK_ERR_CHECKSUM  = -2,
    JK_ERR_TYPE      = -3,  /* not a cell-info frame */
} jk_err_t;

/* 8-bit sum of `len` bytes (the JK frame checksum). */
uint8_t jk_checksum(const uint8_t *bytes, size_t len);

/* Build a 20-byte command frame (value little-endian, vlen bytes). Returns 20 or 0. */
size_t jk_build_command(uint8_t *out, size_t cap, uint8_t cmd, uint32_t value, uint8_t vlen);

/* Decode a cell-info (0x02) frame into out (out is cleared first). */
jk_err_t jk_parse_cellinfo(const uint8_t *frame, size_t len, jk_variant_t variant,
                           bms_data_t *out);

#ifdef __cplusplus
}
#endif

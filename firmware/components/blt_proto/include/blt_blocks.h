/*
 * BLT160 EEPROM blocks written before a test (docs/BLT_REGISTERS.md section 4,
 * docs/BLT_FIRMWARE_COMMANDS.md). Every block starts with 0xA5 and ends with
 * the XOR of all bytes before it. Pure C, host-testable.
 *
 *   ID block   RD/WD 0x40,  50 B: [0]=A5, +0x01 battery ID (16 ASCII),
 *                                  +0x11 text (32 ASCII), +0x31 XOR
 *   CFG block  RD/WD 0x100, 62 B: [0]=A5, +0x03 u16 LE rated capacity (Ah),
 *                                  ..., +0x3D XOR
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLT_BLK_MARK        0xA5u
#define BLT_ID_ADDR         0x40u
#define BLT_ID_LEN          50u
#define BLT_ID_SERIAL_OFF   0x01u
#define BLT_ID_SERIAL_LEN   16u
#define BLT_CFG_ADDR        0x100u
#define BLT_CFG_LEN         62u
#define BLT_CFG_RATED_AH    0x03u

/* [0] == 0xA5 and the last byte == XOR of all bytes before it */
bool blt_block_valid(const uint8_t *b, size_t len);

/* set [0] = 0xA5 and recompute the trailing XOR */
void blt_block_seal(uint8_t *b, size_t len);

/* battery ID (serial) from an ID block, trailing spaces trimmed */
void blt_id_get_serial(const uint8_t *id_blk, char *out, size_t n);

/* Put serial (1..16 printable ASCII) into the ID block, space-padded, and
 * re-seal it. Leaves the 32-char text field untouched. */
bool blt_id_set_serial(uint8_t *id_blk, const char *serial);

uint16_t blt_cfg_get_rated_ah(const uint8_t *cfg);
/* set rated capacity (1..9999 Ah) and re-seal; nothing else is changed */
bool blt_cfg_set_rated_ah(uint8_t *cfg, uint16_t ah);

/* Next serial of the form YYYYMMDDNNNN for the given date: if last has the
 * same date prefix its number + 1, else 0001. out needs 13 bytes. */
void blt_next_serial(char *out, size_t n, int year, int month, int day, const char *last);

#ifdef __cplusplus
}
#endif

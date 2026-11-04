/*
 * JBD / Xiaoxiang BMS protocol (UART-TTL and BLE carry the same frames).
 *
 * Request : DD A5 <reg> 00 <chkH> <chkL> 77
 * Response : DD <reg> <status> <len> <data...> <chkH> <chkL> 77
 *   checksum is 16-bit, big-endian, = 0x10000 - sum(<bytes>)   (see jbd_checksum)
 *   values inside payloads are big-endian.
 *
 * The request-side checksum is anchored to the well-known read-basic-info frame
 * `DD A5 03 00 FF FD 77`. The response field offsets below follow the public JBD
 * spec; confirm against a real device capture before trusting edge fields.
 */
#pragma once

#include "bms.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JBD_START       0xDDu
#define JBD_READ        0xA5u
#define JBD_END         0x77u

#define JBD_REG_BASIC   0x03u   /* pack voltage/current/soc/temps/balance */
#define JBD_REG_CELLS   0x04u   /* per-cell voltages                      */
#define JBD_REG_NAME    0x05u   /* device name (string)                   */

typedef enum {
    JBD_OK            = 0,
    JBD_ERR_FRAMING   = -1,  /* start/end byte or length wrong */
    JBD_ERR_CHECKSUM  = -2,
    JBD_ERR_STATUS    = -3,  /* device returned a non-zero status */
    JBD_ERR_SHORT     = -4,
    JBD_ERR_UNSUPPORTED = -5,
} jbd_err_t;

/* 16-bit checksum over `len` bytes = (0x10000 - sum) & 0xFFFF. */
uint16_t jbd_checksum(const uint8_t *bytes, size_t len);

/* Build a register-read request. Returns frame length (7) or 0 if cap too small. */
size_t jbd_build_read(uint8_t *out, size_t cap, uint8_t reg);

/*
 * Decode one complete response frame into `out`. Updates only the field group
 * carried by this frame's register (so 0x03 then 0x04 accumulate into one
 * snapshot); call bms_recalc_cells() is done internally for 0x04.
 * `out` is not cleared — memset it before the first frame of a snapshot.
 */
jbd_err_t jbd_parse(const uint8_t *frame, size_t len, bms_data_t *out);

#ifdef __cplusplus
}
#endif

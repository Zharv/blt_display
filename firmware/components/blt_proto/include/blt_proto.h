/*
 * Alfa Progetti BLT160 / SmartIC battery-tester serial protocol.
 * See docs/BLT_PROTOCOL.md. Reversed from BltView.exe 2.6.16.
 *
 * Plain C, no ESP-IDF dependencies, so it can be unit-tested on a PC.
 *
 * Request  (host  -> device): '@' NN CC AAAAAA LLLL [DD...] XX '\r'
 * Response (device -> host) : '!' NN CC <8 hex> [DD...] XX '\r'
 *   - all multi-byte fields are UPPERCASE hex ASCII, zero-padded
 *     (Delphi Format %x is uppercase; the builder pads spaces with '0')
 *   - NN = node address (2 hex), default 0x63
 *   - CC = 2-letter command
 *   - AAAAAA = address/parameter (6 hex)   [request only in this fixed width]
 *   - LLLL = payload length N in bytes (4 hex)   [request]
 *   - XX = XOR of every ASCII char from the start char up to (not incl.) XX
 *   - response payload starts at byte offset 13; framelen = 2*N + 15 (excl. CR)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLT_NODE_DEFAULT     0x63u
#define BLT_START_REQ        '@'
#define BLT_START_RESP       '!'
#define BLT_EOF              '\r'

#define BLT_REQ_HEADER_LEN   15u   /* '@' + NN(2) + CC(2) + AAAAAA(6) + LLLL(4) */
#define BLT_RESP_HEADER_LEN  13u   /* response payload starts here */
#define BLT_RESP_OVERHEAD    15u   /* header(13) + checksum(2); framelen = 2*N + 15 */

#define BLT_MAX_PAYLOAD      8192u
/* request worst case: header + 2*N payload hex + checksum(2) + CR */
#define BLT_MAX_FRAME        (BLT_REQ_HEADER_LEN + 2u * BLT_MAX_PAYLOAD + 3u)

/* Command index == order in BltView's command table (FUN_005d7e8c switch). */
typedef enum {
    BLT_CMD_RI = 0,   /* Read Info / identify            */
    BLT_CMD_RD = 1,   /* Read Data (RAM/registers)       */
    BLT_CMD_RE = 2,   /* Read EEPROM (addr==10 -> string)*/
    BLT_CMD_WD = 3,   /* Write Data                      */
    BLT_CMD_WC = 4,   /* Write Config / clock            */
    BLT_CMD_CI = 5,   /* command, response NOT validated */
    BLT_CMD_CP = 6,   /* command, response NOT validated */
    BLT_CMD_RW = 7,   /* Read Work / recorder            */
    BLT_CMD_GG = 8,   /* special, 3 inline payload bytes */
    BLT_CMD_RB = 9,   /* Read Block                      */
    BLT_CMD__COUNT
} blt_cmd_t;

/* RX/TX error codes, 1-based, matching BltView's string table (0 == OK). */
typedef enum {
    BLT_OK               = 0,
    BLT_ERR_PORT_NOT_OPEN = 1,
    BLT_ERR_CREATE_HANDLE = 2,
    BLT_ERR_GETDCB        = 3,
    BLT_ERR_SETDCB        = 4,
    BLT_ERR_SPEED         = 5,
    BLT_ERR_PARITY        = 6,
    BLT_ERR_DATABITS      = 7,
    BLT_ERR_STOPBITS      = 8,
    BLT_ERR_TX            = 9,
    BLT_ERR_RX_TIMEOUT    = 10,
    BLT_ERR_RX_CHAR       = 11,  /* non-hex char in response          */
    BLT_ERR_RX_FRAMELEN   = 12,  /* length != 2*N + 15                */
    BLT_ERR_RX_STX        = 13,  /* first char not '!'                */
    BLT_ERR_RX_NODE       = 14,  /* node echo mismatch                */
    BLT_ERR_RX_CMD        = 15,  /* command echo mismatch             */
    BLT_ERR_RX_CHECKSUM   = 16,  /* XOR checksum mismatch             */
    BLT_ERR_RX_NAK        = 17,  /* node echo == "EE" (device NAK)    */
    BLT_ERR_UNDEFINED     = 18
} blt_err_t;

/* "RI".."RB", or "??" for an out-of-range value. */
const char *blt_cmd_str(blt_cmd_t cmd);
/* Parse a 2-letter command; returns true and sets *out on success. */
bool        blt_cmd_from_str(const char *s, blt_cmd_t *out);
/* Human-readable error name. */
const char *blt_err_str(blt_err_t err);

/* XOR of all bytes, returned as a single byte (the protocol checksum). */
uint8_t blt_checksum(const uint8_t *data, size_t len);

/*
 * Build a request frame into out (NUL is NOT appended; the frame ends with CR).
 * node    : device address (BLT_NODE_DEFAULT for the single-unit case)
 * cmd     : command
 * addr    : 24-bit address/parameter (upper bits ignored)
 * data    : payload bytes, or NULL for a pure read
 * nbytes  : value written to LLLL. For a read this is how many bytes to request
 *           (data may be NULL); for a write it must equal the real payload size.
 * Returns the frame length, or 0 if it does not fit in cap.
 *
 * Special case faithful to the original builder: cmd==RE && addr==10 appends
 * `data` as raw ASCII (not hex-encoded).
 */
size_t blt_build_frame(uint8_t *out, size_t cap, uint8_t node, blt_cmd_t cmd,
                        uint32_t addr, const uint8_t *data, uint16_t nbytes);

/*
 * GG command. BltView patches 3 payload bytes at a fixed buffer offset (0x0d).
 * Reconstructed but NOT verified against a device; kept for completeness.
 */
size_t blt_build_gg(uint8_t *out, size_t cap, uint8_t node, uint32_t addr,
                    uint8_t b0, uint8_t b1, uint8_t b2);

typedef struct {
    uint8_t  node_echo;                 /* decoded from frame[1:3]              */
    bool     is_nak;                    /* frame[1:3] == "EE"                   */
    char     cmd_echo[3];               /* frame[3:5] + NUL                     */
    uint8_t  hdr_extra[4];              /* frame[5:13] (8 hex) = addr/len/status
                                           echo; exact semantics not verified   */
    uint8_t  payload[BLT_MAX_PAYLOAD];  /* decoded payload (N bytes)            */
    uint16_t payload_len;               /* N                                    */
    bool     checksum_ok;               /* set even when other checks pass      */
} blt_response_t;

/*
 * Validate and decode a complete response frame (trailing CR/LF optional).
 * sent_cmd / sent_node are the request's, used for the echo checks.
 * Returns BLT_OK on a fully valid frame, otherwise the first failing check.
 * On BLT_ERR_RX_NAK, out->is_nak is set. Fields decoded before the failing
 * check are still populated where meaningful.
 */
blt_err_t blt_parse_response(const uint8_t *frame, size_t len,
                             blt_cmd_t sent_cmd, uint8_t sent_node,
                             blt_response_t *out);

#ifdef __cplusplus
}
#endif

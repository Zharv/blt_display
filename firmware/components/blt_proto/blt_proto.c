#include "blt_proto.h"

#include <string.h>

static const char *const k_cmd_str[BLT_CMD__COUNT] = {
    "RI", "RD", "RE", "WD", "WC", "CI", "CP", "RW", "GG", "RB"
};

const char *blt_cmd_str(blt_cmd_t cmd)
{
    if ((unsigned)cmd < (unsigned)BLT_CMD__COUNT) {
        return k_cmd_str[cmd];
    }
    return "??";
}

bool blt_cmd_from_str(const char *s, blt_cmd_t *out)
{
    if (s == NULL) {
        return false;
    }
    for (unsigned i = 0; i < (unsigned)BLT_CMD__COUNT; i++) {
        if (s[0] == k_cmd_str[i][0] && s[1] == k_cmd_str[i][1]) {
            if (out) {
                *out = (blt_cmd_t)i;
            }
            return true;
        }
    }
    return false;
}

const char *blt_err_str(blt_err_t err)
{
    switch (err) {
    case BLT_OK:                return "OK";
    case BLT_ERR_PORT_NOT_OPEN: return "PORT NOT OPENED";
    case BLT_ERR_CREATE_HANDLE: return "CREATE HANDLE";
    case BLT_ERR_GETDCB:        return "GETDCB";
    case BLT_ERR_SETDCB:        return "SETDCB";
    case BLT_ERR_SPEED:         return "SPEED";
    case BLT_ERR_PARITY:        return "PARITY";
    case BLT_ERR_DATABITS:      return "DATABITS";
    case BLT_ERR_STOPBITS:      return "STOPBITS";
    case BLT_ERR_TX:            return "TX ERROR";
    case BLT_ERR_RX_TIMEOUT:    return "RX TIMEOUT";
    case BLT_ERR_RX_CHAR:       return "RX CHAR";
    case BLT_ERR_RX_FRAMELEN:   return "RX FRAMELEN";
    case BLT_ERR_RX_STX:        return "RX STX";
    case BLT_ERR_RX_NODE:       return "RX NODE";
    case BLT_ERR_RX_CMD:        return "RX CMD";
    case BLT_ERR_RX_CHECKSUM:   return "RX CHECKSUM";
    case BLT_ERR_RX_NAK:        return "RX NAK";
    case BLT_ERR_UNDEFINED:     return "UNDEFINED";
    }
    return "UNDEFINED";
}

uint8_t blt_checksum(const uint8_t *data, size_t len)
{
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++) {
        x ^= data[i];
    }
    return x;
}

static char hex_upper(uint8_t nib)
{
    nib &= 0x0Fu;
    return (char)(nib < 10 ? ('0' + nib) : ('A' + (nib - 10)));
}

/* Write `digits` uppercase hex chars of val (zero-padded) at p. */
static void put_hex(uint8_t *p, uint32_t val, int digits)
{
    for (int i = digits - 1; i >= 0; i--) {
        p[i] = (uint8_t)hex_upper((uint8_t)(val & 0x0Fu));
        val >>= 4;
    }
}

static int hex_val(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int hex2byte(const uint8_t *p, uint8_t *out)
{
    int hi = hex_val(p[0]);
    int lo = hex_val(p[1]);
    if (hi < 0 || lo < 0) {
        return -1;
    }
    *out = (uint8_t)((hi << 4) | lo);
    return 0;
}

size_t blt_build_frame(uint8_t *out, size_t cap, uint8_t node, blt_cmd_t cmd,
                       uint32_t addr, const uint8_t *data, uint16_t nbytes)
{
    if (out == NULL || (unsigned)cmd >= (unsigned)BLT_CMD__COUNT) {
        return 0;
    }

    bool raw = (cmd == BLT_CMD_RE && addr == 10);
    /* payload bytes appear in the frame only on a write (data != NULL); on a
       read the byte count lives in LLLL and no payload follows. */
    bool emit = (data != NULL && nbytes > 0);
    size_t payload_chars = emit ? (raw ? (size_t)nbytes : (size_t)nbytes * 2u) : 0u;
    size_t need = BLT_REQ_HEADER_LEN + payload_chars + 2u /*XX*/ + 1u /*CR*/;
    if (need > cap) {
        return 0;
    }

    const char *cc = k_cmd_str[cmd];
    size_t i = 0;

    out[i++] = (uint8_t)BLT_START_REQ;
    put_hex(&out[i], node, 2);        i += 2;
    out[i++] = (uint8_t)cc[0];
    out[i++] = (uint8_t)cc[1];
    put_hex(&out[i], addr & 0xFFFFFFu, 6); i += 6;
    put_hex(&out[i], nbytes, 4);      i += 4;

    if (emit) {
        if (raw) {
            memcpy(&out[i], data, nbytes);
            i += nbytes;
        } else {
            for (uint16_t b = 0; b < nbytes; b++) {
                put_hex(&out[i], data[b], 2);
                i += 2;
            }
        }
    }
    /* else: read request — LLLL carries the byte count, no payload follows */

    uint8_t x = blt_checksum(out, i);
    put_hex(&out[i], x, 2);           i += 2;
    out[i++] = (uint8_t)BLT_EOF;
    return i;
}

size_t blt_build_gg(uint8_t *out, size_t cap, uint8_t node, uint32_t addr,
                    uint8_t b0, uint8_t b1, uint8_t b2)
{
    uint8_t data[3] = { b0, b1, b2 };
    return blt_build_frame(out, cap, node, BLT_CMD_GG, addr, data, 3);
}

blt_err_t blt_parse_response(const uint8_t *frame, size_t len,
                             blt_cmd_t sent_cmd, uint8_t sent_node,
                             blt_response_t *out)
{
    if (out == NULL || frame == NULL) {
        return BLT_ERR_UNDEFINED;
    }
    memset(out, 0, sizeof(*out));

    /* strip trailing CR/LF */
    while (len > 0 && (frame[len - 1] == '\r' || frame[len - 1] == '\n')) {
        len--;
    }

    if (len < BLT_RESP_OVERHEAD || ((len - BLT_RESP_OVERHEAD) % 2u) != 0u) {
        return BLT_ERR_RX_FRAMELEN;
    }
    if (frame[0] != (uint8_t)BLT_START_RESP) {
        return BLT_ERR_RX_STX;
    }

    /* node echo / NAK */
    if (frame[1] == 'E' && frame[2] == 'E') {
        out->is_nak = true;
        return BLT_ERR_RX_NAK;
    }
    uint8_t node_echo;
    if (hex2byte(&frame[1], &node_echo) != 0) {
        return BLT_ERR_RX_CHAR;
    }
    out->node_echo = node_echo;
    if (node_echo != sent_node) {
        return BLT_ERR_RX_NODE;
    }

    /* command echo */
    out->cmd_echo[0] = (char)frame[3];
    out->cmd_echo[1] = (char)frame[4];
    out->cmd_echo[2] = '\0';
    const char *cc = blt_cmd_str(sent_cmd);
    if (frame[3] != (uint8_t)cc[0] || frame[4] != (uint8_t)cc[1]) {
        return BLT_ERR_RX_CMD;
    }

    /* hex validity from index 5 to end */
    for (size_t k = 5; k < len; k++) {
        if (hex_val(frame[k]) < 0) {
            return BLT_ERR_RX_CHAR;
        }
    }

    /* checksum: XOR over everything except the trailing 2 hex chars */
    uint8_t want;
    (void)hex2byte(&frame[len - 2], &want);
    uint8_t got = blt_checksum(frame, len - 2);
    out->checksum_ok = (got == want);

    /* header extra (4 bytes) from frame[5..12] */
    for (int b = 0; b < 4; b++) {
        (void)hex2byte(&frame[5 + b * 2], &out->hdr_extra[b]);
    }

    /* payload from offset 13 */
    size_t n = (len - BLT_RESP_OVERHEAD) / 2u;
    if (n > BLT_MAX_PAYLOAD) {
        return BLT_ERR_RX_FRAMELEN;
    }
    for (size_t b = 0; b < n; b++) {
        (void)hex2byte(&frame[BLT_RESP_HEADER_LEN + b * 2], &out->payload[b]);
    }
    out->payload_len = (uint16_t)n;

    if (!out->checksum_ok) {
        return BLT_ERR_RX_CHECKSUM;
    }
    return BLT_OK;
}

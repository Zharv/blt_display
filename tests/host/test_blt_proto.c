/*
 * Host unit test for firmware/components/blt_proto.
 *   - build cases from tests/gen_frames.py and compare against the Python oracle
 *   - exercise blt_parse_response with valid / NAK / mismatch / bad-checksum frames
 * Exit code 0 on success, 1 on any failure.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "blt_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);       \
               printf(__VA_ARGS__); printf("\n"); }                        \
    } while (0)

static int hexnib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static size_t parse_hex_bytes(const char *s, uint8_t *out, size_t cap)
{
    if (s[0] == '-' && s[1] == '\0') return 0;
    size_t n = 0;
    for (size_t i = 0; s[i] && s[i + 1] && n < cap; i += 2) {
        int hi = hexnib((unsigned char)s[i]);
        int lo = hexnib((unsigned char)s[i + 1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

static void run_build_cases(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) { printf("FAIL: cannot open %s\n", path); g_fail++; return; }

    char line[1024];
    int count = 0;
    while (fgets(line, sizeof line, fp)) {
        char cmd[8], datahex[600], expected[700];
        unsigned node, addr, nbytes;
        if (sscanf(line, "%7s %x %x %x %599s %699s",
                   cmd, &node, &addr, &nbytes, datahex, expected) != 6) {
            continue;
        }
        blt_cmd_t c;
        CHECK(blt_cmd_from_str(cmd, &c), "unknown cmd %s", cmd);

        uint8_t data[300];
        size_t dn = parse_hex_bytes(datahex, data, sizeof data);

        uint8_t out[BLT_MAX_FRAME];
        size_t n = blt_build_frame(out, sizeof out, (uint8_t)node, c, addr,
                                   dn ? data : NULL, (uint16_t)nbytes);
        CHECK(n > 0, "build returned 0 for %s", cmd);

        /* strip CR for comparison with expected (which has none) */
        if (n > 0 && out[n - 1] == '\r') n--;
        out[n] = '\0';
        CHECK(strcmp((char *)out, expected) == 0,
              "%s: built \"%s\" expected \"%s\"", cmd, (char *)out, expected);
        count++;
    }
    fclose(fp);
    printf("built %d frames, matched Python oracle\n", count);
}

/* Build a valid response frame for testing the parser. */
static size_t make_response(uint8_t *buf, uint8_t node, const char *cmd,
                            const uint8_t *payload, size_t n)
{
    size_t i = 0;
    i += (size_t)sprintf((char *)buf + i, "!%02X%s", node, cmd); /* '!' NN CC */
    /* 8-char header-extra block (addr/len echo) — fill with zeros */
    i += (size_t)sprintf((char *)buf + i, "%08X", 0u);
    for (size_t b = 0; b < n; b++) {
        i += (size_t)sprintf((char *)buf + i, "%02X", payload[b]);
    }
    uint8_t x = blt_checksum(buf, i);
    i += (size_t)sprintf((char *)buf + i, "%02X", x);
    buf[i++] = '\r';
    return i;
}

static void run_parse_cases(void)
{
    uint8_t buf[256];
    blt_response_t r;

    const uint8_t pay[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    size_t n = make_response(buf, 0x63, "RD", pay, 4);
    blt_err_t e = blt_parse_response(buf, n, BLT_CMD_RD, 0x63, &r);
    CHECK(e == BLT_OK, "valid RD resp -> %s", blt_err_str(e));
    CHECK(r.checksum_ok, "valid RD checksum");
    CHECK(r.payload_len == 4, "payload_len %u", r.payload_len);
    CHECK(memcmp(r.payload, pay, 4) == 0, "payload mismatch");
    CHECK(!r.is_nak, "should not be NAK");

    /* NAK: node echo "EE" */
    const char *nak = "!EERD0000000000\r";
    e = blt_parse_response((const uint8_t *)nak, strlen(nak), BLT_CMD_RD, 0x63, &r);
    CHECK(e == BLT_ERR_RX_NAK && r.is_nak, "NAK detection -> %s", blt_err_str(e));

    /* command echo mismatch */
    n = make_response(buf, 0x63, "RE", pay, 4);
    e = blt_parse_response(buf, n, BLT_CMD_RD, 0x63, &r);
    CHECK(e == BLT_ERR_RX_CMD, "cmd mismatch -> %s", blt_err_str(e));

    /* node echo mismatch */
    n = make_response(buf, 0x10, "RD", pay, 4);
    e = blt_parse_response(buf, n, BLT_CMD_RD, 0x63, &r);
    CHECK(e == BLT_ERR_RX_NODE, "node mismatch -> %s", blt_err_str(e));

    /* bad checksum: flip last-but-two char */
    n = make_response(buf, 0x63, "RD", pay, 4);
    buf[n - 2] = (buf[n - 2] == '0') ? '1' : '0';
    e = blt_parse_response(buf, n, BLT_CMD_RD, 0x63, &r);
    CHECK(e == BLT_ERR_RX_CHECKSUM && !r.checksum_ok, "bad checksum -> %s", blt_err_str(e));

    /* non-hex char in body */
    n = make_response(buf, 0x63, "RD", pay, 4);
    buf[10] = 'Z';
    e = blt_parse_response(buf, n, BLT_CMD_RD, 0x63, &r);
    CHECK(e == BLT_ERR_RX_CHAR, "non-hex char -> %s", blt_err_str(e));

    /* wrong STX */
    n = make_response(buf, 0x63, "RD", pay, 4);
    buf[0] = '?';
    e = blt_parse_response(buf, n, BLT_CMD_RD, 0x63, &r);
    CHECK(e == BLT_ERR_RX_STX, "bad STX -> %s", blt_err_str(e));

    printf("parser cases done\n");
}

int main(int argc, char **argv)
{
    /* sanity: the two hand-verified literals */
    uint8_t out[BLT_MAX_FRAME];
    size_t n = blt_build_frame(out, sizeof out, 0x63, BLT_CMD_RD, 0x000001, NULL, 0x10);
    out[n - 1] = '\0';
    CHECK(strcmp((char *)out, "@63RD000001001053") == 0, "RD literal: %s", (char *)out);

    const uint8_t wd[6] = { 0x12, 0x34, 0x09, 0x11, 0x26, 0x00 };
    n = blt_build_frame(out, sizeof out, 0x63, BLT_CMD_WD, 0x000000, wd, 6);
    out[n - 1] = '\0';
    CHECK(strcmp((char *)out, "@63WD000000000612340911260059") == 0, "WD literal: %s", (char *)out);

    if (argc > 1) {
        run_build_cases(argv[1]);
    }
    run_parse_cases();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

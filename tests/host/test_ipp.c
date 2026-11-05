/* Host test for the IPP request builder. */
#define _CRT_SECURE_NO_WARNINGS
#include "ipp.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int contains(const uint8_t *h, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    if (m > n) return 0;
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(h + i, needle, m) == 0) return 1;
    return 0;
}

int main(void)
{
    uint8_t buf[512];
    const char *uri = "ipp://192.168.1.50:631/ipp/print";
    size_t n = ipp_build_print_job(buf, sizeof buf, 1, uri, "roman", "image/urf", "blt-report");
    CHECK(n > 0 && n <= sizeof buf, "size %zu", n);

    CHECK(buf[0] == 2 && buf[1] == 0, "version 2.0");
    CHECK((buf[2] << 8 | buf[3]) == IPP_OP_PRINT_JOB, "op print-job");
    CHECK((uint32_t)(buf[4] << 24 | buf[5] << 16 | buf[6] << 8 | buf[7]) == 1, "request-id");
    CHECK(buf[8] == IPP_TAG_OPERATION, "operation-attributes tag");
    CHECK(buf[n - 1] == IPP_TAG_END, "end-of-attributes tag");

    CHECK(contains(buf, n, "attributes-charset"), "charset attr");
    CHECK(contains(buf, n, "printer-uri"), "printer-uri attr");
    CHECK(contains(buf, n, uri), "uri value");
    CHECK(contains(buf, n, "requesting-user-name"), "user attr");
    CHECK(contains(buf, n, "roman"), "user value");
    CHECK(contains(buf, n, "document-format"), "doc-format attr");
    CHECK(contains(buf, n, "image/urf"), "doc-format value");
    CHECK(contains(buf, n, "blt-report"), "job-name value");

    /* two-pass size query */
    size_t need = ipp_build_print_job(NULL, 0, 1, uri, "roman", "image/urf", "blt-report");
    CHECK(need == n, "size query %zu vs %zu", need, n);

    /* get-printer-attributes */
    size_t gn = ipp_build_get_printer_attributes(buf, sizeof buf, 2, uri, "roman");
    CHECK(gn > 0 && (buf[2] << 8 | buf[3]) == IPP_OP_GET_PRINTER_ATTRIBUTES, "get-attrs op");
    CHECK(buf[gn - 1] == IPP_TAG_END, "get-attrs end");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

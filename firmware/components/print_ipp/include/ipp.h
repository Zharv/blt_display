/*
 * Minimal IPP (RFC 8010) request builder for submitting a print job.
 * Pure C: builds the IPP message bytes; the firmware layer POSTs them (plus the
 * document data) to http://<printer>:631/ipp/print. Host-testable.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IPP operation ids */
#define IPP_OP_PRINT_JOB              0x0002
#define IPP_OP_GET_PRINTER_ATTRIBUTES 0x000B

/* value tags */
#define IPP_TAG_OPERATION   0x01
#define IPP_TAG_END         0x03
#define IPP_TAG_INTEGER     0x21
#define IPP_TAG_ENUM        0x23
#define IPP_TAG_KEYWORD     0x44
#define IPP_TAG_URI         0x45
#define IPP_TAG_CHARSET     0x47
#define IPP_TAG_NATLANG     0x48
#define IPP_TAG_MIMETYPE    0x49
#define IPP_TAG_NAME        0x42

/*
 * Build a Print-Job request header into out (document data is appended by the
 * caller after this). If out is NULL, returns the size needed; otherwise writes
 * at most cap bytes and returns the total (nothing written if cap < total).
 *   printer_uri : "ipp://192.168.1.50:631/ipp/print"
 *   user        : requesting-user-name
 *   doc_format  : e.g. "image/urf"
 *   job_name    : job name (may be NULL)
 */
size_t ipp_build_print_job(uint8_t *out, size_t cap, uint32_t request_id,
                           const char *printer_uri, const char *user,
                           const char *doc_format, const char *job_name);

/* Build a Get-Printer-Attributes request (to discover supported formats). */
size_t ipp_build_get_printer_attributes(uint8_t *out, size_t cap, uint32_t request_id,
                                        const char *printer_uri, const char *user);

#ifdef __cplusplus
}
#endif

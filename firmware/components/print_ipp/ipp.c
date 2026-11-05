#include "ipp.h"

#include <string.h>

typedef struct { uint8_t *out; size_t cap; size_t n; } sink_t;

static void put(sink_t *s, uint8_t b)
{
    if (s->out && s->n < s->cap) s->out[s->n] = b;
    s->n++;
}
static void put16(sink_t *s, uint16_t v) { put(s, (uint8_t)(v >> 8)); put(s, (uint8_t)v); }
static void put32(sink_t *s, uint32_t v)
{
    put(s, (uint8_t)(v >> 24)); put(s, (uint8_t)(v >> 16));
    put(s, (uint8_t)(v >> 8));  put(s, (uint8_t)v);
}
static void put_str(sink_t *s, const char *p)
{
    for (; p && *p; p++) put(s, (uint8_t)*p);
}

/* one attribute: tag, name-length, name, value-length, value (text) */
static void attr(sink_t *s, uint8_t tag, const char *name, const char *value)
{
    put(s, tag);
    put16(s, (uint16_t)strlen(name));
    put_str(s, name);
    put16(s, (uint16_t)strlen(value));
    put_str(s, value);
}

static size_t header(sink_t *s, uint16_t op, uint32_t rid,
                     const char *uri, const char *user)
{
    /* version 2.0, operation, request-id */
    put(s, 2); put(s, 0);
    put16(s, op);
    put32(s, rid);
    /* operation-attributes group (order matters in IPP) */
    put(s, IPP_TAG_OPERATION);
    attr(s, IPP_TAG_CHARSET, "attributes-charset", "utf-8");
    attr(s, IPP_TAG_NATLANG, "attributes-natural-language", "en");
    attr(s, IPP_TAG_URI,     "printer-uri", uri);
    attr(s, IPP_TAG_NAME,    "requesting-user-name", user ? user : "blt");
    return s->n;
}

size_t ipp_build_print_job(uint8_t *out, size_t cap, uint32_t request_id,
                           const char *printer_uri, const char *user,
                           const char *doc_format, const char *job_name)
{
    sink_t s = { out, cap, 0 };
    header(&s, IPP_OP_PRINT_JOB, request_id, printer_uri, user);
    if (job_name) attr(&s, IPP_TAG_NAME, "job-name", job_name);
    if (doc_format) attr(&s, IPP_TAG_MIMETYPE, "document-format", doc_format);
    put(&s, IPP_TAG_END);
    return s.n;
}

size_t ipp_build_get_printer_attributes(uint8_t *out, size_t cap, uint32_t request_id,
                                        const char *printer_uri, const char *user)
{
    sink_t s = { out, cap, 0 };
    header(&s, IPP_OP_GET_PRINTER_ATTRIBUTES, request_id, printer_uri, user);
    put(&s, IPP_TAG_END);
    return s.n;
}

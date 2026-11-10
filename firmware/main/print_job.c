#include "print_job.h"
#include "netw.h"
#include "raster.h"
#include "urf.h"
#include "ipp.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "print";

#define PAGE_W 1000
#define PAGE_H 1400

/* Encode r as URF and POST one IPP Print-Job. */
static esp_err_t send_once(const raster_t *r, uint32_t dpi, const char *job_name)
{
    char host[64], resource[64];
    uint16_t port = 631;
    if (!netw_is_connected()) {
        ESP_LOGW(TAG, "Wi-Fi not connected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!netw_resolve_printer(host, sizeof host, &port, resource, sizeof resource)) {
        ESP_LOGW(TAG, "printer not found");
        return ESP_ERR_NOT_FOUND;
    }

    /* IPP Print-Job header + URF document in one contiguous body */
    char uri[256];
    snprintf(uri, sizeof uri, "ipp://%s:%u%s", host, (unsigned)port, resource);
    uint8_t ipp[512];
    size_t ipp_len = ipp_build_print_job(ipp, sizeof ipp, 1, uri, "blt-display",
                                         "image/urf", job_name);
    size_t urf_len = urf_encode(r, dpi, NULL, 0);
    size_t body_len = ipp_len + urf_len;

    uint8_t *body = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM);
    if (!body) return ESP_ERR_NO_MEM;
    memcpy(body, ipp, ipp_len);
    urf_encode(r, dpi, body + ipp_len, urf_len);

    char url[256];
    snprintf(url, sizeof url, "http://%s:%u%s", host, (unsigned)port, resource);
    ESP_LOGI(TAG, "printing %dx%d @%u dpi to %s (%u bytes)", r->w, r->h, (unsigned)dpi, url, (unsigned)body_len);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        /* a sleeping printer takes the first data very slowly while it wakes
         * (one 4 KB write blocked ~36 s): 30 s gave "print failed" */
        .timeout_ms = 120000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_set_header(cli, "Content-Type", "application/ipp");

    ESP_LOGI(TAG, "heap: internal %u (largest %u), dma %u (largest %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    /* send in chunks so a stall shows where it happens */
    esp_err_t err = esp_http_client_open(cli, (int)body_len);
    size_t sent = 0;
    int64_t t0 = esp_timer_get_time(), last_log = t0;
    while (err == ESP_OK && sent < body_len) {
        int chunk = body_len - sent > 4096 ? 4096 : (int)(body_len - sent);
        int w = esp_http_client_write(cli, (const char *)body + sent, chunk);
        if (w <= 0) { ESP_LOGE(TAG, "write stalled at %u / %u bytes (%d)", (unsigned)sent, (unsigned)body_len, w); err = ESP_ERR_HTTP_WRITE_DATA; break; }
        sent += (size_t)w;
        int64_t now = esp_timer_get_time();
        if (now - last_log > 2000000) {
            last_log = now;
            ESP_LOGI(TAG, "sent %u / %u bytes (%.1f KB/s)", (unsigned)sent, (unsigned)body_len,
                     sent / 1024.0 / ((now - t0) / 1e6));
        }
    }
    int status = 0;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "sent %u bytes in %.1f s, waiting for the reply", (unsigned)sent, (esp_timer_get_time() - t0) / 1e6);
        if (esp_http_client_fetch_headers(cli) < 0) err = ESP_ERR_HTTP_FETCH_HEADER;
        status = esp_http_client_get_status_code(cli);
        /* the IPP reply: status-code is bytes 2..3 */
        uint8_t rep[64];
        int rn = esp_http_client_read(cli, (char *)rep, sizeof rep);
        if (rn >= 4) ESP_LOGI(TAG, "IPP reply status 0x%02x%02x", rep[2], rep[3]);
    }
    esp_http_client_close(cli);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "printer %s:%u -> HTTP %d", host, (unsigned)port, status);
    } else {
        ESP_LOGE(TAG, "POST failed: %s (HTTP %d)", esp_err_to_name(err), status);
    }
    esp_http_client_cleanup(cli);
    heap_caps_free(body);

    if (err != ESP_OK) return ESP_ERR_TIMEOUT;     /* no answer: network / printer asleep */
    return status == 200 ? ESP_OK : ESP_FAIL;
}

esp_err_t print_job_send_raster(const raster_t *r, uint32_t dpi, const char *job_name)
{
    if (!netw_is_connected()) {                    /* e.g. retries ran out while the printer was off */
        netw_reconnect();
        netw_wait_connected(20000);
    }
    /* no forced Wi-Fi re-join on a stalled upload: that dropped the link (and
     * the BMS) for nothing; the network layer reconnects on a real loss */
    esp_err_t e = send_once(r, dpi, job_name);
    return e == ESP_ERR_TIMEOUT ? ESP_FAIL : e;
}

esp_err_t print_job_send(const report_data_t *d)
{
    /* render report -> grayscale raster (PSRAM) */
    uint8_t *px = heap_caps_malloc((size_t)PAGE_W * PAGE_H, MALLOC_CAP_SPIRAM);
    if (!px) return ESP_ERR_NO_MEM;
    raster_t r;
    raster_init(&r, px, PAGE_W, PAGE_H);
    raster_clear(&r, 255);
    report_render(d, &r);
    esp_err_t e = print_job_send_raster(&r, (uint32_t)CONFIG_BLT_PRINT_DPI, "BLT test report");
    heap_caps_free(px);
    return e;
}

/* ---------------------------------------------------- printer capabilities */

static void log_attrs(const uint8_t *b, int n)
{
    static const char *want[] = {
        "printer-make-and-model", "document-format-supported", "urf-supported",
        "pwg-raster-document-resolution-supported", "printer-resolution-supported",
        "printer-resolution-default", "media-default", "media-ready", "media-supported",
        "print-color-mode-supported", "pwg-raster-document-type-supported",
        "print-quality-supported", "media-top-margin-supported", "media-left-margin-supported",
        "printer-state", "printer-state-reasons", "printer-state-message", "queued-job-count",
        "printer-is-accepting-jobs",
    };
    if (n < 8) return;
    ESP_LOGI(TAG, "IPP status 0x%02x%02x", b[2], b[3]);
    int i = 8;
    char name[64] = "";
    while (i < n) {
        uint8_t tag = b[i++];
        if (tag == IPP_TAG_END) break;
        if (tag < 0x10) continue;                       /* group delimiter */
        if (i + 2 > n) break;
        int nl = (b[i] << 8) | b[i + 1]; i += 2;
        if (i + nl + 2 > n) break;
        if (nl) { int k = nl < 63 ? nl : 63; memcpy(name, b + i, k); name[k] = 0; }
        i += nl;
        int vl = (b[i] << 8) | b[i + 1]; i += 2;
        if (i + vl > n) break;
        const uint8_t *v = b + i;
        i += vl;
        bool hit = false;
        for (size_t w = 0; w < sizeof want / sizeof want[0]; w++) if (!strcmp(name, want[w])) hit = true;
        if (!hit) continue;
        if (tag == IPP_TAG_INTEGER || tag == IPP_TAG_ENUM) {
            int32_t x = (int32_t)((v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3]);
            ESP_LOGI(TAG, "  %s = %ld", name, (long)x);
        } else if (tag == 0x32 && vl == 9) {           /* resolution */
            ESP_LOGI(TAG, "  %s = %ldx%ld u%d", name,
                     (long)((v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3]),
                     (long)((v[4] << 24) | (v[5] << 16) | (v[6] << 8) | v[7]), v[8]);
        } else if (tag >= 0x40) {
            ESP_LOGI(TAG, "  %s = %.*s", name, vl, (const char *)v);
        } else {
            ESP_LOGI(TAG, "  %s (tag 0x%02x, %d bytes)", name, tag, vl);
        }
    }
}

static void query_task(void *arg)
{
    char host[64], resource[64], uri[192], url[192];
    uint16_t port = 631;
    if (!netw_is_connected() || !netw_resolve_printer(host, sizeof host, &port, resource, sizeof resource)) {
        ESP_LOGW(TAG, "attrs: no printer");
        vTaskDelete(NULL);
    }
    snprintf(uri, sizeof uri, "ipp://%s:%u%s", host, (unsigned)port, resource);
    snprintf(url, sizeof url, "http://%s:%u%s", host, (unsigned)port, resource);
    uint8_t req[384];
    size_t rl = ipp_build_get_printer_attributes(req, sizeof req, 7, uri, "blt-display");
    const int cap = 64 * 1024;
    uint8_t *resp = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 10000 };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_set_header(cli, "Content-Type", "application/ipp");
    int got = 0;
    if (resp && esp_http_client_open(cli, (int)rl) == ESP_OK) {
        esp_http_client_write(cli, (const char *)req, (int)rl);
        esp_http_client_fetch_headers(cli);
        int r;
        while (got < cap && (r = esp_http_client_read(cli, (char *)resp + got, cap - got)) > 0) got += r;
    }
    ESP_LOGI(TAG, "attrs: HTTP %d, %d bytes", esp_http_client_get_status_code(cli), got);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    if (got > 0) log_attrs(resp, got);
    heap_caps_free(resp);
    vTaskDelete(NULL);
}

void print_job_query_attrs(void)
{
    xTaskCreate(query_task, "ippq", 8192, NULL, 4, NULL);
}

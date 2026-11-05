#include "print_job.h"
#include "netw.h"
#include "raster.h"
#include "urf.h"
#include "ipp.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"

static const char *TAG = "print";

#define PAGE_W 1000
#define PAGE_H 1400

esp_err_t print_job_send(const report_data_t *d)
{
    char host[64], resource[64];
    uint16_t port = 631;
    if (!netw_is_connected()) {
        ESP_LOGW(TAG, "Wi-Fi not connected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!netw_resolve_printer(host, sizeof host, &port, resource, sizeof resource)) {
        return ESP_ERR_NOT_FOUND;
    }

    /* render report into a grayscale raster (PSRAM) */
    uint8_t *px = heap_caps_malloc((size_t)PAGE_W * PAGE_H, MALLOC_CAP_SPIRAM);
    if (!px) return ESP_ERR_NO_MEM;
    raster_t r;
    raster_init(&r, px, PAGE_W, PAGE_H);
    raster_clear(&r, 255);
    report_render(d, &r);

    /* encode URF */
    size_t urf_len = urf_encode(&r, (uint32_t)CONFIG_BLT_PRINT_DPI, NULL, 0);
    uint8_t *urf = heap_caps_malloc(urf_len, MALLOC_CAP_SPIRAM);
    if (!urf) { heap_caps_free(px); return ESP_ERR_NO_MEM; }
    urf_encode(&r, (uint32_t)CONFIG_BLT_PRINT_DPI, urf, urf_len);
    heap_caps_free(px);

    /* build IPP Print-Job header */
    char uri[256];
    snprintf(uri, sizeof uri, "ipp://%s:%u%s", host, (unsigned)port, resource);
    uint8_t ipp[512];
    size_t ipp_len = ipp_build_print_job(ipp, sizeof ipp, 1, uri, "blt-display",
                                         "image/urf", "BLT test report");

    /* POST (application/ipp = IPP header immediately followed by the document) */
    char url[256];
    snprintf(url, sizeof url, "http://%s:%u%s", host, (unsigned)port, resource);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_set_header(cli, "Content-Type", "application/ipp");

    esp_err_t err = esp_http_client_open(cli, (int)(ipp_len + urf_len));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        heap_caps_free(urf);
        return err;
    }

    int w = esp_http_client_write(cli, (const char *)ipp, ipp_len);
    size_t off = 0;
    while (w >= 0 && off < urf_len) {
        size_t chunk = urf_len - off;
        if (chunk > 4096) chunk = 4096;
        int k = esp_http_client_write(cli, (const char *)(urf + off), chunk);
        if (k < 0) { w = -1; break; }
        off += (size_t)k;
    }
    heap_caps_free(urf);

    if (w < 0) {
        ESP_LOGE(TAG, "write failed");
        esp_http_client_cleanup(cli);
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    ESP_LOGI(TAG, "printer %s:%u -> HTTP %d (%u bytes doc)", host, (unsigned)port,
             status, (unsigned)urf_len);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    return (status == 200) ? ESP_OK : ESP_FAIL;
}

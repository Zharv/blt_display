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
        ESP_LOGW(TAG, "printer not found");
        return ESP_ERR_NOT_FOUND;
    }

    /* render report -> grayscale raster (PSRAM) */
    uint8_t *px = heap_caps_malloc((size_t)PAGE_W * PAGE_H, MALLOC_CAP_SPIRAM);
    if (!px) return ESP_ERR_NO_MEM;
    raster_t r;
    raster_init(&r, px, PAGE_W, PAGE_H);
    raster_clear(&r, 255);
    report_render(d, &r);

    /* IPP Print-Job header + URF document in one contiguous body */
    char uri[256];
    snprintf(uri, sizeof uri, "ipp://%s:%u%s", host, (unsigned)port, resource);
    uint8_t ipp[512];
    size_t ipp_len = ipp_build_print_job(ipp, sizeof ipp, 1, uri, "blt-display",
                                         "image/urf", "BLT test report");
    size_t urf_len = urf_encode(&r, (uint32_t)CONFIG_BLT_PRINT_DPI, NULL, 0);
    size_t body_len = ipp_len + urf_len;

    uint8_t *body = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM);
    if (!body) { heap_caps_free(px); return ESP_ERR_NO_MEM; }
    memcpy(body, ipp, ipp_len);
    urf_encode(&r, (uint32_t)CONFIG_BLT_PRINT_DPI, body + ipp_len, urf_len);
    heap_caps_free(px);

    char url[256];
    snprintf(url, sizeof url, "http://%s:%u%s", host, (unsigned)port, resource);
    ESP_LOGI(TAG, "printing to %s (%u bytes)", url, (unsigned)body_len);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_set_header(cli, "Content-Type", "application/ipp");
    esp_http_client_set_post_field(cli, (const char *)body, (int)body_len);

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "printer %s:%u -> HTTP %d", host, (unsigned)port, status);
    } else {
        ESP_LOGE(TAG, "POST failed: %s (HTTP %d)", esp_err_to_name(err), status);
    }
    esp_http_client_cleanup(cli);
    heap_caps_free(body);

    return (err == ESP_OK && status == 200) ? ESP_OK : ESP_FAIL;
}

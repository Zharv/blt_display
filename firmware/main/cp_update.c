#include "cp_update.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "eh_host_cp_ota.h"

static const char *TAG = "cp_update";

#define CPFW_PART_LABEL  "storage"
#define CPFW_HDR_LEN     16u
#define CPFW_WANT_MAJOR  3u           /* host component is esp_hosted 3.1.0 */

typedef struct {
    char     magic[4];                 /* "CPFW" */
    uint32_t len;
    uint32_t crc;
    uint32_t reserved;
} cpfw_hdr_t;

static uint32_t applied_crc(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open("cpfw", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "crc", &v);
        nvs_close(h);
    }
    return v;
}

static void mark_applied(uint32_t crc)
{
    nvs_handle_t h;
    if (nvs_open("cpfw", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "crc", crc);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t cp_update_if_needed(void)
{
    esp_hosted_coprocessor_fwver_t v = {0};
    esp_err_t verr = esp_hosted_get_coprocessor_fwversion(&v);
    if (verr == ESP_OK) {
        ESP_LOGI(TAG, "C6 firmware %u.%u.%u", (unsigned)v.major1, (unsigned)v.minor1,
                 (unsigned)v.patch1);
    } else {
        ESP_LOGW(TAG, "C6 firmware version unknown (%s) - treating as old",
                 esp_err_to_name(verr));
    }

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_ANY,
                                                           CPFW_PART_LABEL);
    if (!part) {
        ESP_LOGW(TAG, "no '%s' partition", CPFW_PART_LABEL);
        return ESP_OK;
    }

    cpfw_hdr_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof hdr) != ESP_OK ||
        memcmp(hdr.magic, "CPFW", 4) != 0 ||
        hdr.len == 0 || hdr.len > part->size - CPFW_HDR_LEN) {
        ESP_LOGI(TAG, "no C6 image staged in '%s'", CPFW_PART_LABEL);
        return ESP_OK;
    }

    if (verr == ESP_OK && v.major1 >= CPFW_WANT_MAJOR) {
        ESP_LOGI(TAG, "C6 already up to date");
        return ESP_OK;
    }
    if (applied_crc() == hdr.crc) {
        ESP_LOGW(TAG, "staged image (crc %08lx) was already applied - not retrying",
                 (unsigned long)hdr.crc);
        return ESP_OK;
    }

    size_t chunk = EH_HOST_CP_OTA_CHUNK_MAX;
    if (chunk > 4096) chunk = 4096;
    uint8_t *buf = malloc(chunk);
    if (!buf) return ESP_ERR_NO_MEM;

    /* verify the staged image before touching the C6 */
    uint32_t crc = 0;
    for (uint32_t off = 0; off < hdr.len; off += chunk) {
        size_t n = (hdr.len - off < chunk) ? hdr.len - off : chunk;
        if (esp_partition_read(part, CPFW_HDR_LEN + off, buf, n) != ESP_OK) {
            free(buf);
            return ESP_FAIL;
        }
        crc = esp_rom_crc32_le(crc, buf, n);
    }
    if (crc != hdr.crc) {
        ESP_LOGE(TAG, "staged image CRC mismatch (%08lx != %08lx) - skipping",
                 (unsigned long)crc, (unsigned long)hdr.crc);
        free(buf);
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGW(TAG, "updating C6 co-processor: %lu bytes", (unsigned long)hdr.len);
    esp_err_t err = esp_hosted_cp_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        free(buf);
        return err;
    }

    uint32_t last_pct = 0;
    for (uint32_t off = 0; off < hdr.len; off += chunk) {
        size_t n = (hdr.len - off < chunk) ? hdr.len - off : chunk;
        esp_partition_read(part, CPFW_HDR_LEN + off, buf, n);
        err = esp_hosted_cp_ota_write(buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed at %lu: %s", (unsigned long)off,
                     esp_err_to_name(err));
            free(buf);
            return err;   /* old firmware stays active - nothing switched yet */
        }
        uint32_t pct = (off + n) * 100u / hdr.len;
        if (pct >= last_pct + 10) {
            ESP_LOGI(TAG, "  %lu%%", (unsigned long)pct);
            last_pct = pct;
        }
    }
    free(buf);

    err = esp_hosted_cp_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s", esp_err_to_name(err));
        return err;
    }
    /* CP >= 2.6 needs an explicit activate; older CPs switch on end(). */
    esp_err_t aerr = esp_hosted_cp_ota_activate();
    ESP_LOGI(TAG, "ota_activate: %s", esp_err_to_name(aerr));

    mark_applied(hdr.crc);
    ESP_LOGW(TAG, "C6 update written - restarting to boot the new co-processor firmware");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

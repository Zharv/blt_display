#include "blt_prepare.h"
#include "blt_usb.h"
#include "blt_blocks.h"
#include "blt_live.h"
#include "i18n.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "blt_prep";

/* main.c: latest live block and its age */
extern bool blt_live_latest(blt_live_t *out, uint32_t *age_ms);

static esp_err_t read_block(uint32_t addr, uint8_t *buf, uint16_t len)
{
    static blt_response_t r;
    blt_err_t be = BLT_OK;
    esp_err_t e = blt_usb_read(BLT_CMD_RD, addr, len, &r, &be, 2600);
    if (e != ESP_OK) return e;
    if (r.payload_len != len) return ESP_ERR_INVALID_SIZE;
    memcpy(buf, r.payload, len);
    return blt_block_valid(buf, len) ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t blt_prepare_read(char *serial, size_t n, uint16_t *rated_ah)
{
    uint8_t id[BLT_ID_LEN], cfg[BLT_CFG_LEN];
    esp_err_t e = read_block(BLT_ID_ADDR, id, sizeof id);
    if (e == ESP_OK && serial) blt_id_get_serial(id, serial, n);
    if (e == ESP_OK) e = read_block(BLT_CFG_ADDR, cfg, sizeof cfg);
    if (e == ESP_OK && rated_ah) *rated_ah = blt_cfg_get_rated_ah(cfg);
    return e;
}

static esp_err_t write_verify(uint32_t addr, const uint8_t *blk, uint16_t len)
{
    blt_usb_arm_write();
    esp_err_t e = blt_usb_write_block(addr, blk, len);
    if (e != ESP_OK) return e;
    uint8_t back[BLT_CFG_LEN];
    e = read_block(addr, back, len);
    if (e != ESP_OK) return e;
    if (memcmp(back, blk, len) != 0) {
        ESP_LOGE(TAG, "read-back of %06lx differs", (unsigned long)addr);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t blt_prepare_write(const char *serial, uint16_t rated_ah, char *msg, size_t n)
{
    blt_live_t l;
    uint32_t age = 0;
    if (!blt_live_latest(&l, &age) || age > 5000) {
        snprintf(msg, n, "%s", tr(S_PREP_NO_TESTER));
        return ESP_ERR_INVALID_STATE;
    }
    if (l.state != BLT_ST_START) {                 /* never touch a running test */
        snprintf(msg, n, "%s", tr(S_PREP_BUSY));
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t id[BLT_ID_LEN], cfg[BLT_CFG_LEN], id0[BLT_ID_LEN], cfg0[BLT_CFG_LEN];
    esp_err_t e = read_block(BLT_ID_ADDR, id, sizeof id);
    if (e == ESP_OK) e = read_block(BLT_CFG_ADDR, cfg, sizeof cfg);
    if (e != ESP_OK) {
        snprintf(msg, n, "%s (%s)", tr(S_PREP_READ_FAIL), esp_err_to_name(e));
        return e;
    }
    memcpy(id0, id, sizeof id);
    memcpy(cfg0, cfg, sizeof cfg);
    if (serial && serial[0] && !blt_id_set_serial(id, serial)) {
        snprintf(msg, n, "%s", tr(S_PREP_BAD_SERIAL));
        return ESP_ERR_INVALID_ARG;
    }
    if (rated_ah && !blt_cfg_set_rated_ah(cfg, rated_ah)) {
        snprintf(msg, n, "%s", tr(S_PREP_BAD_AH));
        return ESP_ERR_INVALID_ARG;
    }

    bool id_ch = memcmp(id, id0, sizeof id) != 0, cfg_ch = memcmp(cfg, cfg0, sizeof cfg) != 0;
    if (!id_ch && !cfg_ch) {
        snprintf(msg, n, "%s", tr(S_PREP_SAME));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "preparing test: serial %s, rated %u Ah (id %s, cfg %s)",
             serial ? serial : "-", rated_ah, id_ch ? "write" : "same", cfg_ch ? "write" : "same");
    if (id_ch) e = write_verify(BLT_ID_ADDR, id, sizeof id);
    if (e == ESP_OK && cfg_ch) e = write_verify(BLT_CFG_ADDR, cfg, sizeof cfg);
    if (e != ESP_OK) {
        snprintf(msg, n, "%s (%s)", tr(S_PREP_WRITE_FAIL), esp_err_to_name(e));
        return e;
    }
    snprintf(msg, n, "%s", tr(S_PREP_OK));
    return ESP_OK;
}

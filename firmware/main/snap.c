#include "snap.h"
#include "ui_frame.h"
#include "blt_usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#define SNAP_UART   UART_NUM_0
#define SNAP_BAUD   921600
#define BASE_BAUD   CONFIG_ESP_CONSOLE_UART_BAUDRATE

static const char *TAG = "snap";

/* Encode one row; returns encoded byte count. out may be NULL (size pass). */
static size_t enc_row(const uint16_t *px, int w, uint8_t *out)
{
    size_t o = 0;
    int i = 0;
    while (i < w) {
        int run = 1;
        while (i + run < w && run < 128 && px[i + run] == px[i]) run++;
        if (run >= 3) {
            if (out) { out[o] = (uint8_t)(0x80 | (run - 1)); memcpy(&out[o + 1], &px[i], 2); }
            o += 3;
            i += run;
            continue;
        }
        int lit = 0;   /* literals until a run of >=3 starts */
        while (i + lit < w && lit < 128) {
            if (i + lit + 2 < w && px[i + lit] == px[i + lit + 1] && px[i + lit] == px[i + lit + 2]) break;
            lit++;
        }
        if (out) { out[o] = (uint8_t)(lit - 1); memcpy(&out[o + 1], &px[i], (size_t)lit * 2); }
        o += 1 + (size_t)lit * 2;
        i += lit;
    }
    return o;
}

static void send_str(const char *s) { uart_write_bytes(SNAP_UART, s, strlen(s)); }

static void do_snap(void)
{
    bsp_display_lock(0);
    lv_draw_buf_t *db = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    bsp_display_unlock();
    if (!db) { send_str("\nSNAP-ERR nomem\n"); return; }

    int w = db->header.w, h = db->header.h;
    uint32_t stride = db->header.stride;
    size_t total = 0;
    for (int y = 0; y < h; y++) total += enc_row((const uint16_t *)(db->data + y * stride), w, NULL);

    uint8_t *row = malloc((size_t)w * 3 + 8);
    if (!row) { lv_draw_buf_destroy(db); send_str("\nSNAP-ERR nomem\n"); return; }

    esp_log_level_set("*", ESP_LOG_NONE);
    char hdr[64];
    snprintf(hdr, sizeof hdr, "\nSNAP-BAUD %d\n", SNAP_BAUD);
    send_str(hdr);
    uart_wait_tx_done(SNAP_UART, pdMS_TO_TICKS(200));
    vTaskDelay(pdMS_TO_TICKS(300));
    uart_set_baudrate(SNAP_UART, SNAP_BAUD);
    vTaskDelay(pdMS_TO_TICKS(300));

    snprintf(hdr, sizeof hdr, "\nSNAP-BEGIN %d %d %u\n", w, h, (unsigned)total);
    send_str(hdr);
    for (int y = 0; y < h; y++) {
        size_t n = enc_row((const uint16_t *)(db->data + y * stride), w, row);
        uart_write_bytes(SNAP_UART, row, n);
    }
    send_str("\nSNAP-END\n");
    uart_wait_tx_done(SNAP_UART, pdMS_TO_TICKS(30000));
    vTaskDelay(pdMS_TO_TICKS(300));
    uart_set_baudrate(SNAP_UART, BASE_BAUD);
    esp_log_level_set("*", ESP_LOG_INFO);

    free(row);
    lv_draw_buf_destroy(db);
    ESP_LOGI(TAG, "snapshot %dx%d sent (%u bytes)", w, h, (unsigned)total);
}

static void snap_task(void *arg)
{
    (void)arg;
    char line[48];
    int len = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(SNAP_UART, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r') continue;
        if (c != '\n') {
            if (len < (int)sizeof line - 1) line[len++] = (char)c;
            continue;
        }
        line[len] = '\0';
        len = 0;
        if (strcmp(line, "SNAP") == 0) do_snap();
        else if (strncmp(line, "TAB ", 4) == 0) snap_nav(atoi(line + 4));
        else if (strncmp(line, "CHART ", 6) == 0) snap_chart(atoi(line + 6));
        else if (strncmp(line, "CELL ", 5) == 0) snap_cell(atoi(line + 5));
        else if (strncmp(line, "BLTALLOW ", 9) == 0) blt_usb_allow_reads(atoi(line + 9) != 0);
        else if (strncmp(line, "BLT ", 4) == 0) {        /* "BLT RD 000000 0010" - reads only */
            char cmd[3] = { 0 };
            unsigned long addr = 0, len = 0;
            if (sscanf(line + 4, "%2s %lx %lx", cmd, &addr, &len) == 3) {
                blt_cmd_t c;
                if (blt_cmd_from_str(cmd, &c)) blt_usb_read(c, (uint32_t)addr, (uint16_t)len, NULL, NULL, 2000);
            }
        }
        else if (strcmp(line, "BMS") == 0) snap_bms_connect();
        else if (strcmp(line, "BMSOFF") == 0) snap_bms_disconnect();

    }
}

void snap_start(void)
{
    if (!uart_is_driver_installed(SNAP_UART)) {
        if (uart_driver_install(SNAP_UART, 512, 4096, 0, NULL, 0) != ESP_OK) return;
        uart_vfs_dev_use_driver(SNAP_UART);
    }
    xTaskCreate(snap_task, "snap", 4096, NULL, 2, NULL);
}

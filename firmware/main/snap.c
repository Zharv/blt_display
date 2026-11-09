#include "snap.h"
#include "ui_frame.h"
#include "blt_usb.h"
#include "print_job.h"
#include "sd_store.h"
#include "doc_print.h"
#include "ui_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
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

/* the console works on UART0 (CH340 port) and on the chip's own USB
 * Serial/JTAG port; replies go back where the command came from */
typedef enum { T_UART = 0, T_USB } tport_t;
static tport_t s_cur;
static bool s_usb_ok;

static void tx(const void *d, size_t n)
{
    if (s_cur == T_UART) uart_write_bytes(SNAP_UART, d, n);
    else usb_serial_jtag_write_bytes(d, n, pdMS_TO_TICKS(2000));
}

static void tx_wait(uint32_t ms)
{
    if (s_cur == T_UART) uart_wait_tx_done(SNAP_UART, pdMS_TO_TICKS(ms));
    else usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(ms));
}

static void send_str(const char *s) { tx(s, strlen(s)); }

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
    tx_wait(200);
    vTaskDelay(pdMS_TO_TICKS(300));
    if (s_cur == T_UART) uart_set_baudrate(SNAP_UART, SNAP_BAUD);   /* USB: no baud rate */
    vTaskDelay(pdMS_TO_TICKS(300));

    snprintf(hdr, sizeof hdr, "\nSNAP-BEGIN %d %d %u\n", w, h, (unsigned)total);
    send_str(hdr);
    for (int y = 0; y < h; y++) {
        size_t n = enc_row((const uint16_t *)(db->data + y * stride), w, row);
        tx(row, n);
    }
    send_str("\nSNAP-END\n");
    tx_wait(30000);
    vTaskDelay(pdMS_TO_TICKS(300));
    if (s_cur == T_UART) uart_set_baudrate(SNAP_UART, BASE_BAUD);
    esp_log_level_set("*", ESP_LOG_INFO);

    free(row);
    lv_draw_buf_destroy(db);
    ESP_LOGI(TAG, "snapshot %dx%d sent (%u bytes)", w, h, (unsigned)total);
}

static void run_cmd(const char *line);

static void snap_task(void *arg)
{
    (void)arg;
    char line[2][48];
    int len[2] = { 0, 0 };
    for (;;) {
        /* one task, both ports (each read waits up to 20 ms) */
        for (int t = 0; t < 2; t++) {
            uint8_t c;
            int r = t == T_UART ? uart_read_bytes(SNAP_UART, &c, 1, pdMS_TO_TICKS(20))
                  : (s_usb_ok ? usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(20)) : 0);
            if (r != 1 || c == '\r') continue;
            if (c != '\n') {
                if (len[t] < (int)sizeof line[t] - 1) line[t][len[t]++] = (char)c;
                continue;
            }
            line[t][len[t]] = '\0';
            len[t] = 0;
            s_cur = (tport_t)t;
            run_cmd(line[t]);
        }
    }
}

static void run_cmd(const char *line)
{
    {
        if (strcmp(line, "SNAP") == 0) do_snap();
        else if (strncmp(line, "TAB ", 4) == 0) snap_nav(atoi(line + 4));
        else if (strncmp(line, "CHART ", 6) == 0) snap_chart(atoi(line + 6));
        else if (strncmp(line, "CELL ", 5) == 0) snap_cell(atoi(line + 5));
        else if (strncmp(line, "BLTALLOW ", 9) == 0) blt_usb_allow_reads(atoi(line + 9) != 0);
        else if (strcmp(line, "BLTONCE") == 0) snap_blt_once();
        else if (strncmp(line, "BLTPOLL ", 8) == 0) blt_live_poll_enable(atoi(line + 8) != 0);
        else if (strncmp(line, "BLT ", 4) == 0) {        /* "BLT RD 000000 0010" - reads only */
            char cmd[3] = { 0 };
            unsigned long addr = 0, len = 0;
            if (sscanf(line + 4, "%2s %lx %lx", cmd, &addr, &len) == 3) {
                blt_cmd_t c;
                if (blt_cmd_from_str(cmd, &c)) blt_usb_read(c, (uint32_t)addr, (uint16_t)len, NULL, NULL, 2000);
            }
        }
        else if (strcmp(line, "BMS") == 0) snap_bms_connect(NULL);
        else if (strncmp(line, "BMS ", 4) == 0) snap_bms_connect(line + 4);
        else if (strcmp(line, "BMSOFF") == 0) snap_bms_disconnect();
        else if (strcmp(line, "IPPATTR") == 0) print_job_query_attrs();
        else if (strcmp(line, "SD") == 0) sd_store_selftest();
        else if (strcmp(line, "PRINTTEST") == 0) doc_print_test_page();
        else if (strcmp(line, "SDLS") == 0) sd_store_ls(NULL);
        else if (strncmp(line, "SDLS ", 5) == 0) sd_store_ls(line + 5);
        else if (strcmp(line, "PROTO") == 0) snap_protocol(false, 0xFFFF);
        else if (strncmp(line, "PROTO ", 6) == 0) snap_protocol(false, (uint16_t)atoi(line + 6));
        else if (strcmp(line, "PROTOPRINT") == 0) snap_protocol(true, 0xFFFF);
        else if (strncmp(line, "ARSEL ", 6) == 0) ui_archive_dev(atoi(line + 6), false);
        else if (strcmp(line, "AREDIT") == 0) ui_archive_dev(-1, true);
    }
}

void snap_start(void)
{
    if (!uart_is_driver_installed(SNAP_UART)) {
        if (uart_driver_install(SNAP_UART, 512, 4096, 0, NULL, 0) != ESP_OK) return;
        uart_vfs_dev_use_driver(SNAP_UART);
    }
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t uc = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        if (usb_serial_jtag_driver_install(&uc) == ESP_OK) {
            usb_serial_jtag_vfs_use_driver();
            s_usb_ok = true;
        }
    } else {
        s_usb_ok = true;
    }
    xTaskCreate(snap_task, "snap", 4096, NULL, 2, NULL);
}

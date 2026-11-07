#include "blt_usb.h"

#include <stdio.h>
#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "soc/usb_dwc_struct.h"

static const char *TAG = "blt_usb";

#define BLT_VID 0x04D8
#define BLT_PID 0x000A

static volatile blt_usb_state_t s_state = BLT_USB_OFF;
static volatile uint16_t s_vid, s_pid;
static volatile bool s_reads_allowed;
static cdc_acm_dev_hdl_t s_dev;
static SemaphoreHandle_t s_done;      /* a full line ('\r') arrived */
static SemaphoreHandle_t s_txn;       /* one transaction at a time */

/* RX line assembly (shared with the transaction while it waits) */
static uint8_t s_rx[2 * BLT_MAX_PAYLOAD + 64];
static volatile size_t s_rx_len;
static volatile bool s_waiting;
static bool s_fsls_api;               /* espressif/usb >= 1.6 latched FS/LS-only at install */
static bool s_quiet;                  /* polling: raw frames only at debug level */

static void log_bytes(const char *what, const uint8_t *d, size_t n)
{
    char line[200];
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < sizeof line; i++) {
        uint8_t c = d[i];
        if (c >= 0x20 && c < 0x7F) line[o++] = (char)c;
        else o += snprintf(line + o, sizeof line - o, "<%02x>", c);
    }
    line[o] = '\0';
    ESP_LOGI(TAG, "%s (%u B): %s%s", what, (unsigned)n, line, o + 4 >= sizeof line ? "..." : "");
}

static bool on_rx(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    if (!s_waiting) {                     /* the tester spoke on its own: just log it */
        log_bytes("unsolicited RX", data, len);
        return true;
    }
    for (size_t i = 0; i < len; i++) {
        if (s_rx_len < sizeof s_rx) s_rx[s_rx_len++] = data[i];
        if (data[i] == BLT_EOF && s_rx_len > 1) {
            s_waiting = false;
            xSemaphoreGive(s_done);
            break;
        }
    }
    return true;
}

static void on_event(const cdc_acm_host_dev_event_data_t *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "tester disconnected");
        cdc_acm_host_close(ev->data.cdc_hdl);
        s_dev = NULL;
        s_state = BLT_USB_WAITING;
        break;
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC error %d", ev->data.error);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "serial state 0x%04x", ev->data.serial_state.val);
        break;
    default:
        break;
    }
}

/* runs in the USB host context: just record what was plugged in */
static void on_new_dev(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *dd;
    if (usb_host_get_device_descriptor(usb_dev, &dd) == ESP_OK) {
        s_vid = dd->idVendor;
        s_pid = dd->idProduct;
        ESP_LOGI(TAG, "USB device %04x:%04x class %02x%s", dd->idVendor, dd->idProduct,
                 dd->bDeviceClass,
                 (dd->idVendor == BLT_VID && dd->idProduct == BLT_PID) ? " (Microchip CDC - BLT tester)" : "");
    }
}

/* The tester is a full-speed device (PIC18, EP0 = 8 B). On the P4's
 * high-speed DWC core its enumeration failed at the config descriptor ~every
 * time (EP0 transaction errors). The core has an "FS/LS only" mode (HCFG
 * FSLSSupp) that IDF 5.5 never sets; set it before the port is powered so the
 * port runs as a plain full-speed host. */
static void force_fs_only(void)
{
    USB_DWC_HS.hcfg_reg.fslssupp = 1;
}

static void conn_task(void *arg)
{
    (void)arg;
    const cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = 1500,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .event_cb = on_event,
        .data_cb = on_rx,
        .user_arg = NULL,
    };
    int misses = 0;
    for (;;) {
        if (s_dev) { misses = 0; vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        s_state = BLT_USB_WAITING;
        cdc_acm_dev_hdl_t h = NULL;
        /* any CDC device: log what is actually on the port */
        esp_err_t e = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID, 0, &cfg, &h);
        if (e != ESP_OK) {
            /* The host does not retry a failed enumeration by itself: cycle the
             * root port every ~3 s so a device that failed (or was plugged in
             * while the port was busy) is enumerated again. Plain USB, nothing
             * is sent to the tester. */
            if (++misses >= 2) {
                misses = 0;
                usb_host_lib_set_root_port_power(false);
                vTaskDelay(pdMS_TO_TICKS(300));
                if (!s_fsls_api) force_fs_only();
                usb_host_lib_set_root_port_power(true);
            }
            continue;
        }
        s_dev = h;
        s_state = BLT_USB_CONNECTED;
        ESP_LOGI(TAG, "CDC device open (passive: nothing is sent to it)");
        cdc_acm_host_desc_print(h);
    }
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}


esp_err_t blt_usb_start(void)
{
    s_done = xSemaphoreCreateBinary();
    s_txn = xSemaphoreCreateMutex();
    /* Own install (not bsp_usb_host_start), HS controller ONLY (BIT0).
     * Never enable the full-speed USB 1.1 controller (BIT1) on this board: its
     * internal PHY sits on GPIO26/27 and GPIO27 is the LCD reset - the panel
     * goes black. (usb_host fsls_only needs a newer IDF HAL than 5.5; the HS
     * core gets the HCFG FS/LS-only bit instead, see force_fs_only.) */
    usb_host_config_t hc = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .peripheral_map = BIT0,
    };
    esp_err_t e = usb_host_install(&hc);
    if (e != ESP_OK) { ESP_LOGE(TAG, "usb host: %s", esp_err_to_name(e)); return e; }
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    const cdc_acm_host_driver_config_t drv = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 9,
        .xCoreID = 0,
        .new_dev_cb = on_new_dev,
    };
    e = cdc_acm_host_install(&drv);
    if (e != ESP_OK) { ESP_LOGE(TAG, "cdc install: %s", esp_err_to_name(e)); return e; }
    if (!s_fsls_api) {
        /* re-power the port in FS-only mode (see force_fs_only) */
        usb_host_lib_set_root_port_power(false);
        vTaskDelay(pdMS_TO_TICKS(100));
        force_fs_only();
        usb_host_lib_set_root_port_power(true);
    }
    ESP_LOGI(TAG, "HS core FS/LS-only: %s, hcfg.fslssupp=%d", s_fsls_api ? "usb_host fsls_only" : "HCFG bit",
             (int)USB_DWC_HS.hcfg_reg.fslssupp);
    s_state = BLT_USB_WAITING;
    xTaskCreate(conn_task, "blt_usb", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "waiting for the tester on USB (passive mode)");
    return ESP_OK;
}

blt_usb_state_t blt_usb_state(uint16_t *vid, uint16_t *pid)
{
    if (vid) *vid = s_vid;
    if (pid) *pid = s_pid;
    return s_state;
}

void blt_usb_allow_reads(bool on)
{
    s_reads_allowed = on;
    ESP_LOGW(TAG, "read commands %s", on ? "ALLOWED" : "blocked");
}

bool blt_usb_reads_allowed(void) { return s_reads_allowed; }

/* Pure reads only (docs/BLT_REGISTERS.md). NOT RE: despite the name it is the
 * action command - RE 0/1/2 reset statistics/cycles/graph on the tester. */
static bool is_read_cmd(blt_cmd_t c)
{
    return c == BLT_CMD_RI || c == BLT_CMD_RD || c == BLT_CMD_RW || c == BLT_CMD_RB;
}

/* BLTView never reads more than 64 bytes per frame */
#define BLT_USB_MAX_READ 64

esp_err_t blt_usb_read(blt_cmd_t cmd, uint32_t addr, uint16_t len, blt_response_t *out,
                       blt_err_t *perr, uint32_t timeout_ms)
{
    if (!is_read_cmd(cmd)) {
        ESP_LOGE(TAG, "refused: %s is not a read command", blt_cmd_str(cmd));
        return ESP_ERR_NOT_ALLOWED;
    }
    if (!s_reads_allowed) {
        ESP_LOGW(TAG, "refused: reads not allowed (tester may be testing)");
        return ESP_ERR_NOT_ALLOWED;
    }
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (len == 0 || len > BLT_USB_MAX_READ) return ESP_ERR_INVALID_ARG;

    uint8_t frame[BLT_REQ_HEADER_LEN + 8];
    size_t n = blt_build_frame(frame, sizeof frame, BLT_NODE_DEFAULT, cmd, addr, NULL, len);
    if (!n) return ESP_ERR_INVALID_SIZE;

    xSemaphoreTake(s_txn, portMAX_DELAY);
    s_rx_len = 0;
    xSemaphoreTake(s_done, 0);
    s_waiting = true;
    if (!s_quiet) log_bytes("TX", frame, n);
    esp_err_t e = cdc_acm_host_data_tx_blocking(s_dev, frame, n, 1000);
    if (e == ESP_OK && xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) e = ESP_ERR_TIMEOUT;
    s_waiting = false;
    if (s_rx_len && (!s_quiet || e != ESP_OK)) log_bytes(e == ESP_ERR_TIMEOUT ? "RX (partial)" : "RX", s_rx, s_rx_len);
    if (e == ESP_OK) {
        static blt_response_t tmp;
        blt_response_t *r = out ? out : &tmp;
        /* skip anything before the '!' start char */
        size_t st = 0;
        while (st < s_rx_len && s_rx[st] != BLT_START_RESP) st++;
        blt_err_t be = blt_parse_response(s_rx + st, s_rx_len - st, cmd, BLT_NODE_DEFAULT, r);
        if (perr) *perr = be;
        if (be != BLT_OK) {
            ESP_LOGW(TAG, "response: %s", blt_err_str(be));
            e = ESP_ERR_INVALID_RESPONSE;
        }
    } else {
        ESP_LOGW(TAG, "%s %06lx: %s", blt_cmd_str(cmd), (unsigned long)addr, esp_err_to_name(e));
    }
    xSemaphoreGive(s_txn);
    return e;
}

esp_err_t blt_usb_read_live(blt_live_t *out)
{
    static blt_response_t r;
    static uint8_t blk[BLT_LIVE_LEN];
    s_quiet = true;
    esp_err_t e = ESP_OK;
    for (int i = 0; i < 3 && e == ESP_OK; i++) {
        blt_err_t be = BLT_OK;
        e = blt_usb_read(BLT_CMD_RI, (uint32_t)(i * 64), 64, &r, &be, 2600);
        if (e == ESP_OK && r.payload_len != 64) e = ESP_ERR_INVALID_SIZE;
        if (e == ESP_OK) memcpy(blk + i * 64, r.payload, 64);
    }
    s_quiet = false;
    if (e != ESP_OK) return e;
    return blt_live_decode(blk, sizeof blk, out) ? ESP_OK : ESP_FAIL;
}

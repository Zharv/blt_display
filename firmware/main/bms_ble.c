#include "bms_ble.h"
#include "bms_jk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "esp_hosted_bt_host_stack.h"

static const char *TAG = "bms_ble";

#define JK_SVC_UUID   0xFFE0
#define JK_CHR_UUID   0xFFE1
#define CCCD_UUID     0x2902
#define NVS_NS        "bms"

static SemaphoreHandle_t s_lock;
static volatile bms_ble_state_t s_state = BMS_BLE_OFF;
static uint8_t s_own_addr_type;
static volatile bool s_synced;

/* target + connection */
static bms_ble_dev_t s_target;
static volatile bool s_have_target;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
/* Some JK units expose TWO characteristics with UUID 0xFFE1 at different
 * handles: one notifies, one takes writes (seen in the ZHARV-BMS reader). */
static uint16_t s_svc_start, s_svc_end, s_notify_handle, s_write_handle, s_cccd;
static bool s_write_with_rsp;

/* decoded data */
static bms_data_t s_data;
static int64_t s_data_us;
static uint32_t s_frames;
static int s_variant_hint = -1;          /* from the device-info frame, if any */

/* JK frame reassembly (notifications arrive in MTU-sized pieces) */
static uint8_t s_rx[512];
static size_t s_rx_len;

/* scan handoff */
static bms_ble_dev_t *s_scan_out;
static int s_scan_max, s_scan_n;
static SemaphoreHandle_t s_scan_done;

static esp_timer_handle_t s_reconnect_t;
static esp_timer_handle_t s_kick_t;
static int64_t s_link_us;       /* when notifications were enabled */

static int gap_event(struct ble_gap_event *ev, void *arg);

/* ------------------------------------------------------------------ NVS */

static void save_target(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "dev", &s_target, sizeof s_target);
    nvs_commit(h);
    nvs_close(h);
}

static void forget_target(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "dev");
    nvs_commit(h);
    nvs_close(h);
}

static void load_target(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s_target;
    if (nvs_get_blob(h, "dev", &s_target, &len) == ESP_OK && len == sizeof s_target) {
        s_target.name[sizeof s_target.name - 1] = '\0';
        s_have_target = true;
        ESP_LOGI(TAG, "saved BMS: %s", s_target.name[0] ? s_target.name : "(no name)");
    }
    nvs_close(h);
}

/* ------------------------------------------------------------- JK frames */

static void send_cmd(uint8_t cmd)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_write_handle) return;
    uint8_t f[20];
    jk_build_command(f, sizeof f, cmd, 0, 0);
    int rc = s_write_with_rsp
        ? ble_gattc_write_flat(s_conn, s_write_handle, f, sizeof f, NULL, NULL)
        : ble_gattc_write_no_rsp_flat(s_conn, s_write_handle, f, sizeof f);
    if (rc) ESP_LOGW(TAG, "write cmd 0x%02x rc=%d", cmd, rc);
}

static long cells_vs_pack(const bms_data_t *d)
{
    if (d->cell_count == 0 || d->pack_mv <= 1000 || d->pack_mv > 200000) return 1L << 30;
    long sum = 0;
    for (int i = 0; i < d->cell_count && i < BMS_MAX_CELLS; i++) sum += d->cell_mv[i];
    long diff = sum - d->pack_mv;
    return diff < 0 ? -diff : diff;
}

static void copy_ascii(char *dst, size_t dn, const uint8_t *src, size_t sn)
{
    size_t j = 0;
    for (size_t i = 0; i < sn && j + 1 < dn; i++) {
        if (src[i] == 0) break;
        dst[j++] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
    }
    dst[j] = '\0';
}

static void handle_frame(const uint8_t *f, size_t len)
{
    if (len < JK_FRAME_LEN) return;
    if (jk_checksum(f, JK_FRAME_LEN - 1) != f[JK_FRAME_LEN - 1]) {
        ESP_LOGW(TAG, "frame type 0x%02x: bad checksum", f[4]);
        return;
    }

    if (f[4] == JK_TYPE_DEVINFO) {
        char vendor[17], hw[9], sw[9];
        copy_ascii(vendor, sizeof vendor, &f[6], 16);
        copy_ascii(hw, sizeof hw, &f[22], 8);
        copy_ascii(sw, sizeof sw, &f[30], 8);
        int hw_major = atoi(hw);
        s_variant_hint = (hw_major >= 11) ? JK_VARIANT_32S : JK_VARIANT_24S;
        ESP_LOGI(TAG, "JK device: %s hw %s sw %s -> %s", vendor, hw, sw,
                 s_variant_hint == JK_VARIANT_32S ? "32S layout" : "24S layout");
        return;
    }
    if (f[4] != JK_TYPE_CELLINFO) return;   /* settings (0x01) etc. ignored */

    /* Decode with both layouts and keep the one whose cells add up to the pack
     * voltage; the device-info hint breaks ties. */
    bms_data_t a, b;
    long sa = (jk_parse_cellinfo(f, len, JK_VARIANT_24S, &a) == JK_OK) ? cells_vs_pack(&a) : (1L << 30);
    long sb = (jk_parse_cellinfo(f, len, JK_VARIANT_32S, &b) == JK_OK) ? cells_vs_pack(&b) : (1L << 30);
    bool use32 = (sb < sa) || (sb == sa && s_variant_hint == JK_VARIANT_32S);
    const bms_data_t *pick = use32 ? &b : &a;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data = *pick;
    s_data_us = esp_timer_get_time();
    s_frames++;
    uint32_t n = s_frames;
    xSemaphoreGive(s_lock);
    s_state = BMS_BLE_STREAMING;

    if (n == 1 || n % 60 == 0) {
        ESP_LOGI(TAG, "JK %s: %u cells, %.2f V, %.2f A, SOC %u%%, d=%u mV (frame %lu)",
                 use32 ? "32S" : "24S", pick->cell_count, pick->pack_mv / 1000.0,
                 pick->pack_ma / 1000.0, pick->soc_pct, pick->imbalance_mv, (unsigned long)n);
    }
}

static void jk_feed(const uint8_t *d, size_t n)
{
    bool hdr = n >= 4 && d[0] == JK_HDR0 && d[1] == JK_HDR1 && d[2] == JK_HDR2 && d[3] == JK_HDR3;
    if (hdr) s_rx_len = 0;
    else if (s_rx_len == 0) return;          /* tail of a frame we already used */
    if (s_rx_len + n > sizeof s_rx) { s_rx_len = 0; return; }
    memcpy(s_rx + s_rx_len, d, n);
    s_rx_len += n;
    if (s_rx_len >= JK_FRAME_LEN) {
        handle_frame(s_rx, s_rx_len);
        s_rx_len = 0;
    }
}

/* --------------------------------------------------------- GATT set-up */

static void kick_cb(void *arg)
{
    (void)arg;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    /* JK streams cell info after 0x96; re-request if it went quiet. Some units
     * stop notifying on a live link and ignore re-requests (seen after ~2 min):
     * after 15 s of silence drop the link - the disconnect handler reconnects. */
    int64_t last = s_data_us > s_link_us ? s_data_us : s_link_us;
    int64_t quiet = esp_timer_get_time() - last;
    if (quiet > 15000000) {
        ESP_LOGW(TAG, "no data for %d s - reconnecting", (int)(quiet / 1000000));
        esp_timer_stop(s_kick_t);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    } else if (quiet > 4000000) {
        send_cmd(JK_CMD_CELLINFO);
    }
}

static int cccd_written(uint16_t conn, const struct ble_gatt_error *err,
                        struct ble_gatt_attr *attr, void *arg)
{
    if (err->status != 0) {
        ESP_LOGW(TAG, "enable notify failed: %d", err->status);
        s_state = BMS_BLE_ERROR;
        return 0;
    }
    ESP_LOGI(TAG, "notifications on - requesting device + cell info");
    s_state = BMS_BLE_CONNECTED;
    s_link_us = esp_timer_get_time();
    send_cmd(JK_CMD_DEVINFO);
    esp_timer_stop(s_kick_t);
    esp_timer_start_periodic(s_kick_t, 1500 * 1000);
    return 0;
}

static void enable_notify(uint16_t conn)
{
    uint16_t handle = s_cccd ? s_cccd : (uint16_t)(s_notify_handle + 1);
    uint8_t on[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(conn, handle, on, sizeof on, cccd_written, NULL);
    if (rc) { ESP_LOGW(TAG, "cccd write rc=%d", rc); s_state = BMS_BLE_ERROR; }
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0) {
        /* first CCCD after the notify value handle belongs to that characteristic */
        if (!s_cccd && ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(CCCD_UUID)) == 0)
            s_cccd = dsc->handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE) enable_notify(conn);
    else { ESP_LOGW(TAG, "descriptor discovery: %d", err->status); enable_notify(conn); }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0) {
        uint8_t p = chr->properties;
        ESP_LOGI(TAG, "FFE1 @0x%04x props 0x%02x", chr->val_handle, p);
        if ((p & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) && !s_notify_handle)
            s_notify_handle = chr->val_handle;
        if (p & BLE_GATT_CHR_PROP_WRITE_NO_RSP) {
            s_write_handle = chr->val_handle;          /* preferred */
            s_write_with_rsp = false;
        } else if ((p & BLE_GATT_CHR_PROP_WRITE) && !s_write_handle) {
            s_write_handle = chr->val_handle;
            s_write_with_rsp = true;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE && (s_notify_handle || s_write_handle)) {
        /* single-characteristic units: one handle does both jobs */
        if (!s_notify_handle) s_notify_handle = s_write_handle;
        if (!s_write_handle) { s_write_handle = s_notify_handle; s_write_with_rsp = false; }
        ESP_LOGI(TAG, "JK notify @0x%04x, write @0x%04x (%s)", s_notify_handle, s_write_handle,
                 s_write_with_rsp ? "with response" : "no response");
        int rc = ble_gattc_disc_all_dscs(conn, s_notify_handle, s_svc_end, on_dsc, NULL);
        if (rc) enable_notify(conn);
    } else {
        ESP_LOGW(TAG, "JK characteristic 0xFFE1 not found (%d)", err->status);
        s_state = BMS_BLE_ERROR;
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE && s_svc_start) {
        ble_gattc_disc_chrs_by_uuid(conn, s_svc_start, s_svc_end,
                                    BLE_UUID16_DECLARE(JK_CHR_UUID), on_chr, NULL);
    } else {
        ESP_LOGW(TAG, "JK service 0xFFE0 not found (%d) - not a JK BMS?", err->status);
        s_state = BMS_BLE_ERROR;
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    ESP_LOGI(TAG, "MTU %u", mtu);
    return 0;
}

/* -------------------------------------------------------------- connect */

static void start_connect(void)
{
    if (!s_synced || !s_have_target || s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    ble_gap_disc_cancel();
    ble_addr_t a = { .type = s_target.addr_type };
    memcpy(a.val, s_target.addr, 6);
    s_state = BMS_BLE_CONNECTING;
    ESP_LOGI(TAG, "connecting to %s", s_target.name[0] ? s_target.name : "BMS");
    int rc = ble_gap_connect(s_own_addr_type, &a, 10000, NULL, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_connect rc=%d", rc);
        s_state = BMS_BLE_ERROR;
        esp_timer_start_once(s_reconnect_t, 5000 * 1000);
    }
}

static void reconnect_cb(void *arg)
{
    (void)arg;
    start_connect();
}

/* ---------------------------------------------------------------- scan */

static void scan_add(const struct ble_gap_disc_desc *d)
{
    if (!s_scan_out) return;
    int i;
    for (i = 0; i < s_scan_n; i++)
        if (memcmp(s_scan_out[i].addr, d->addr.val, 6) == 0) break;
    if (i == s_scan_n) {
        if (s_scan_n >= s_scan_max) return;
        memset(&s_scan_out[i], 0, sizeof s_scan_out[i]);
        memcpy(s_scan_out[i].addr, d->addr.val, 6);
        s_scan_out[i].addr_type = d->addr.type;
        s_scan_n++;
    }
    bms_ble_dev_t *e = &s_scan_out[i];
    e->rssi = d->rssi;

    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof f);
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0) {
        if (f.name && f.name_len && !e->name[0]) {
            size_t n = f.name_len < sizeof e->name - 1 ? f.name_len : sizeof e->name - 1;
            memcpy(e->name, f.name, n);
            e->name[n] = '\0';
        }
        for (int k = 0; k < f.num_uuids16; k++)
            if (ble_uuid_u16(&f.uuids16[k].u) == JK_SVC_UUID) e->is_jk = true;
    }
    if (strncasecmp(e->name, "JK", 2) == 0) e->is_jk = true;
}

static int dev_cmp(const void *x, const void *y)
{
    const bms_ble_dev_t *a = x, *b = y;
    if (a->is_jk != b->is_jk) return a->is_jk ? -1 : 1;
    return b->rssi - a->rssi;
}

int bms_ble_scan(bms_ble_dev_t *out, int max, uint32_t ms)
{
    if (!s_synced || max <= 0) return 0;
    s_scan_out = out;
    s_scan_max = max;
    s_scan_n = 0;
    xSemaphoreTake(s_scan_done, 0);

    bms_ble_state_t prev = s_state;
    if (s_state == BMS_BLE_IDLE || s_state == BMS_BLE_ERROR) s_state = BMS_BLE_SCANNING;

    struct ble_gap_disc_params p = { 0 };
    p.passive = 0;               /* active: scan responses carry most names */
    p.filter_duplicates = 0;
    int rc = ble_gap_disc(s_own_addr_type, (int32_t)ms, &p, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
        s_scan_out = NULL;
        if (s_state == BMS_BLE_SCANNING) s_state = prev;
        return 0;
    }
    xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(ms + 2000));
    int n = s_scan_n;
    s_scan_out = NULL;
    if (s_state == BMS_BLE_SCANNING) s_state = BMS_BLE_IDLE;

    /* keep named or JK-looking devices only */
    int k = 0;
    for (int i = 0; i < n; i++)
        if (out[i].name[0] || out[i].is_jk) out[k++] = out[i];
    qsort(out, k, sizeof out[0], dev_cmp);
    ESP_LOGI(TAG, "scan: %d devices (%d listed)", n, k);
    return k;
}

/* ----------------------------------------------------------- GAP events */

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        scan_add(&ev->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        xSemaphoreGive(s_scan_done);
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_svc_start = s_svc_end = s_notify_handle = s_write_handle = s_cccd = 0;
            s_write_with_rsp = false;
            s_rx_len = 0;
            s_frames = 0;
            s_variant_hint = -1;
            ESP_LOGI(TAG, "connected");
            ble_gattc_exchange_mtu(s_conn, on_mtu, NULL);
            ble_gattc_disc_svc_by_uuid(s_conn, BLE_UUID16_DECLARE(JK_SVC_UUID), on_svc, NULL);
        } else {
            ESP_LOGW(TAG, "connect failed: %d", ev->connect.status);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_state = BMS_BLE_ERROR;
            if (s_have_target) esp_timer_start_once(s_reconnect_t, 5000 * 1000);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason %d)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        esp_timer_stop(s_kick_t);
        s_state = s_have_target ? BMS_BLE_ERROR : BMS_BLE_IDLE;
        if (s_have_target) esp_timer_start_once(s_reconnect_t, 3000 * 1000);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (ev->notify_rx.attr_handle != s_notify_handle) return 0;
        uint8_t buf[260];
        uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
        if (len > sizeof buf) len = sizeof buf;
        os_mbuf_copydata(ev->notify_rx.om, 0, len, buf);
        jk_feed(buf, len);
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        return 0;

    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- host */

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    s_state = BMS_BLE_IDLE;
    ESP_LOGI(TAG, "BLE host synced");
    if (s_have_target) start_connect();
}

static void on_reset(int reason)
{
    s_synced = false;
    s_state = BMS_BLE_OFF;
    ESP_LOGW(TAG, "BLE host reset (%d)", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t bms_ble_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_scan_done = xSemaphoreCreateBinary();

    const esp_timer_create_args_t rc = { .callback = reconnect_cb, .name = "bms_reconn" };
    const esp_timer_create_args_t kc = { .callback = kick_cb, .name = "bms_kick" };
    esp_timer_create(&rc, &s_reconnect_t);
    esp_timer_create(&kc, &s_kick_t);

    load_target();

    esp_hosted_bt_host_stack_cfg_t cfg = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    esp_err_t err = esp_hosted_bt_host_stack_setup(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hosted BT setup failed: %s (does the C6 firmware have BT?)",
                 esp_err_to_name(err));
        s_state = BMS_BLE_ERROR;
        return err;
    }
    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        s_state = BMS_BLE_ERROR;
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_att_set_preferred_mtu(247);
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

esp_err_t bms_ble_connect(const bms_ble_dev_t *dev)
{
    if (!s_synced) return ESP_ERR_INVALID_STATE;
    s_target = *dev;
    s_have_target = true;
    save_target();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);  /* reconnects on disconnect */
    } else {
        ble_gap_conn_cancel();
        start_connect();
    }
    return ESP_OK;
}

void bms_ble_disconnect(void)
{
    s_have_target = false;
    forget_target();
    esp_timer_stop(s_reconnect_t);
    esp_timer_stop(s_kick_t);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    else ble_gap_conn_cancel();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data_us = 0;
    s_frames = 0;
    xSemaphoreGive(s_lock);
    if (s_synced) s_state = BMS_BLE_IDLE;
}

bms_ble_state_t bms_ble_state(char *name, size_t name_len)
{
    if (name && name_len) strlcpy(name, s_have_target ? s_target.name : "", name_len);
    return s_state;
}

bool bms_ble_get(bms_data_t *out, uint32_t *age_ms)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_data_us != 0 && s_frames > 0;
    if (ok) {
        *out = s_data;
        if (age_ms) *age_ms = (uint32_t)((esp_timer_get_time() - s_data_us) / 1000);
    }
    xSemaphoreGive(s_lock);
    return ok;
}

uint32_t bms_ble_frames(void) { return s_frames; }

const char *bms_ble_state_str(bms_ble_state_t s)
{
    switch (s) {
    case BMS_BLE_OFF:        return "BLE off";
    case BMS_BLE_IDLE:       return "not connected";
    case BMS_BLE_SCANNING:   return "scanning...";
    case BMS_BLE_CONNECTING: return "connecting...";
    case BMS_BLE_CONNECTED:  return "connected, waiting for data";
    case BMS_BLE_STREAMING:  return "receiving data";
    case BMS_BLE_ERROR:      return "link error, retrying";
    }
    return "?";
}

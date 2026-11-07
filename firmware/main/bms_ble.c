#include "bms_ble.h"
#include "bms_jk.h"
#include "bms_daly_d2.h"

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
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_hosted_bt_host_stack.h"

static const char *TAG = "bms_ble";

#define JK_SVC_UUID   0xFFE0
#define JK_CHR_UUID   0xFFE1
#define DALY_SVC_UUID 0xFFF0
#define DALY_NTF_UUID 0xFFF1
#define DALY_WR_UUID  0xFFF2
#define CCCD_UUID     0x2902
#define NVS_NS        "bms"

static SemaphoreHandle_t s_lock;
static volatile bms_ble_state_t s_state = BMS_BLE_OFF;
static uint8_t s_own_addr_type;
static volatile bool s_synced;

/* target + connection */
static bms_ble_dev_t s_target;
static uint8_t s_kind;                    /* kind of the connected/target BMS */
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

/* The chosen BMS lives for this session only: every battery has its own BMS,
 * so nothing is auto-connected at boot. Older firmware saved it in NVS -
 * drop that entry so it doesn't linger. */
static void erase_legacy_target(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_erase_key(h, "dev") == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

/* The BMS of the running test is remembered ("bms"/"last") so that after a
 * restart it can be re-joined - but only while the tester reports a running
 * test (main.c decides). "Disconnect" and the end of the test forget it. */
static void save_last(const bms_ble_dev_t *d)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "last", d, sizeof *d);
    nvs_commit(h);
    nvs_close(h);
}

static void clear_last(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_erase_key(h, "last") == ESP_OK) nvs_commit(h);
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

/* link diagnostics (reset when a cell-info frame is accepted) */
static uint32_t s_ntf, s_ntf_bytes, s_ntf_dropped, s_types[4];
static bool s_kicked, s_started;
static int s_small_logged;

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
        /* like the JK app: device info first, then one request for the cell
         * stream (only if it isn't running yet - every command beeps) */
        if (s_data_us < s_link_us && !s_started) {
            s_started = true;
            send_cmd(JK_CMD_CELLINFO);
        }
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

    s_ntf = s_ntf_bytes = s_ntf_dropped = 0;
    memset(s_types, 0, sizeof s_types);
    s_kicked = false;
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
    s_ntf++;
    s_ntf_bytes += n;
    bool hdr = n >= 4 && d[0] == JK_HDR0 && d[1] == JK_HDR1 && d[2] == JK_HDR2 && d[3] == JK_HDR3;
    if (hdr) {
        s_rx_len = 0;
        if (n > 4) s_types[d[4] & 3]++;
    } else if (s_rx_len == 0) {               /* tail of a frame we already used */
        s_ntf_dropped++;
        if (n < 20 && s_small_logged < 12) {  /* what does the JK say when it stops? */
            s_small_logged++;
            char hex[64];
            size_t o = 0;
            for (size_t i = 0; i < n && o + 3 < sizeof hex; i++) o += snprintf(hex + o, sizeof hex - o, "%02x ", d[i]);
            ESP_LOGD(TAG, "small notify (%u B): %s", (unsigned)n, hex);   /* JK: AT<CR><LF> pings */
        }
        return;
    }
    if (s_rx_len + n > sizeof s_rx) { s_rx_len = 0; return; }
    memcpy(s_rx + s_rx_len, d, n);
    s_rx_len += n;
    if (s_rx_len >= JK_FRAME_LEN) {
        handle_frame(s_rx, s_rx_len);
        s_rx_len = 0;
    }
}

/* 16-bit UUID of u: either a 16-bit UUID, or a 128-bit one on the Bluetooth
 * base (0000xxxx-0000-1000-8000-00805F9B34FB) - Daly lists FFF1/FFF2 that way. */
static uint16_t uuid16_of(const ble_uuid_t *u)
{
    if (u->type == BLE_UUID_TYPE_16) return ble_uuid_u16(u);
    if (u->type == BLE_UUID_TYPE_128) {
        static const uint8_t base[12] = { 0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00 };
        const uint8_t *v = ((const ble_uuid128_t *)u)->value;
        if (memcmp(v, base, 12) == 0 && v[14] == 0 && v[15] == 0) return (uint16_t)(v[12] | (v[13] << 8));
    }
    return 0;
}

/* ---------------------------------------------------------- Daly frames */

static void daly_request_status(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_write_handle) return;
    uint8_t f[8];
    daly_d2_build_read(f, sizeof f, 0x0000, DALY_D2_STATUS_REGS);
    int rc = s_write_with_rsp
        ? ble_gattc_write_flat(s_conn, s_write_handle, f, sizeof f, NULL, NULL)
        : ble_gattc_write_no_rsp_flat(s_conn, s_write_handle, f, sizeof f);
    if (rc) ESP_LOGW(TAG, "daly request rc=%d", rc);
}

/* Responses may be split over several notifications (small MTU): collect from
 * the 0xD2 start byte until the length in byte 2 is complete. */
static void daly_feed(const uint8_t *d, size_t n)
{
    s_ntf++;
    s_ntf_bytes += n;
    if (s_rx_len == 0 && (n == 0 || d[0] != DALY_D2_START)) { s_ntf_dropped++; return; }
    if (s_rx_len + n > sizeof s_rx) { s_rx_len = 0; return; }
    memcpy(s_rx + s_rx_len, d, n);
    s_rx_len += n;
    size_t want = daly_d2_frame_len(s_rx, s_rx_len);
    if (s_rx_len >= 3 && !want) { s_rx_len = 0; return; }        /* not a frame we know */
    if (!want || s_rx_len < want) return;

    bms_data_t b;
    daly_d2_err_t e = daly_d2_parse_status(s_rx, want, &b);
    s_rx_len = 0;
    if (e != DALY_D2_OK) {
        ESP_LOGW(TAG, "daly frame (%u B): error %d", (unsigned)want, e);
        return;
    }
    s_ntf = s_ntf_bytes = s_ntf_dropped = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data = b;
    s_data_us = esp_timer_get_time();
    s_frames++;
    uint32_t nf = s_frames;
    xSemaphoreGive(s_lock);
    s_state = BMS_BLE_STREAMING;
    if (nf == 1 || nf % 60 == 0)
        ESP_LOGI(TAG, "Daly: %u cells, %.1f V, %.1f A, SOC %u%%, d=%u mV, %u temps (frame %lu)",
                 b.cell_count, b.pack_mv / 1000.0, b.pack_ma / 1000.0, b.soc_pct, b.imbalance_mv,
                 b.temp_count, (unsigned long)nf);
}

/* --------------------------------------------------------- GATT set-up */

static void kick_cb(void *arg)
{
    (void)arg;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    if (s_kind == BMS_KIND_DALY) {             /* Daly answers each request: poll */
        int64_t last = s_data_us > s_link_us ? s_data_us : s_link_us;
        if (esp_timer_get_time() - last > 15000000) {
            ESP_LOGW(TAG, "Daly: no data for 15 s - reconnecting");
            esp_timer_stop(s_kick_t);
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return;
        }
        s_rx_len = 0;                         /* drop a half frame from the last request */
        daly_request_status();
        return;
    }
    /* JK streams cell info after 0x96; re-request once if it went quiet (every
     * command makes the JK beep), and after 15 s of silence drop the link - the
     * disconnect handler reconnects. */
    int64_t last = s_data_us > s_link_us ? s_data_us : s_link_us;
    int64_t quiet = esp_timer_get_time() - last;
    if (quiet > 15000000) {
        ESP_LOGW(TAG, "no data for %d s - reconnecting (since last frame: %lu notifies, %lu B, "
                 "%lu dropped, types 1:%lu 2:%lu 3:%lu, rx %u)", (int)(quiet / 1000000),
                 (unsigned long)s_ntf, (unsigned long)s_ntf_bytes, (unsigned long)s_ntf_dropped,
                 (unsigned long)s_types[1], (unsigned long)s_types[2], (unsigned long)s_types[3],
                 (unsigned)s_rx_len);
        esp_timer_stop(s_kick_t);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    } else if (quiet > 4000000 && !s_kicked) {
        /* ask once: the JK beeps on every command it gets */
        s_kicked = true;
        ESP_LOGI(TAG, "quiet %d ms (%lu notifies, %lu B, %lu dropped) - re-requesting device + cell info",
                 (int)(quiet / 1000), (unsigned long)s_ntf, (unsigned long)s_ntf_bytes,
                 (unsigned long)s_ntf_dropped);
        /* repeat the connect sequence: device info, then (on its reply) cells */
        s_started = false;
        send_cmd(JK_CMD_DEVINFO);
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
    s_state = BMS_BLE_CONNECTED;
    s_link_us = esp_timer_get_time();
    s_kicked = false;
    s_started = false;
    s_small_logged = 0;
    s_ntf = s_ntf_bytes = s_ntf_dropped = 0;
    esp_timer_stop(s_kick_t);
    if (s_kind == BMS_KIND_DALY) {
        ESP_LOGI(TAG, "notifications on - polling Daly status every 1 s");
        daly_request_status();
        esp_timer_start_periodic(s_kick_t, 1000 * 1000);
    } else {
        ESP_LOGI(TAG, "notifications on - requesting device + cell info");
        send_cmd(JK_CMD_DEVINFO);
        esp_timer_start_periodic(s_kick_t, 1500 * 1000);
    }
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
        uint16_t u = uuid16_of(&chr->uuid.u);
        ESP_LOGI(TAG, "chr %04X @0x%04x props 0x%02x", u, chr->val_handle, p);
        if (s_kind == BMS_KIND_DALY) {          /* FFF1 notifies, FFF2 takes requests */
            if (u == DALY_NTF_UUID && (p & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)))
                s_notify_handle = chr->val_handle;
            if (u == DALY_WR_UUID && (p & (BLE_GATT_CHR_PROP_WRITE_NO_RSP | BLE_GATT_CHR_PROP_WRITE))) {
                s_write_handle = chr->val_handle;
                s_write_with_rsp = !(p & BLE_GATT_CHR_PROP_WRITE_NO_RSP);
            }
            return 0;
        }
        if (u != JK_CHR_UUID) return 0;
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
        ESP_LOGI(TAG, "%s notify @0x%04x, write @0x%04x (%s)", bms_kind_str(s_kind), s_notify_handle, s_write_handle,
                 s_write_with_rsp ? "with response" : "no response");
        int rc = ble_gattc_disc_all_dscs(conn, s_notify_handle, s_svc_end, on_dsc, NULL);
        if (rc) enable_notify(conn);
    } else {
        ESP_LOGW(TAG, "%s characteristics not found (%d)", bms_kind_str(s_kind), err->status);
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
        if (s_kind == BMS_KIND_UNKNOWN) s_kind = (uint8_t)(intptr_t)arg;
        ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, on_chr, NULL);
    } else if ((intptr_t)arg == BMS_KIND_JK && s_kind == BMS_KIND_UNKNOWN) {
        ESP_LOGI(TAG, "no JK service - trying Daly 0xFFF0");
        ble_gattc_disc_svc_by_uuid(conn, BLE_UUID16_DECLARE(DALY_SVC_UUID), on_svc, (void *)(intptr_t)BMS_KIND_DALY);
    } else {
        ESP_LOGW(TAG, "BMS service not found (%d) - unsupported BMS?", err->status);
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
        for (int k = 0; k < f.num_uuids16; k++) {
            uint16_t u = ble_uuid_u16(&f.uuids16[k].u);
            if (u == JK_SVC_UUID) e->kind = BMS_KIND_JK;
            else if (u == DALY_SVC_UUID && e->kind == BMS_KIND_UNKNOWN) e->kind = BMS_KIND_DALY;
        }
    }
    if (strncasecmp(e->name, "JK", 2) == 0) e->kind = BMS_KIND_JK;
    else if (strncasecmp(e->name, "DL-", 3) == 0 || strncasecmp(e->name, "DALY", 4) == 0) e->kind = BMS_KIND_DALY;
}

static int dev_cmp(const void *x, const void *y)
{
    const bms_ble_dev_t *a = x, *b = y;
    bool ka = a->kind != BMS_KIND_UNKNOWN, kb = b->kind != BMS_KIND_UNKNOWN;
    if (ka != kb) return ka ? -1 : 1;           /* recognised BMS first */
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

    /* keep named or recognised devices only */
    int k = 0;
    for (int i = 0; i < n; i++)
        if (out[i].name[0] || out[i].kind != BMS_KIND_UNKNOWN) out[k++] = out[i];
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
            s_kind = s_target.kind;
            if (s_kind == BMS_KIND_DALY)
                ble_gattc_disc_svc_by_uuid(s_conn, BLE_UUID16_DECLARE(DALY_SVC_UUID), on_svc, (void *)(intptr_t)BMS_KIND_DALY);
            else
                ble_gattc_disc_svc_by_uuid(s_conn, BLE_UUID16_DECLARE(JK_SVC_UUID), on_svc, (void *)(intptr_t)BMS_KIND_JK);
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
        if (!s_have_target) {          /* forgotten by the user: drop the link */
            ble_gap_terminate(ev->notify_rx.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        uint8_t buf[260];
        uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
        if (len > sizeof buf) len = sizeof buf;
        os_mbuf_copydata(ev->notify_rx.om, 0, len, buf);
        if (s_kind == BMS_KIND_DALY) daly_feed(buf, len);
        else jk_feed(buf, len);
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "gap: encryption change status %d", ev->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "gap: conn params updated (status %d)", ev->conn_update.status);
        return 0;

    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
        ESP_LOGI(TAG, "gap: peer asks conn params itvl %u-%u lat %u to %u",
                 ev->conn_update_req.peer_params->itvl_min, ev->conn_update_req.peer_params->itvl_max,
                 ev->conn_update_req.peer_params->latency, ev->conn_update_req.peer_params->supervision_timeout);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGW(TAG, "gap: passkey action %d requested", ev->passkey.params.action);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGW(TAG, "gap: repeat pairing");
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        ESP_LOGD(TAG, "gap: event %d", ev->type);
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

    erase_legacy_target();

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
    /* a minimal GATT server so the peer's ATT requests (Exchange MTU right
     * after connecting) get answered - the JK goes silent after the 30 s ATT
     * timeout otherwise */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("BLT-160 bench");
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
    save_last(dev);
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
    clear_last();
    esp_timer_stop(s_reconnect_t);
    esp_timer_stop(s_kick_t);
    int rc;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) rc = ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    else rc = ble_gap_conn_cancel();
    ESP_LOGI(TAG, "disconnect + forget (conn %d): rc=%d", (int)s_conn, rc);
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


const char *bms_kind_str(uint8_t kind)
{
    return kind == BMS_KIND_JK ? "JK" : kind == BMS_KIND_DALY ? "Daly" : "";
}

bool bms_ble_resume_last(void)
{
    if (!s_synced || s_have_target || s_conn != BLE_HS_CONN_HANDLE_NONE) return false;
    nvs_handle_t h;
    bms_ble_dev_t d;
    size_t len = sizeof d;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "last", &d, &len) == ESP_OK && len == sizeof d;
        nvs_close(h);
    }
    if (!ok) return false;
    d.name[sizeof d.name - 1] = '\0';
    ESP_LOGI(TAG, "test running: re-joining the BMS of this test (%s)", d.name);
    s_target = d;
    s_have_target = true;
    start_connect();
    return true;
}

void bms_ble_forget_last(void)
{
    clear_last();
}

uint8_t bms_ble_kind(void)
{
    return s_kind;
}

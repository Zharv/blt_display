#include "netw.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_wifi_remote.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "netw";

#define WIFI_RETRY_MAX 8
#define BIT_CONNECTED  BIT0
#define NVS_NS         "wifi"

static EventGroupHandle_t s_events;
static int s_retry;
static volatile bool s_connected;
static volatile bool s_have_creds;
static char s_ssid[33];
static esp_ip4_addr_t s_ip;

static void save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

static bool load_creds(char *ssid, size_t sl, char *pass, size_t pl)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, "ssid", ssid, &sl) == ESP_OK) &&
              (nvs_get_str(h, "pass", pass, &pl) == ESP_OK) && ssid[0];
    nvs_close(h);
    return ok;
}

static void apply_config(const char *ssid, const char *pass)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, pass, sizeof wc.sta.password);
    strlcpy(s_ssid, ssid, sizeof s_ssid);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_have_creds = (ssid[0] != '\0');
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_creds) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        if (s_have_creds && s_retry < WIFI_RETRY_MAX) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGI(TAG, "retry connect (%d)", s_retry);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_ip = e->ip_info.ip;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&s_ip));
        s_retry = 0;
        s_connected = true;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

void netw_start(void)
{
    s_events = xEventGroupCreate();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_hosted_init();
    esp_hosted_connect_to_slave();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    assert(sta);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* saved creds (NVS) win; else the build-time CONFIG default; else idle */
    char ssid[33] = {0}, pass[65] = {0};
    if (load_creds(ssid, sizeof ssid, pass, sizeof pass)) {
        ESP_LOGI(TAG, "using saved Wi-Fi \"%s\"", ssid);
        apply_config(ssid, pass);
    } else if (strlen(CONFIG_BLT_WIFI_SSID) > 0) {
        apply_config(CONFIG_BLT_WIFI_SSID, CONFIG_BLT_WIFI_PASS);
    } else {
        ESP_LOGW(TAG, "no Wi-Fi saved - set it on the Settings screen");
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    mdns_init();
    mdns_hostname_set("blt-display");
}

bool netw_is_connected(void) { return s_connected; }

bool netw_wait_connected(uint32_t timeout_ms)
{
    if (!s_events) return false;
    EventBits_t b = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(timeout_ms));
    return (b & BIT_CONNECTED) != 0;
}

int netw_scan(netw_ap_t *out, int max)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) return 0;
    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (!recs) return 0;
    esp_wifi_scan_get_ap_records(&num, recs);

    int n = 0;
    for (int i = 0; i < num && n < max; i++) {
        /* skip duplicate SSIDs (keep the strongest, which comes first) */
        bool dup = false;
        for (int j = 0; j < n; j++)
            if (strcmp(out[j].ssid, (char *)recs[i].ssid) == 0) { dup = true; break; }
        if (dup || recs[i].ssid[0] == '\0') continue;
        strlcpy(out[n].ssid, (char *)recs[i].ssid, sizeof out[n].ssid);
        out[n].rssi = recs[i].rssi;
        out[n].secure = (recs[i].authmode != WIFI_AUTH_OPEN);
        n++;
    }
    free(recs);
    return n;
}

void netw_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return;
    save_creds(ssid, pass ? pass : "");
    s_retry = 0;
    apply_config(ssid, pass ? pass : "");
    esp_wifi_disconnect();
    esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
}

void netw_get_ip_str(char *buf, size_t n)
{
    if (s_connected) snprintf(buf, n, IPSTR, IP2STR(&s_ip));
    else if (n) buf[0] = '\0';
}

void netw_get_ssid(char *buf, size_t n) { strlcpy(buf, s_ssid, n); }

int netw_scan_printers(netw_printer_t *out, int max)
{
    mdns_result_t *res = NULL;
    uint8_t want = (uint8_t)(max > 20 ? 20 : max);
    if (mdns_query_ptr("_ipp", "_tcp", 3000, want, &res) != ESP_OK || res == NULL) {
        if (res) mdns_query_results_free(res);
        return 0;
    }
    int n = 0;
    for (mdns_result_t *r = res; r != NULL && n < max; r = r->next) {
        if (!r->addr) continue;
        snprintf(out[n].ip, sizeof out[n].ip, IPSTR, IP2STR(&r->addr->addr.u_addr.ip4));
        out[n].port = r->port ? r->port : 631;
        strlcpy(out[n].name, r->instance_name ? r->instance_name : out[n].ip, sizeof out[n].name);
        strlcpy(out[n].resource, "/ipp/print", sizeof out[n].resource);
        for (size_t i = 0; i < r->txt_count; i++) {   /* TXT "rp" = resource path */
            if (r->txt[i].key && strcmp(r->txt[i].key, "rp") == 0 && r->txt[i].value) {
                snprintf(out[n].resource, sizeof out[n].resource, "/%s", r->txt[i].value);
            }
        }
        n++;
    }
    mdns_query_results_free(res);
    return n;
}

void netw_set_printer(const netw_printer_t *p)
{
    nvs_handle_t h;
    if (nvs_open("printer", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ip", p->ip);
    nvs_set_u16(h, "port", p->port);
    nvs_set_str(h, "res", p->resource);
    nvs_set_str(h, "name", p->name);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "printer saved: %s %s:%u%s", p->name, p->ip, (unsigned)p->port, p->resource);
}

bool netw_get_printer(netw_printer_t *p)
{
    nvs_handle_t h;
    if (nvs_open("printer", NVS_READONLY, &h) != ESP_OK) return false;
    size_t il = sizeof p->ip, rl = sizeof p->resource, nl = sizeof p->name;
    bool ok = nvs_get_str(h, "ip", p->ip, &il) == ESP_OK && p->ip[0];
    if (nvs_get_u16(h, "port", &p->port) != ESP_OK) p->port = 631;
    if (nvs_get_str(h, "res", p->resource, &rl) != ESP_OK) strlcpy(p->resource, "/ipp/print", sizeof p->resource);
    if (nvs_get_str(h, "name", p->name, &nl) != ESP_OK) p->name[0] = '\0';
    nvs_close(h);
    return ok;
}

bool netw_resolve_printer(char *host, size_t host_len, uint16_t *port,
                          char *resource, size_t res_len)
{
    /* saved selection wins */
    netw_printer_t saved;
    if (netw_get_printer(&saved)) {
        strlcpy(host, saved.ip, host_len);
        strlcpy(resource, saved.resource, res_len);
        *port = saved.port;
        return true;
    }

    strlcpy(resource, CONFIG_BLT_PRINTER_RESOURCE, res_len);
    *port = (uint16_t)CONFIG_BLT_PRINTER_PORT;
    if (strlen(CONFIG_BLT_PRINTER_HOST) > 0) {
        strlcpy(host, CONFIG_BLT_PRINTER_HOST, host_len);
        return true;
    }

    /* else first mDNS responder */
    netw_printer_t found[1];
    if (netw_scan_printers(found, 1) >= 1) {
        strlcpy(host, found[0].ip, host_len);
        strlcpy(resource, found[0].resource, res_len);
        *port = found[0].port;
        return true;
    }
    ESP_LOGW(TAG, "no _ipp._tcp printer found via mDNS");
    return false;
}

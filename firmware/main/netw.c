#include "netw.h"
#include "cp_update.h"

#include <string.h>
#include <stdlib.h>

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/etharp.h"
#include <errno.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
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
#include "freertos/task.h"

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
static esp_ip4_addr_t s_gw;
static bool s_diag_done;

/* Non-blocking TCP connect with a timeout; logs reachability + error detail. */
static bool conntest(const char *ip, uint16_t port, int timeout_ms)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { ESP_LOGW(TAG, "    socket() errno=%d", errno); return false; }
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(ip);
    bool ok = false;
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    int cerr = (r == 0) ? 0 : errno;
    int sel = -2, soerr = -1;
    if (r == 0) {
        ok = true;
    } else if (cerr == EINPROGRESS) {
        fd_set ws; FD_ZERO(&ws); FD_SET(s, &ws);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        sel = select(s + 1, NULL, &ws, NULL, &tv);
        if (sel > 0) {
            socklen_t el = sizeof soerr;
            getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &el);
            ok = (soerr == 0);
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "    %s:%u connect r=%d errno=%d(%s) select=%d so_error=%d(%s)",
                 ip, (unsigned)port, r, cerr, strerror(cerr), sel,
                 soerr, soerr > 0 ? strerror(soerr) : "-");
    }
    close(s);
    return ok;
}

static void dump_arp(void)
{
    for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t *ip; struct netif *nif; struct eth_addr *eth;
        if (etharp_get_entry(i, &ip, &nif, &eth)) {
            ESP_LOGW(TAG, "    arp %-15s -> %02x:%02x:%02x:%02x:%02x:%02x", ip4addr_ntoa(ip),
                     eth->addr[0], eth->addr[1], eth->addr[2],
                     eth->addr[3], eth->addr[4], eth->addr[5]);
        }
    }
}

static void diag_task(void *arg)
{
    (void)arg;
    char gw[16];
    snprintf(gw, sizeof gw, IPSTR, IP2STR(&s_gw));
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGW(TAG, "connectivity test (gw=%s, my mac=%02x:%02x:%02x:%02x:%02x:%02x):", gw,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        uint8_t proto = 0;
        esp_wifi_get_protocol(WIFI_IF_STA, &proto);
        ESP_LOGW(TAG, "  assoc: rssi=%d auth=%d pair=%d group=%d ap[b%d g%d n%d ax%d] sta_proto=0x%02x",
                 ap.rssi, ap.authmode, ap.pairwise_cipher, ap.group_cipher,
                 ap.phy_11b, ap.phy_11g, ap.phy_11n, ap.phy_11ax, proto);
        ESP_LOGW(TAG, "  bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%d",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5],
                 ap.primary);
    }
    ESP_LOGW(TAG, "  gateway   %s:80   -> %s", gw, conntest(gw, 80, 4000) ? "OK" : "FAIL");
    ESP_LOGW(TAG, "  internet  8.8.8.8:53 -> %s", conntest("8.8.8.8", 53, 4000) ? "OK" : "FAIL");
    char ph[64], pres[64];
    uint16_t pport = 631;
    if (netw_resolve_printer(ph, sizeof ph, &pport, pres, sizeof pres)) {
        ESP_LOGW(TAG, "  printer   %s:%u -> %s", ph, (unsigned)pport,
                 conntest(ph, pport, 4000) ? "OK" : "FAIL");
    } else {
        ESP_LOGW(TAG, "  printer   (none configured)");
    }
    ESP_LOGW(TAG, "  PC        192.168.0.108:445 -> %s",
             conntest("192.168.0.108", 445, 4000) ? "OK" : "FAIL");
    ESP_LOGW(TAG, "  arp table:");
    dump_arp();
    vTaskDelete(NULL);
}

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
        s_gw = e->ip_info.gw;
        ESP_LOGI(TAG, "got ip: " IPSTR " gw: " IPSTR, IP2STR(&s_ip), IP2STR(&s_gw));
        s_retry = 0;
        s_connected = true;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        if (!s_diag_done) {
            s_diag_done = true;
            xTaskCreate(diag_task, "netdiag", 4096, NULL, 4, NULL);
        }
    }
}

bool netw_start(void)
{
    s_events = xEventGroupCreate();
    ESP_LOGI(TAG, "heap before hosted: internal %u (largest %u), dma %u, psram %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_hosted_init();
    esp_hosted_connect_to_slave();

    /* bring the C6 co-processor firmware in line with the host (may restart) */
    cp_update_if_needed();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    assert(sta);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t werr = esp_wifi_init(&cfg);
    if (werr != ESP_OK) {
        /* C6 not answering: keep the UI running without Wi-Fi instead of
         * rebooting in a loop */
        ESP_LOGE(TAG, "esp_wifi_init failed (%s) - Wi-Fi unavailable", esp_err_to_name(werr));
        return false;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* Keep the C6 on 802.11b/g/n: the bench AP is 11n-only anyway, and this
     * keeps association identical to the other clients when debugging. */
    esp_err_t perr = esp_wifi_set_protocol(WIFI_IF_STA,
                                           WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    ESP_LOGI(TAG, "sta protocol 11b/g/n: %s", esp_err_to_name(perr));

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

    /* Modem-sleep makes the STA miss broadcast/multicast frames (ARP requests,
     * mDNS) - peers then can't reach us. This is a mains-powered bench device,
     * so keep the radio awake. */
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "wifi power save off: %s", esp_err_to_name(ps));

    mdns_init();
    mdns_hostname_set("blt-display");
    return true;
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
    /* On a printer's own Wi-Fi Direct network ("DIRECT-xx-..."), the printer is
     * the access point, i.e. our gateway. */
    if (s_connected && strncmp(s_ssid, "DIRECT-", 7) == 0) {
        snprintf(host, host_len, IPSTR, IP2STR(&s_gw));
        strlcpy(resource, "/ipp/print", res_len);
        *port = 631;
        ESP_LOGI(TAG, "Wi-Fi Direct: printer = gateway %s", host);
        return true;
    }

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

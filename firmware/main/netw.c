#include "netw.h"

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_wifi_remote.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "netw";

#define WIFI_RETRY_MAX 10
#define BIT_CONNECTED  BIT0

static EventGroupHandle_t s_events;
static int s_retry;
static volatile bool s_connected;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        if (s_retry < WIFI_RETRY_MAX) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGI(TAG, "retry connect (%d)", s_retry);
        } else {
            ESP_LOGW(TAG, "connect failed");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
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

    /* bring up the C6 co-processor */
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

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_BLT_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_BLT_WIFI_PASS, sizeof wc.sta.password);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (strlen(CONFIG_BLT_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "CONFIG_BLT_WIFI_SSID is empty - set it in sdkconfig.credentials");
    } else {
        ESP_LOGI(TAG, "connecting to \"%s\" via ESP32-C6", CONFIG_BLT_WIFI_SSID);
    }

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

bool netw_resolve_printer(char *host, size_t host_len, uint16_t *port,
                          char *resource, size_t res_len)
{
    strlcpy(resource, CONFIG_BLT_PRINTER_RESOURCE, res_len);
    *port = (uint16_t)CONFIG_BLT_PRINTER_PORT;

    if (strlen(CONFIG_BLT_PRINTER_HOST) > 0) {
        strlcpy(host, CONFIG_BLT_PRINTER_HOST, host_len);
        return true;
    }

    /* mDNS: first _ipp._tcp responder */
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_ipp", "_tcp", 3000, 20, &results);
    if (err != ESP_OK || results == NULL) {
        ESP_LOGW(TAG, "no _ipp._tcp printer found via mDNS");
        if (results) mdns_query_results_free(results);
        return false;
    }
    bool ok = false;
    for (mdns_result_t *r = results; r != NULL; r = r->next) {
        if (r->addr) {
            snprintf(host, host_len, IPSTR, IP2STR(&r->addr->addr.u_addr.ip4));
            if (r->port) *port = r->port;
            ESP_LOGI(TAG, "printer %s at %s:%u", r->instance_name ? r->instance_name : "?",
                     host, (unsigned)*port);
            ok = true;
            break;
        }
    }
    mdns_query_results_free(results);
    return ok;
}

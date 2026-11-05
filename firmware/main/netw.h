/*
 * Wi-Fi (via the on-board ESP32-C6 / ESP-Hosted) + mDNS printer discovery.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up NVS, ESP-Hosted, Wi-Fi STA. Connects if credentials are saved in NVS
 * (or set via CONFIG_BLT_WIFI_SSID); otherwise idles until netw_connect().
 * Returns false if the C6 did not come up (no Wi-Fi/BLE this boot). */
bool netw_start(void);

bool netw_is_connected(void);
bool netw_wait_connected(uint32_t timeout_ms);

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;
} netw_ap_t;

/* Scan for access points (blocking ~2 s). Returns the count written to out. */
int  netw_scan(netw_ap_t *out, int max);

/* Save credentials to NVS and (re)connect. */
void netw_connect(const char *ssid, const char *pass);

/* Current IP as a string, or "" when not connected. */
void netw_get_ip_str(char *buf, size_t n);

/* SSID currently targeted (from NVS/last connect), "" if none. */
void netw_get_ssid(char *buf, size_t n);

typedef struct {
    char     name[64];     /* mDNS instance name, for display */
    char     ip[16];
    uint16_t port;
    char     resource[48]; /* IPP resource path, e.g. "/ipp/print" */
} netw_printer_t;

/* Discover IPP printers (_ipp._tcp) via mDNS (blocking ~3 s). Returns count. */
int  netw_scan_printers(netw_printer_t *out, int max);

/* Persist the chosen printer to NVS (used by printing). */
void netw_set_printer(const netw_printer_t *p);

/* Load the saved printer; returns false if none saved. */
bool netw_get_printer(netw_printer_t *p);

/*
 * Resolve the printer. If CONFIG_BLT_PRINTER_HOST is set it is used directly;
 * otherwise an mDNS query for _ipp._tcp picks the first responder.
 * Fills host (IP string), port and resource path. Returns true on success.
 */
bool netw_resolve_printer(char *host, size_t host_len, uint16_t *port,
                          char *resource, size_t res_len);

#ifdef __cplusplus
}
#endif

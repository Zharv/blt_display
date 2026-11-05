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

/* Bring up NVS, ESP-Hosted, Wi-Fi STA and start connecting (non-blocking). */
void netw_start(void);

bool netw_is_connected(void);
bool netw_wait_connected(uint32_t timeout_ms);

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

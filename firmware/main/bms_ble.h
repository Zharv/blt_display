/*
 * BLE link to a battery BMS (NimBLE central on the P4; the radio is the C6,
 * reached over ESP-Hosted HCI). JK (Jikong, service 0xFFE0, streams after one
 * request) and Daly (service 0xFFF0, "D2" Modbus protocol, polled every second)
 * are implemented; the kind is detected from the advertisement.
 *
 * Threading: scan/connect are called from UI/worker tasks; results and data
 * snapshots are copied out under a lock, so callers never touch NimBLE state.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "bms.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { BMS_KIND_UNKNOWN = 0, BMS_KIND_JK, BMS_KIND_DALY } bms_kind_t;

typedef struct {
    char    name[32];
    uint8_t addr[6];       /* little-endian, as NimBLE stores it */
    uint8_t addr_type;
    int8_t  rssi;
    uint8_t kind;          /* bms_kind_t: from the advertised service / name */
} bms_ble_dev_t;

/* "JK", "Daly" or "" */
const char *bms_kind_str(uint8_t kind);

typedef enum {
    BMS_BLE_OFF = 0,       /* stack not up (yet) */
    BMS_BLE_IDLE,          /* ready, not connected */
    BMS_BLE_SCANNING,
    BMS_BLE_CONNECTING,
    BMS_BLE_CONNECTED,     /* GATT set up, waiting for data */
    BMS_BLE_STREAMING,     /* cell-info frames arriving */
    BMS_BLE_ERROR,
} bms_ble_state_t;

/* Bring up the hosted BT controller + NimBLE. Call after
 * esp_hosted_connect_to_slave(). Nothing is auto-connected at boot. */
esp_err_t bms_ble_start(void);

/* Blocking active scan for `ms`; returns devices found (JK ones first). */
int bms_ble_scan(bms_ble_dev_t *out, int max, uint32_t ms);

/* Connect to dev for this session (re-connects if the link drops). Returns at once. */
esp_err_t bms_ble_connect(const bms_ble_dev_t *dev);

/* Drop the link and end the session (no more re-connects). */
void bms_ble_disconnect(void);

/* Current state; name (may be NULL) gets the connected/target device name. */
bms_ble_state_t bms_ble_state(char *name, size_t name_len);

/* Latest decoded snapshot. Returns false if none yet; age_ms = time since it. */
bool bms_ble_get(bms_data_t *out, uint32_t *age_ms);

/* Number of cell-info frames decoded since connect. */
uint32_t bms_ble_frames(void);

const char *bms_ble_state_str(bms_ble_state_t s);

#ifdef __cplusplus
}
#endif

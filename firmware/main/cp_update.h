/*
 * One-shot update of the ESP32-C6 co-processor (ESP-Hosted slave) firmware
 * from an image stored in the P4's own flash ("storage" partition).
 *
 * Image layout in the partition (made by tools/make_cpfw.py):
 *   "CPFW" | u32 len | u32 crc32(image) | u32 reserved | image[len]
 *
 * Call after esp_hosted_connect_to_slave() and before Wi-Fi init. If the C6
 * runs an older firmware and this exact image has not been applied yet, it is
 * streamed over the OTA RPCs and the P4 restarts (which also resets the C6).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns ESP_OK if nothing to do; does not return if an update was applied. */
esp_err_t cp_update_if_needed(void);

#ifdef __cplusplus
}
#endif

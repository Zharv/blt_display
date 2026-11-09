/*
 * BLT tester over USB: the P4 is the USB host, the tester is a Microchip
 * CDC-ACM device (VID 04D8 PID 000A).
 *
 * SAFETY: the tester may be running a real battery test. By default this
 * module is PASSIVE - it only enumerates the device, prints its descriptors
 * and logs any bytes the tester sends on its own. It never sends line coding,
 * DTR/RTS or a protocol frame by itself. Read commands (RI within the 192-byte
 * live block, RD, RB; <= 64 B per frame like BltView) are only sent by
 * blt_usb_read() after blt_usb_allow_reads(true); everything else is refused
 * unconditionally: WD/WC/GG, RE (reset/action/reboot) and RW/CI/CP (unknown to
 * the firmware, which then repeats the previous command type).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "blt_proto.h"
#include "blt_live.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { BLT_USB_OFF = 0, BLT_USB_WAITING, BLT_USB_CONNECTED } blt_usb_state_t;

/* Start the USB host + CDC-ACM driver and wait for the tester (passive). */
esp_err_t blt_usb_start(void);

/* Link state; vid/pid of the connected device (may be NULL). */
blt_usb_state_t blt_usb_state(uint16_t *vid, uint16_t *pid);

/* Explicit permission for read commands (off at boot). */
void blt_usb_allow_reads(bool on);
bool blt_usb_reads_allowed(void);

/*
 * One read transaction. Refuses (ESP_ERR_NOT_ALLOWED) any command that is not
 * RI/RD/RB, and any command while reads are not allowed. The raw response
 * line is logged. out may be NULL (raw log only). Returns ESP_OK on a valid
 * response, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE (see *perr) otherwise.
 */
esp_err_t blt_usb_read(blt_cmd_t cmd, uint32_t addr, uint16_t len, blt_response_t *out,
                       blt_err_t *perr, uint32_t timeout_ms);

/* Read the 192-byte live block (RI 0x00/0x40/0x80, 64 B each, as BltView
 * does) and decode it. Same permission rules as blt_usb_read(). */
esp_err_t blt_usb_read_live(blt_live_t *out);

/* Read len bytes with RI/RD/RB in 64-byte frames (quiet, 2 tries per frame). */
esp_err_t blt_usb_read_block(blt_cmd_t cmd, uint32_t addr, uint8_t *dst, uint16_t len);

/* The only write path: arm once, then write one sealed block - the battery-ID
 * block (0x40, 50 B) or the settings block (0x100, 62 B). Used by blt_prepare
 * only, after it has checked that the tester is in START. */
void blt_usb_arm_write(void);
esp_err_t blt_usb_write_block(uint32_t addr, const uint8_t *data, uint16_t len);

/* RE 0x03 "apply after write": the tester reloads settings + battery ID from
 * EEPROM into RAM. The only action command the panel may send (user's OK,
 * 2026-11-09); arm once, used by blt_prepare in START only. */
void blt_usb_arm_apply(void);
esp_err_t blt_usb_apply_after_write(void);

#ifdef __cplusplus
}
#endif

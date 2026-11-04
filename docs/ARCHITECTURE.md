# blt-esp-display — architecture

A standalone touch-panel test bench for **Alfa Progetti BLT160 / SmartIC** traction-battery
testers. It re-implements the useful parts of the Windows **BLTView** software, adapted for a
touchscreen, and additionally pairs with the battery's **BMS over Bluetooth LE** so one screen
shows tester *and* BMS data together, with a discharge graph, temperatures and cell imbalance —
and can **print the result + discharge graph to a Wi-Fi printer**.

Replaces: a Windows PC running BLTView + an Access database.

## Hardware

- **Board:** Guition **JC8012P4A1** — ESP32-P4 (dual RISC-V, PSRAM), 10.1" 1280×800 MIPI-DSI
  (JD9365), capacitive touch GSL3680 (I2C), microSD, RX8025T RTC, ES8311 audio. BSP:
  [profi-max/JC8012P4A1_BSP_ESP32P4](https://github.com/profi-max/JC8012P4A1_BSP_ESP32P4),
  LVGL 9.x + PPA.
- **Radio:** ESP32-P4 has no radio of its own. Wi-Fi + BLE come from the on-board **ESP32-C6**
  via **ESP-Hosted-MCU** over SDIO. The P4 runs the BLE *host* (NimBLE) over virtual-HCI; the C6
  is the controller. Needs `esp_hosted ≥ 2.4.2`, `esp_wifi_remote ≥ 1.0.0`, IDF ≥ 5.3.2. BLE 4.2.
- **Tester link:** USB. The tester is a Microchip CDC-ACM device (VID 0x04D8 / PID 0x000A, the
  `SmartUsb.inf` device). The P4 is **USB host** on its USB 2.0 OTG (HS) port; USB-Serial-JTAG
  (the flashing/console port) is separate and stays free.

### Hardware caveats to verify on the real board
1. The C6's ESP-Hosted *slave* firmware must be built **with BT enabled** (not Wi-Fi-only) —
   likely a reflash of the C6.
2. USB host needs the OTG connector wired for host and able to supply (or tolerate) VBUS; check
   `Demos_and_Docs/5-Schematic`.
3. Printer (HP Smart Tank 581) document format — confirm `image/urf` vs PDF via its EWS / `ipptool`.

## Data flow

```
        USB-CDC (host)                     BLE (NimBLE host on P4, radio on C6)
 Tester ───────────────► ESP32-P4 ◄───────────────────────────── BMS (JK/JBD/Daly)
  BLT160/SmartIC         │  session model + SQLite on microSD
                         │
                         ├─► LVGL UI (Tester | BMS, live discharge graph)
                         └─► Wi-Fi ─IPP─► HP Smart Tank 581  (report + graph, image/urf)
```

One test = **one battery**: a single tester channel + a single BLE connection.

## Components (`firmware/components/`, pure C, host-testable)

| Component  | Role | Status |
|---|---|---|
| `blt_proto` | BLT160/SmartIC frame build + response validate/parse (see docs/BLT_PROTOCOL.md). No IDF deps. | **done, host-tested (40/0)**, verified vs BLTView + 2 known frames |
| `bms`       | Vendor-neutral `bms_data_t` + drivers `bms_jbd` (done), `bms_jk`, `bms_daly` (TODO). | JBD done, host-tested (19/0); fields pending a hardware capture |
| `report`    | Render summary + V/A discharge graph + cell/temp/imbalance to an offscreen canvas (screen preview *and* print raster). | TODO |
| `print_ipp` | Minimal IPP client: mDNS `_ipp._tcp` discovery, Get-Printer-Attributes, Print-Job with `image/urf`. | TODO |

## `firmware/main/` (IDF app)

- `usb_cdc` — USB-host CDC-ACM transport (`espressif/usb_host_cdc_acm`), opens VID/PID, line coding, half-duplex send/recv for `blt_proto`.
- `ble_bms` — NimBLE central: scan/connect one BMS, per-vendor GATT (notify char), feed bytes to the `bms` driver.
- `session` — test-session state machine, merges tester + BMS samples into a time series.
- `storage` — SQLite on microSD (clients, batteries, cycles, graph samples) ≈ BLTView's TabClienti/TabOld/TabGrafico.
- `ui` — LVGL screens: OnLine (live), Archive (stored cycles), Client/Battery, Settings.
- `netw` — Wi-Fi via `esp_wifi_remote`, mDNS, print trigger.

## UI screens (touch-first; BLTView's forms re-imagined)

1. **OnLine** — live test: large V/A/°C, discharge graph, BMS cell bars with imbalance highlight, Start/Stop.
2. **Archive** — cycles downloaded from the tester's recorder; tap → curve + summary.
3. **Client / Battery** — pick/associate (finger-sized lists).
4. **Settings** — USB/serial params, node, BLE pairing, printer, calibration.

## Testing strategy

- **Host unit tests** (`tests/`, MSVC `cl.exe` or any C compiler): every parser against recorded/spec
  frame fixtures. `blt_proto` is cross-checked against an independent Python implementation
  (`tools/blt_client.py`) and the two hand-verified frames; JBD against the public read-basic frame.
- **Simulators** (`tools/`): `blt_sim.py` replays a synthetic discharge over the real framing so the
  client/session/UI can be developed without hardware. A BLE-BMS simulator is TODO.
- **On-target smoke tests**: USB enumerate, BLE connect, one IPP job.

## Open / not reverse-engineered

- The tester's on-device **register map** (which RD/RE/RB addresses hold live values, the recorder
  layout, the EEPROM cycle-record format) lives in the PIC18 firmware (AP211 v1.36) and is **not**
  decoded yet. `blt_sim.py`'s map is a development placeholder. Decoding it (or capturing BLTView
  traffic) is the next protocol milestone.
- `blt_proto` command *framing* is complete and verified; command *semantics* (what each address
  returns) are not.

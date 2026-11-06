# BLT160 register / command map (as used by BltView.exe 2.6.16)

Source: static RE of `BltView.exe` (Delphi). Function addresses are Ghidra VAs (image base 0x400000).
Wire framing, checksum and command letters are in `BLT_PROTOCOL.md`; this file covers **what BLTView
sends and how it decodes the replies**.
Confidence tags: **[proven]** = read directly from decompiled code / disassembly;
**[inferred]** = deduced from UI label names, units or by analogy. Treat inferred items as unverified.

---

## 0. Transport facts that matter for the ESP32

* **Chunking [proven]**: every read goes through `FUN_005d8530`, which splits it into blocks of
  `(DAT_005e8628 - 0x10)/2 = (0x90-0x10)/2 = 64` bytes and sends **one frame per 64-byte block**,
  incrementing the address by 64 each time (`local_10 = local_10 + local_28`). So the 192-byte live read
  is three frames: `RI` addr 0x00, 0x40, 0x80, each with LLLL=0x0040. `DAT_005e8628` is only read, never written.
* **Expected reply length [proven]** (`FUN_005d8480`): BLTView's `respLen` argument (Ghidra's `param_5` of
  8530, `param_4` of 8480) sets how many payload bytes the reply must carry: `0` means "the same as the requested
  length" (reads); `1` means a 1-byte ack (writes/commands). The reply must be exactly `2*N+15` chars
  (`FUN_005d81c8`). Two attempts per frame. `CI`/`CP` replies are not validated.
* **Byte order [proven]**: payload hex is decoded byte-by-byte into a buffer (`FUN_005d7bf0`); the app then reads
  16/32-bit fields straight from that buffer on x86, so **multi-byte fields are little-endian** (low byte first
  on the wire). Proven unsigned for RI+2/4/6/8 (`MOVZX word ptr` at 0x5cbb49, 0x5cbb71, 0x5cbbae, 0x5cc05c).
  Exception: the debug `GG` reply is decoded big-endian (`b5b + b5c*0x100`, `FUN_005d1940`).
* **Write blocks carry their own checksum [proven]** (`FUN_005d9928`, `FUN_005d998c`): before `WD`, byte[0] is
  set to `0xA5` and the last byte is the XOR of all preceding bytes (block 0x40: XOR of bytes 0..0x30 stored at 0x31;
  block 0x100: XOR of bytes 0..0x3C stored at 0x3D).
* **Serial line [proven]**:
  * Settings live in `CONFIG_BLT.BIN` (record `DAT_00fa1780`, 0xD3B bytes; load `FUN_005dab20`, save `FUN_005daa9c`).
    `TFMainForm.FormShow` (0x5e2324..0x5e2351) writes 9600 baud (idx 5), no parity, 8 data bits (idx 4) and 1 stop
    bit. It then calls the loader, which **zero-fills the whole record first** and overwrites those values.
    No UI code writes them back: `TFSeriale.BitBtn1Click` 0x5ddaa0 stores only the COM number (+0x12E, 0=Auto) and
    the "SmartUSB" flag. `CBVelocita/CBParita/CBDati/CBStop` are never read.
  * `SetCommState` runs only if `cfg+0xD27 == 1` (`FUN_005d7890`). `+0xD27 = 0` when the **"SmartUSB"**
    checkbox (`CB160uir`) is ticked, and also on a fresh install, where the record is zero.
    **For the USB connection, BLTView does not set baud or parity at all.** The CDC port keeps whatever DCB
    the driver has. Any baud works on a CDC-ACM device.
  * Timeouts: `ReadTotalTimeoutMultiplier = 0x9C4+1 = 2501 ms/byte` and `WriteTotalTimeoutConstant = 1000 ms`.
    The other fields keep the driver defaults. `PurgeComm(TXCLEAR)` runs before every write. The port stays open
    between transactions and is closed on probe/disconnect (`FUN_005d7874`).
* **Node** is hard-coded 0x63 (`FUN_005d8480`: `FUN_005d7e8c(99, …)`). Separately, `*PTR_DAT_005e8b4c`
  (`DAT_00f72f3c`) holds the **COM port number**. The value 99 means "offline/demo, no comms": it is set if a key
  is held while clicking OnLine (`TFMainForm.BOnLineClick`/`TFFindSmartic.FormActivate`).

---

## 1. All call sites, grouped by feature

`FUN_005d8480` has a single caller, `FUN_005d8530`. The table lists every call to `FUN_005d8530`, found by a raw
`E8` call-scan of the CODE section (31 sites), plus the probe wrapper `FUN_005d8684`.
"len" is the total payload length; reads over 64 bytes are split into 64-byte frames.

| feature | wrapper (VA) | cmd | addr | len | respLen | dest buffer / data | triggered from |
|---|---|---|---|---|---|---|---|
| **Identify / probe** | `FUN_005d8684` | RE (2) | 0x69 | 0 | 1 | reply chars [5..6]: `"11"`→type 1 "SmarticI Plus", `"22"`→type 2 "BLT 160", else type 2 | `TFFindSmartic.FormActivate` 0x5dbd30 (per COM port in Auto mode); main-form comm test |
| Connect: serial no. | `FUN_005d9c20` | RD (1) | 0x10 | 0x10 | 0 | `0xf73590` (16 ASCII, "Matricola") | `TFOnLine.FormShow` 0x5c3bdd |
| Connect: FW version | `FUN_005d9b9c` | RD | 0x20 | 0x10 | 0 | `0xf735b1` (16 ASCII → `STMONVers`) | FormShow 0x5c3be2 |
| Connect / every 10th poll: settings | `FUN_005d9768` | RD | 0x100 | 0x3E | 0 | `0xf73414` = CFG block (§4) | FormShow, poll, "Rileggi dati" |
| Connect / every 10th poll: battery ID | `FUN_005d9710` | RD | 0x40 | 0x32 | 0 | `0xf73534` = ID block (§4) | FormShow, poll, after write |
| **LIVE poll** | `FUN_005d9680` | **RI (0)** | **0x000** | **0xC0** (3 frames ×64) | 0 | `0xf731e8` = RI block (§3) | poll `FUN_005c5c4c` 0x5c5d06 |
| GSM/modem status | `FUN_005d963c` | RW (7) | 0 | 0x10 | 0 | `0xf730d8` ([0] modem state, [1] substate, [2] CSQ rssi, [3] CSQ ber) | poll, only while tab `TSComunicazioni` is active |
| Parameter lock flags | `FUN_005d98d4` | RB (9) | 0 | 0x1E | 0 | `0xf734d4`, 1 byte per setting, 0 = editable | "Lettura parametri" (flag +0x1dab) |
| Write settings | `FUN_005d998c` | WD (3) | 0x100 | 0x3E | 1 | CFG block, A5 + XOR | `BInviaDatiClick` 0x5c94eb |
| Write battery ID | `FUN_005d9928` | WD | 0x40 | 0x32 | 1 | ID block, A5 + XOR | `BInviaDatiClick` 0x5c9561 |
| Apply after write [inferred] | `FUN_005d95b8` | RE | 0x03 | 0 | 1 | – | `BInviaDatiClick` 0x5c956b |
| Write serial no. | `FUN_005d9bf4` | WD | 0x10 | 0x10 | 1 | `0xf73590` | `BProgrammaMatricolaClick` → poll flag +0x1db8 |
| **Set clock** | `FUN_005d9a80` | WC (4) | 0 | 7 | 1 | BCD `ss mm hh dow DD MM YY` (dow = Delphi DayOfWeek, 1=Sunday) | `BSetOrologioClick` → poll flag +0x1daf |
| Reset statistics | `FUN_005d9354` | RE | 0x00 | 0 | 1 | – | `BResetStatisticheClick` |
| Reset cycles | `FUN_005d937c` | RE | 0x01 | 0 | 1 | – | `BResetCicliClick` |
| Reset graph | `FUN_005d9560` | RE | 0x02 | 0 | 1 | – | `BResetGraficoClick` |
| Reset graph pointer | `FUN_005d9534` | RE | 0x70 | 0 | 1 | – | `BResetProgresGraficoClick` (then RI) |
| Freeze/prepare download [inferred] | `FUN_005d958c` | RE | 0x0C | 0 | 1 | sent only if FW date ≥ 23/06/2009 | `BMemorizzaOldClick`, `TGraficoAUTOTimer` |
| "Allinea SmartIC" | `FUN_005d95e4` | RE | 0x0A | 4 | 1 | raw ASCII `%.4x` of a value 30..2550 (builder special case) | `TFOnLine.BitBtn1Click` |
| **Recorder: cycle records** | `FUN_005d9a08` | RD | 0xF00 + (n mod 150)·0xC0 | 0xC0 | 0 | `0xf735f4` | poll flags +0x1da5/+0x1da6 ("Download dati") |
| **Recorder: graph pages** | `FUN_005d9c78` | RD | 0x10000 + (n mod 250)·0x80 | 0x80 | 0 | `0xf736b4` (+7 u16 page id, +9+6k / +0xB+6k 20 samples) | poll flag +0x1da3 (graph download) |
| Temp-probe calibration read | `TFOnLine.BRileggiRettaClick` 0x5d15c0 | RD | 0x70 | 0x0C | 0 | `0xf73584` (3×u16 raw, (raw-0x1EA)/k, then 3×u16) | button |
| Temp-probe calibration write | `BT1/BT2/BT3Click` | WD | 0x70 / 0x72 / 0x74 | 2 | 1 | `0xf73584+0/2/4` | buttons |
| Debug I/O | `TFOnLine.BitBtn3Click` 0x5d19fe | GG (8) | 0 | 3 ('0'/'1'×3) | 10 | 10-byte reply, big-endian u16 pairs | debug |
| Debug read | `FUN_005d96c8`, main-form test | RD | 0x01 | 9 | 0 | `0xf732a8` (content not decoded) | `TFDebug.Button1Click`, main-form comm test |
| Free RD | `TFOnLine.BitBtn4Click` 0x5d1dd8 | RD | user | user | 0 | raw hex to memo | hidden debug |

`CI` (5) and `CP` (6) have no callers in this build. `WD` is used only for addresses 0x10, 0x40, 0x70–0x74 and 0x100.

**Memory map implied by `RD`/`WD` [proven addresses, inferred meaning]:** 0x01 (9 B debug), 0x10 serial no. (16),
0x20 FW version (16), 0x40 battery-ID block (50), 0x70 temperature calibration (12), 0x100 settings (62),
0xF00 cycle ring 150×192 B, 0x10000 graph ring 250×128 B. The live block is read with **`RI`** (its own space, addr 0).

---

## 2. Live poll loop [proven]

* `TFMainForm.MainTimer`: **Interval = 50 ms** (raw DFM `Interval\x02\x32`; the text DFM dump dropped it).
  `MainTimerTimer` (0x5e1e34) calls `FUN_005c5c4c` while `byte[0x00f72f44]` is 1. The handler clears that byte
  while it runs, so calls never overlap, and sets it back to 1 at the end.
* `FUN_005c5c4c` adds `MainTimer.Interval` to an accumulator (`TFOnLine+0x1dc0`). Once it reaches **900 ms**
  (`DAT_00f72f4c == 0`, "Gestione Tecnica", the default) or **5000 ms** ("Gestione Commerciale"), it resets the
  accumulator and runs, unless port == 99:
  1. if `PageControl.ActivePage == TSComunicazioni`: `RW` 0 ×16
  2. **`RI` 0 ×0xC0** (three frames: `@63RI00000000405A\r`, `@63RI00004000405E\r`, `@63RI000080004052\r`)
  3. every **10th** poll (`DAT_00aabfb8` counter): `RD 0x40 ×0x32` (`@63RD000040003256\r`) then
     `RD 0x100 ×0x3E` (`@63RD000100003E24\r`)
  4. If the RI read fails, `FUN_005bb090` shows "BLT160 NON COMUNICA", stops polling and closes the form.
     Each frame gets 2 tries.
  5. **Heartbeat**: if `RI[0x26]` (RTC seconds) is unchanged for more than 10 polls, the comms indicator turns red
     (0x5c5d60..0x5c5d9e).
  6. Every 5th poll, one chart/recorder sample is stored:
     V = RI+2/10; I = +RI+6/10 if RI+6≠0, else −RI+8/10 (charge positive, discharge negative);
     `v_soglia = CFG+1 · CFG+0x1D / 200`; `v_nominale = CFG+1`; `i_corr_scar = −CFG+5`;
     `v_min_prog = CFG+0xD` if `CFG+9 == 7`, else `CFG+1 · CFG+0xD / 200`.
  7. Then the user-requested one-shot jobs run (flags set by buttons): clock, reread, write ID, downloads, and so on.
  8. Display: `FUN_005c7eac` → `FUN_005cb2ac` decodes RI (§3).
* Startup sequence before polling (`TFOnLine.FormShow` 0x5c385c), all after a successful `RE 0x69` probe:
  `RD 0x10`, `RD 0x20`, `RD 0x100`, `RD 0x40`, then the poll flag is set to 1.
  **No start or mode command is needed for live data.** BLTView has no "start test" or "stop test" command;
  RI is polled regardless of state, and test state is only read (`RI[0x6B]`).

---

## 3. RI live block (192 bytes, addr 0) — `PTR_DAT_005e8870` → `0x00f731e8`

Little-endian. "u16" = unsigned 16-bit. Evidence is `FUN_005cb2ac` (display) unless noted.
Labels are TFOnLine/TFDebug component names, resolved through the Delphi RTTI field table.

| off | type | scale / unit | meaning | evidence |
|---|---|---|---|---|
| 0x00–0x01 | ? | | not used by the app | – |
| **0x02** | u16 | **/10 → V** | **battery voltage** (`Ind1Tach` gauge; note `STMONTensBat` is the *nominal* V copied from settings, not this) | `FUN_005bca5c(RI+2)` → `FUN_005d8a60` prints `x/10 "." x%10`; poll: `(float)*(ushort*)(RI+2)/10.0` (0x5c770c = 10.0) |
| 0x02 (derived) | | | V per 2-V cell = `round(RI+2 / (CFG+1 / 2) · 10)`, printed `/100` | `FUN_005bed20` (FDIV, FMUL 10.0, Round) → `FUN_005d8b20` |
| 0x04 | u16 | /10 V | voltage across the active load ("TensioneAttivi", debug) | 0x5cc05c `MOVZX [RI+4]` → `STMONTensioneAttivi` |
| **0x06** | u16 | **/10 → A** | **charge current** (`STMONCorrCar`) | 0x5cbb49 `MOVZX [RI+6]` → `FUN_005d8a60` → `+0x164c` |
| **0x08** | u16 | **/10 → A** | **discharge current** (`STMONCorrSca`) | 0x5cbb71 `MOVZX [RI+8]` → `+0x1650` |
| derived | | W | power `= RI+2 · (RI+8/10) / 10` (`STMONPotenza`); active power `= (RI+4/10)·RI+8/10` | 0x5cbb93..0x5cbbcd; 0x5cc07d |
| **0x0A** | s16 | /10 → °C | temperature probe 1 (internal, `TextTermo1BLT`) | `FUN_005bc1e4((short)RI+0xA)`: `T/10` + "°C" |
| 0x0C | s16 | /10 °C | probe 2 (battery [inferred]); shown if `CFG[0x37]&1` | `FUN_005bc310` |
| 0x0E | s16 | /10 °C | probe 3 (heatsink [inferred]); shown if `CFG[0x37]&2` | `FUN_005bc438` |
| 0x10 | s16 | /10 °C | probe 4 (aux [inferred]); shown if `CFG[0x37]&4` | `FUN_005bc560` |
| 0x14 | 16-bit | if `CFG[0x37]&8`: voltage analogue, `/10` → % of bar, printed `%3.1f`; if `&0x10`: current analogue, `(x-400)/10` | analogue sensor input | `FUN_005bc688` |
| **0x18** | u16 | bits | `0x8000` charge alarm LED (`LedAllCar`), `0x4000` charging LED (`LedCar`), `0x2000` discharging LED (`LedScar`), `0x1000` discharge alarm LED (`LedAllScar`) | `FUN_005bc158(…, RI+0x18 & mask)` |
| 0x1A | u8 | bits | bits 0..4 = load stages `Diss01..05` on; `0x20` contactor CB (`Kcb`); `0x40` battery contactor (`Kbat`) | `FUN_005b8928(RI[0x1a],1..5)`, `&0x20`, `&0x40` |
| 0x1B | u8 | bits | `0x01` fans; `0x02` AUXcb; `0x04` AUXall; `0x08` AUXfc; `0x10` AUXfs; `0x20/0x40/0x80` = SV0/SV1/SV2 range relays; range shown as 0x20→96 V, 0x40→72 V, 0x80→48 V, none→24 V | 0x5cb95e.., 0x5cbc23.., `FUN_005b8928(RI[0x1b],6/7/8)` |
| 0x1C | u16 | raw | PWM output (`STMONOutPwm`) | |
| 0x1F | 16-bit | | if 0, SOC is forced to 0 (meaning unknown) | `*(short*)(RI+0x1f)==0` |
| 0x22 | u8 | V | nominal voltage of the running test [inferred] (graph `RI[0x22]·CFG+0x1D/200`) | `FUN_005b874c`, `FUN_005bcea4` |
| 0x23 | u8 | A | test discharge current [inferred] (graph uses `-RI[0x23]`) | `FUN_005bcea4` |
| 0x24, 0x25 | u8 | cV per 2-V cell | graph threshold lines: `RI[x]/200 · RI[0x22]` → V [inferred] | `FUN_005bcea4` |
| **0x26..0x2B** | 6×BCD | | **RTC**: 0x26 ss, 0x27 mm, 0x28 hh, 0x29 DD, 0x2A MM, 0x2B YY (`STMONDataOra`) | `FUN_005d8e18`: `"%2d/%2d/%02d %2d:%2d:%2d"` over BCD (`FUN_005d8c4c` = StrToInt(IntToHex(x))) |
| 0x2C..0x30 | 5×BCD | | discharge start: 0x2C/0x2D minute/hour (order inferred by analogy with RTC), 0x2E DD, 0x2F MM, 0x30 YY | `STINFOInizioSca` |
| 0x31..0x35 | 5×BCD | | charge start, same layout | `STINFOInizioCar` |
| 0x36 | u16 | min → h:mm | charge phase-1 duration | `FUN_005d89c0` = `x/60 ":" x%60` |
| **0x38** | u16 | min | **charge time** (`STMONTempoCarica`); phase 2 = 0x38−0x36 | |
| **0x3A** | u16 | min | **discharge time** (`STMONTempoScarica`) | |
| 0x3C | u16 | min | charge pause | `STINFOPausaCar` |
| 0x3E | u16 | min | discharge pause | `STINFOPausaSca` |
| 0x40 | u8 | count | number of charges | `STINFONumCar` |
| **0x41** | u8 | count | **number of discharges / cycle no.** (`STMONNciclo`) | |
| 0x42 | u16 | Ah | Ah at discharge start | `STINFOAhInizioSca` |
| 0x44 | u16 | Ah | Ah at end of discharge / start of charge | `STINFOFineSca`, `STINFOAhInizioCar` |
| 0x46 | u16 | Ah | Ah at end of charge | `STINFOAhFinCar` |
| **0x48** | u16 | Ah | **present charge**; SOC % = `RI+0x48·100 / CFG+3`, clamped 0..100 | `FUN_005bec44`, `Label103`="… Ah" |
| 0x4A | u16 | Ah | Ah charged in phase 1 | `STINFOAhF1Car` |
| **0x4C** | u16 | Ah | **Ah charged total** (`STMONCapCaricataTOT`) | |
| **0x4E** | u16 | Ah | **Ah discharged** (`STMONAhScaricata`); mean discharge current = `0x4E·60/0x3A` A | 0x5cc… |
| 0x50 | u16 | Ah | "Auto" Ah (meaning unknown) | `STINFOAutoSca` |
| 0x52 | u8 | % | efficiency (≥0xFB = n/a) | `STINFOEfficienza` |
| 0x53 | u16 | /100 | Vmin during discharge (V/cell [inferred]) | `FUN_005d8b20` |
| 0x55 | u16 | /100 | Vmax during charge (V/cell [inferred]) | |
| 0x57 | u16 | /10 A | end-of-charge current | `STINFOCorFinCar` |
| 0x59 | u16 | /10 A | max charge current | `STINFOCorMaxCar` |
| 0x5B / 0x5C / 0x5E | s8 | °C | max temperature in discharge: electronics / battery / heatsink | `(char)RI[x]` |
| 0x5D | s8 | °C | max battery temperature in charge | |
| **0x5F..0x64** | 6 B bitmap | | **alarms/anomalies**: anomaly id = 8·(byte−0x5F) + bit + 1 (bit 0 = LSB). Texts are in table below | loop at 0x5cc… with `FUN_005b8928(RI[0x5f+k], b+1)` → `FUN_0057be60(id)` |
| **0x6B** | u8 | enum | **state**: 0 "START", 1 "BATTERIA IN SCARICA", 2 "PAUSA DI SCARICA", 3 "BATTERIA IN CARICA", 4 "PAUSA DI CARICA", 5 "FINE TEST", 6 "***", 7 "TEST", 8 "COLLAUDO", 9 "IN PROGRAMMAZIONE" | `switch(RI[0x6b])` in `FUN_005cb2ac` |
| 0x6D | u8 | count | active cycles in ring | `DEBUG_CICLI_ATTIVI` |
| 0x6E | u16 | index | cycle-record write pointer (`Pr_ciclo`) | `DEBUG_PR_CICLO` |
| 0x70 | u16 | count | test number (`STMONNtest`, `Pr_test`) | |
| 0x72 | u32 | index | graph-page write pointer (`Pr_grafico`) | `DEBUG_PR_GRAFICO` |
| 0x76..0xBF | | | not read by the app | – |

Anomaly ids (from `Db BltView\BLTVIEW.mdb`, table `TabAnomalie`): 1 EEPROM DIFETTOSA, 2 RTC DIFETTOSO,
3 TERMOM.INT. DIF., 4 PILA RTC SCARICA, 5 MANCA ALIM.RS485, 6 ERRORE COM.485, 7 ERRORE COM.232,
8 ERRORE COM.USB, 9 VENTOLA N. LENTA, 10 VENTOLA N. FERMA, 11 TEMP.INT. ELEVATA, 12 FUSIB.N suAP208-N,
13 VENT.N. VEL. MAX, 14 RESIS.INSER.BASSA, 15 RAM TENS.RIF.DIFF, 17 C.A. SONDA TBATT, 18 C.C. SONDA TBATT,
19 C.A. SONDA TDISS, 20 C.C. SONDA TDISS, 21 C.A. SONDA TAUX, 22 C.C. SONDA TAUX, 23 C.A. SONDA ANAL.,
24 F.S. SONDA ANAL, 25 FUSIBILE BATTERIA, 26 VBnom FUORI RANGE, 27 TENS.BATT.ALTA, 28 VSTOP IN SCARICA,
29 POLI BATT. INVERT, 30 CORR. ATTIVI ALTA, 31 RESIST.RICH. MAX, 32 POT. ATTIVI ALTA, 33 FUSIBILE CAR.BATT,
35 TIMEOUT IN CARICA, 41 STOP TEST DA BATT (others are blank).

---

## 4. Other blocks

**Identify (`RE 0x69`) [proven]**: the 1-byte reply is not decoded. The app looks only at reply characters [5] and
[6], which are the first two characters after `!NNCC`: `"11"` means SmarticI Plus (type 1), `"22"` means BLT 160
(type 2), and anything else falls back to type 2.

**RD 0x10 / 0x20 [proven]**: 16-character ASCII serial number and firmware-version string. FW-date checks use
`Copy(ver, 7, 10)` compared with "16/12/2011" (gates CFG[0x2F] bits 1..4) and "23/06/2009" (gates `RE 0x0C`).

**RD 0x40 ID block (50 B) [proven offsets]**: [0]=0xA5, +0x01 battery ID (16 ASCII, "Batteria", also used for the
recorder DB name), +0x11 32-character text (`StaticText131`, meaning unknown), +0x31 XOR checksum.

**RD 0x100 CFG block (62 B)**: [0]=0xA5, +0x3D XOR checksum. Fields used [offsets proven, meanings mostly inferred
from labels]:

* +0x01 u16 nominal voltage. Volts if +0x31 battery type = 0; 0.1 V if type = 1 (`FUN_005c04fc`). The gauge full
  scale is 1.5·Vnom.
* +0x03 u16 nominal Ah.
* +0x05 u16 discharge current setpoint, A.
* +0x09 u16 work mode (`ModoLavoro`).
* +0x0B u16 work-current threshold.
* +0x0D u16 stop voltage (cV per 2-V cell, or absolute when mode = 7).
* +0x17 language.
* +0x1B graph sample time.
* +0x1D u16 end voltage, cV per 2-V cell.
* +0x2F flags.
* +0x31 battery type.
* +0x37 analogue/probe enable bits.
* +0x38 relay bits.
* +0x39 fan type.

---

## 5. Open questions / uncertainties

1. **[open]** Meaning of reply-header characters [7..12] and of RI bytes 0x00–0x01, 0x12–0x13, 0x1D–0x21 and 0x76–0xBF.
   The app never reads them.
2. **[inferred]** Whether durations are minutes (h:mm display) or seconds (m:ss). The formatter is `x/60 ":" x%60`;
   the labels suggest hours:minutes.
3. **[inferred]** Vmin/Vmax (0x53/0x55) units: printed `/100`, assumed V per 2-V cell.
4. **[inferred]** The minute/hour order of the start timestamps (0x2C/0x2D and 0x31/0x32) is guessed by analogy
   with the RTC block.
5. **[open]** The real baud/format when SmartUSB is unticked depends on an existing `CONFIG_BLT.BIN`. With a zeroed
   record, `SetCommState` would get 110 baud and 4 data bits, which suggests that path is unused. Over USB-CDC,
   baud does not matter.
6. **[inferred]** RE sub-codes 0x03 (apply/reload after a settings write) and 0x0C (snapshot before download) are
   named from context only.
7. RI is decoded from the 3-frame concatenation. Nothing in the app shows whether the device would accept a single
   0xC0-byte `RI` frame. Replicate the 64-byte chunking.

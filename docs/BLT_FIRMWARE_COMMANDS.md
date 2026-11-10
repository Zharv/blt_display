# BLT160 (AP211) firmware: serial command handling (static RE of v1.36)

Source: `D:\rea\Install_STANDARD_V2.6.16\Hex\AP211_V136_9cc8.hex` (PIC18F87J50, banner `V1.36 06.12.2011`
at 0x14D9C). The HEX was converted to a flat image (addresses kept) and analysed with Ghidra 12.1.4 headless,
language `PIC-18:LE:24:PIC-18`. The compiler is Microchip C18 with a software stack (FSR1/FSR2) and heavy
procedural abstraction, so the work is from disassembly. `L[k]` means the stack local `[FSR2+k]`. Ghidra's
decompiler output was unusable. No hardware was touched.

Tags: **[proven]** = read directly from the disassembly at the quoted addresses. **[inferred]** = reasoning from context.

---

## 1. Summary (answers first)

* **`CI` and `CP` do not exist in this firmware [proven].** The dispatcher recognises exactly 8 command
  codes: `WD RI RD RE WC GG RB RV` (§3). `RW` (used by BLTView for "modem status") is not recognised either.
* **An unrecognised command is not rejected [proven].** The command-code variable `0x0B9A` is a global. It is
  only written when one of the 8 letter pairs matches, and nothing resets it first (0x31A2–0x3394; these are the
  only writes in the image). An unknown `CC` therefore **re-runs the previous valid frame's command type** with
  the new address, length and data. The reply echoes the unknown letters (for example `!63CI…`).
  * After a read (`RI/RD/RB/RV`), a `CI`/`CP`/`RW` frame is just that read again. BLTView's `RW 0 ×16`
    normally follows an `RI` poll, so it returns `RI[0..15]`.
  * After a `WD`, a `CI`/`CP` frame with data **writes EEPROM**. After an `RE`, a `CI 000001` would **reset the
    cycles**, and a `CI 000070` would **reboot the unit into the bootloader**.
  * `0x0B9A` is not in the C18 idata table (only 0x3FE/0x3FF are initialised) and no init routine clears it.
    So after power-on, the first unknown command runs an undefined command type [inferred].
  * Codes 5, 6 and 8–12 have no handler. This fits `CI`=5 and `CP`=6 having been removed from an older code
    base that BLTView still lists [inferred].
* **No serial command starts, stops or pauses a test [proven, for direct writes].** The state byte `RI[0x6B]` is
  the RAM variable `0x0A6B` itself (the RI block is RAM 0x0A00–0x0ABF). A flow-following walk from the handler
  entry `0x2EF2` (calls, branches and skips: 4355 instructions) reaches **no write to `0x0A6B`**. It also reaches
  no write to the keypad flags `0x0C7A/7B/7C` that start and stop tests.
  * The same walk over every routine a command triggers later (0x27CE, 0x261E, 0x28F8, 0x11E82, 0x9086, 0x293E,
    0x11F82, 0x138A0, 0x13720, 0x125CA/0x125F4) finds no `0x0A6B` write either.
  * The writes to `0x0A6B` are in the test state machine `0x5970`, the key handler `0x5C8E`, the menu/programming
    handler `0x87E6`, the stop routine `0x2038` (`CLRF 0xA6B`, reached by a long key hold at 0x5D12 or an alarm
    at 0x54D6), and `0xBD3C`/`0xC734` (state 0 / state 5).
  * Indirect influence is possible: a settings write changes the thresholds the running state machine compares
    against (§5).

---

## 2. Transport and framing [proven]

| port | caller | rx = tx buffer | out-length var |
|---|---|---|---|
| USB CDC | `0x445A` (`0x451C`): frame must start with `0x0600[0]=='@'` and end with CR (0x44BE..0x44EA) | RAM 0x0600 | 0x0C31 |
| UART1 (RS-232/485) | `0x4842` | RAM 0x0100 | 0x0BA8 |
| UART2 | `0x4878` | RAM 0x0200 | 0x0BA8 |

All three ports call the same handler, **`FUN_002EF2(rx, tx, len, &outlen)`** (thunk 0xB9E6). The reply is
built in place over the request buffer.

Handler steps:
1. `0x2F2A..0x2F62`: XOR of `buf[0 .. len-3)`. `0x2F64..0x2FD0`: the 2 hex characters before CR are parsed
   (`0x12C5A` hex digit to nibble, `0x12CBA` join nibbles) and compared. A mismatch gives a NAK.
2. `0x3076..0x30E2`: parse node `buf[1..2]`. `SUBLW #0x63 / BZ accept`. The fallback compares with `L[18]`,
   which is also 0x63, so **only node 0x63 is accepted**. Any other node gets a NAK reply, not silence.
3. `0x31A2..0x3394`: command letters `buf[3],buf[4]` set `0x0B9A` (§3).
4. `0x3396..0x348A`: `buf[5..6]` → `0x0BA1` (address bits 23..16), `buf[7..10]` → `0x0B9F/0x0BA0` (address
   bits 15..0). `0x348E..0x3530`: `buf[11..14]` → length `L[20..21]`.
5. `0x3536..0x3556`: **the length is clamped to 0x40** (`MOVLW 0x40 … SUBFWB … BC`). This is why BLTView
   reads in 64-byte chunks.
6. `0x35EE..0x3936`: reply header `tx[0]='!'`, `tx[1..2]="63"`, `tx[3..14] = rx[3..14]` (CC, AAAAAA, LLLL
   echoed). **The payload overwrites from `tx[13]`**, so a data reply is `! NN CC AAAAAA LL <2N hex>`.
7. `0x42AA..0x430A` **NAK**: if the error flag `L[13..14]==1` or the memory status `L[17]!=0`, then
   `tx[3]=tx[4]='E'` and the length is 5. The reply is `!63EE<XX>\r` (the "EE" sits in the command field).
8. `0x430C..0x4430`: XOR over `tx[0..len)`, two hex characters appended, then CR (`MOVLW 0x0D` at 0x442E).

NAK causes: bad checksum, node ≠ 0x63, `RD`/`WD` external-memory error or range error (`L[17]`, `L[13]`), and
`RE` with an unknown sub-address. `RI`/`RB`/`RV`/`GG`/`WC` never NAK on their own. `WC` NAKs only on an RTC
I²C error.

---

## 3. Command table (`0x0B9A` codes) [proven]

| letters | code | set at | handler | function |
|---|---|---|---|---|
| `WD` | 0 | 0x31C6 | 0x355A (data parse), 0x3F34 | write external EEPROM (§4.5) |
| `RI` | 1 | 0x3208 | 0x393A | read **RAM 0x0A00 + addr** (live block) |
| `RD` | 2 | 0x324A | 0x3C36 | read external EEPROM, 24-bit address |
| `RE` | 3 | 0x328C | 0x3DEA | action / "execute" by sub-address (§4.4) |
| `WC` | 4 | 0x32CE | 0x4052 | set RTC (7 bytes) |
| `GG` | 7 | 0x3310 | 0x4142 | debug stub (§4.6) |
| `RB` | 0x0D | 0x3352 | 0x3A36 | read **RAM 0x07C1 + addr** (parameter-lock flags) |
| `RV` | 0x0E | 0x3394 | 0x3B36 | read **RAM 0x0B00 + addr** (internal variables; not used by BLTView) |
| `CI`, `CP`, `RW`, anything else | – | – | – | **no match; `0x0B9A` keeps its old value** (§1) |

Evidence for the RI base, from 0x3974..0x398A:
```
ADDWF 0x9f,w ; MOVWF FSR0L ; MOVF 0xa0,w ; ADDWFC FSR0H ; MOVLW 0x00 ; ADDWF FSR0L ; MOVLW 0x0a ; ADDWFC FSR0H ; MOVF INDF0,w
```
That is `FSR0 = 0x0A00 + i + addr`. RB uses `+0x07C1` (0x3A82/0x3A86) and RV uses `+0x0B00` (0x3B82/0x3B86).
The routine `0xB9B8` copies the RTC (0x0C0E..) into `0x0A26..0x0A2B`, which is exactly BLTView's `RI[0x26..0x2B]`.
This confirms that the RI block is RAM at 0x0A00.

---

## 4. Per-command details

### 4.1 `RI` / `RB` / `RV` (RAM reads) [proven]
* No range check, and the high address byte `0x0BA1` is ignored. The FSR is 12-bit, so `0xA00+addr` wraps.
  `RI` with addr ≥ 0x500 (or `RV` with addr ≥ 0x400) reads the **SFR area 0xF00–0xFFF**. Reading registers such
  as RCREGx/SSPBUF/POSTINCx has side effects [inferred]. **Keep `RI` addr+len ≤ 0xC0.**
* No state guard and no side effects within the GPR range. Reply `13 + 2·len` chars, then XX and CR.

### 4.2 `RD` [proven]
`addr16 + len` must be ≤ 0x17FFF (0x3C40..0x3CA0; the high byte is not added). The data is read from external
I²C EEPROM (`0x125CA` → per-byte `0x12564`, 6 retries, SDA on PORTC.4) using the full 24-bit address
(`0x0BA1:0x0BA0:0x0B9F`) into the scratch buffer RAM 0x0373, then hex-encoded. On error `L[17]≠0` gives a NAK.

### 4.3 `WC` (clock) [proven code path, field meaning inferred]
7 data bytes `rx[15..28]` → RAM 0x0373..0x0379 (0x405A..0x40D2). Then `0x037A..0x037C = 1,2,3` and a call to
`0x138A0(0x0373)`. That routine is a bit-banged I²C write to device **0xD0** (an RTC, DS1307-type), register 0.
An I²C error gives a NAK. Then `0x13720(0x0E,0x0C)` → `0x0BAD`, which clears `0x0C67.3` and sets `0x0C67.1`
when 1 ≤ `0x0BAD` ≤ 8 (the RTC anomaly flags [inferred]). No test-state access. Reply: header only (15 chars).

### 4.4 `RE` sub-addresses (16-bit compare on `0x0B9F/0x0BA0`; `0x0BA1` ignored) [proven]
Switch at 0x3DF4..0x3EA2. Every accepted case replies with header only (`*outlen = 0x0F`, 0x3F24), which BLTView
reads as N=1.

| addr | firmware action | BLTView label (from BLT_REGISTERS.md) |
|---|---|---|
| 0x00 | **nothing** (0x3EA2 → 0x3F24) | "reset statistics" |
| 0x01 | `BSF 0x0C6F,5` (0x3EA6). The main loop at 0x8CEC..0x8CFE consumes it: `0x11E82` re-initialises the test record and `0x9086` resets the cycles (see below) | "reset cycles" ✔ |
| 0x02 | nothing | "reset graph" |
| 0x03 | `CALL 0x27CE` (reload the 0x3C-byte CFG block from EEPROM, clamp it, or load defaults if the checksum is bad); `CALL 0x261E` (reload the battery-ID block, or default `"BATTERY N_1"`); `0x0BAF=0xFE` (forces a display redraw [inferred]) | "apply after write" ✔ |
| 0x04, 0x05, 0x06, 0x08, 0x09, 0x0A, 0x0B, 0x0C | nothing | 0x0A "Allinea", 0x0C "freeze/prepare download" |
| 0x69 | `tx[5]=tx[6]='3'` (0x3EC2..0x3EF8). **Identify reply is "33"** (`!63RE330069…`). BLTView maps "11"/"22" and treats anything else as type 2 (BLT160) | identify probe |
| 0x70 | `RAM[0x0300]=0x55` (0x3EFC..0x3F00), `CALL 0x1470C` (UCON=0, UIE=0, USB state=0: **USB detach**), **`RESET`** (0x3F06) | "reset graph pointer" ✘ |
| other | `L[13]=1` → **NAK** (0x3F14) | – |

Working battery-ID block vs. the record serial [proven on the tester, 2026-11-10]:
* `0x261E` loads the ID block into RAM at `0x03B8` (serial at `0x03B9`, 16 chars, space padded).
* A test copies that serial into its record (`RI 0x76`) only on a state change (`0xB002` → `0x11F2E` →
  `0x11D98`, copy at `0x11E40`). In START `RI 0x76` is still the serial of the **previous** test, so it cannot
  be used to check a `WD` + `RE 0x03`.
* `RI` does not bound its address, so the working block is readable: the serial is at **`RI 0x09B6`**
  (dump of `RI 0x09A0..0x09DF` showed it there, followed by spaces). The panel reads only this bank-3 window
  (`blt_usb_read_ram3`), never higher (SFRs).

`RE 0x01` detail (0x11E82, 0x9086):
* Discharge-start timestamp `RI[0x2C..0x30]` := RTC.
* `RI[0x31..0x3F]` (charge start, durations, pauses) := 0.
* `RI[0x42/0x44/0x46]` := `RI[0x48]` (present Ah).
* `RI[0x4A..0x51]` := 0. `RI[0x52]` (efficiency) := 0xFE.
* `RI[0x53..0x56]` recomputed.
* `RI[0x57..0x6A]` := 0, which includes the **anomaly bitmap 0x5F..0x64**.
* `RI[0x6D]` (active cycles) := 0. `RI[0x70]` (test number) := 1.
* Then `0x293E` and `0x11F82` (cycle-ring pointer reset/flush [inferred]).

It **does not touch `RI[0x6B]`**: 0x11E82 stops at 0x0A6A. The same path also runs from the keypad (0x8CCA..0x8CEA:
state 0, menu page 7, key combination).

`RE 0x70` [inferred]: the application itself only ever clears 0x0300 (`0x46DC`, at start-up). The 0x55 marker must
therefore be read by the boot block at 0x0000–0x1FFF, which is not in this HEX. This is the "jump to firmware-update
(DFU) bootloader" command. **On a v1.36 unit, `RE 0x70` reboots the tester and drops USB.** Whatever test is running
is interrupted. Treat it as dangerous. Note that BLTView labels the call site `BResetProgresGraficoClick`, which
does not match this firmware.

### 4.5 `WD` [proven]
* Payload `rx[15+2i..16+2i]` → RAM 0x0373+i, with i < len ≤ 0x40 (0x355A..0x35EC).
* Range check `addr16+len ≤ 0x17FFF`. Then `0x125F4(len, 0x0373, addr24)` writes the **external EEPROM only**
  (per-byte `0x124CE`). An error gives a NAK.
* After a successful write (0x3FE2..0x403E):
  * if addr == 0x000100 (settings): call `0x27CE` (reload CFG into RAM immediately) and set `0x0C08=1`. The main
    loop at 0x8E90 then calls `0x28F8` to re-save CFG.
  * Exception: when `CFG.mode` (RAM 0x078D) is 4 or 5 and is 5 with `RI[0x41] == CFG+0x0B` (0x078F), the reload
    is skipped. **The reload has no test-state guard**, so new settings take effect while a test is running.
  * any address: if state `0x0A6B` is 0 (START) or 9 (PROGRAMMING), `RI[0x48]` (present Ah) := CFG nominal Ah
    (RAM 0x0787).
* Reply: header only.

The CFG RAM copy is based at **0x0784** [inferred from consistent offsets]:
* +3 (0x0787) = nominal Ah.
* +9 (0x078D) = mode.
* +0x0D (0x0791) = stop voltage, compared with the measured voltage in discharge at 0x5A72..0x5A80.
* +7 (0x078B) is compared with the discharge time `RI[0x3A]`.
* +0x15 (0x0799) is compared with the charge pause `RI[0x3C]`.
* +0x19 (0x079D) is compared with the discharge pause `RI[0x3E]`.

`RB` (0x07C1) starts right at the end of this block.

### 4.6 `GG` [proven]
Ignores the request data. It loops 5 times, writing 4 hex characters each, from the 16-bit local `L[5..6]`,
which is **never initialised** (no write to it in the function). It then adds one more character, '0', and sets
`*outlen = 0x22` (0x414C..0x42A8). The reply is 34 characters + XX: header + 20 characters of stale stack data
+ "0". That is 1 character more than BLTView's 10-byte expectation. **No side effects.** It is a vestigial debug stub.

---

## 5. What can change the test state

The state machine is `FUN_5970`, called from the scheduler at 0xB1B0. Its dispatch on `0x0A6B` at 0x5984..0x59B8:

| state | transition logic |
|---|---|
| 0 START | 0x59BC: idle, re-initialises the record |
| 1 discharging | 0x59F4: → 2 when discharge time ≥ CFG+7, or when V < stop voltage for 60 ticks |
| 2 discharge pause | 0x5AD6: → 5 via `0xC734`, or → 3 when the pause ≥ CFG+0x19 |
| 3 charging | 0x5B26: → 4 or 5 |
| 4 charge pause | 0x5BBC: → 1 when the pause ≥ CFG+0x15; cycle number +1 |
| 5 END | 0x5C04 |
| 8 COLLAUDO | 0x5C0A |
| 7, 9 | no automatic transition |

Start comes only from the keypad. In `0x5C8E`, the key flag `0x0C7C` bit 7 (set by the key scanner at 0x1354C)
moves state 0 → 1 (0x5CC0) or 0 → 3 (0x5CE6). Stop/abort comes from a long hold of key `0x0C7A` bit 0
(0x5D12..0x5D30) or an alarm condition (0x54D6), both via `0x2038`, which does `CLRF 0x0A6B`.

From the serial side [inferred, indirect only]:
* `WD 0x100` reloads CFG at once (§4.5). Changing the stop voltage, the max discharge time or the pause times
  during a running test can make the next state-machine pass move 1 → 2, 2 → 3/5 or 4 → 1.
* `RE 0x03` does the same reload. If the CFG checksum in EEPROM is bad, `0x27CE` loads **defaults** (mode 2, …)
  and writes them back.
* `RE 0x70` reboots the unit.
* `RE 0x01` clears the record and the alarms but leaves the state alone.
* An unknown command (`CI`/`CP`/`RW`) repeats the previous command type (§1).

None of these is a "start/stop/pause" command.

---

## 6. Practical consequences for blt-esp-display
* Safe reads: `RI` (addr+len ≤ 0xC0), `RD`, `RB` (≤ 0x1E), `RV`. Max 0x40 bytes per frame.
* `RW`, `CI` and `CP` are not real commands on v1.36. Never send them: their effect depends on the previous
  frame. If the ESP only ever sends reads, an accidental `RW` just repeats a read. Still, keep `RW` out of the poll
  loop; it returns `RI` bytes, not modem data.
* Keep `RE`, `WD` and `WC` blocked (already the policy). `RE 0x70` = reboot/bootloader. `RE 0x01` = wipes the test
  record and alarms. `WD 0x100` = live settings change.
* Identify: expect `"33"` in reply characters [5..6] from this firmware (not "22").
* NAK frame: `!63EE<XX>\r` (5 characters + XX + CR). A wrong node also gets a NAK, not silence.

## 7. Open points
* The bootloader (0x0000–0x1FFF) is not in the HEX. The meaning of the 0x55 marker at 0x0300 is inferred.
* The `0x0A6B`-write search is static. It covers direct and `MOVFF` writes but not every FSR-indirect store. No
  indirect store into 0x0A00+ was seen in the handler: its computed-address stores go to the tx buffer, the
  0x0373 scratch buffer and the stack. The helper routines were not checked store by store.
* State 7 ("TEST") has no direct write, so it is probably set indirectly or unused in v1.36.

Scratch artefacts are in the session scratchpad `fw\`: `ap211.bin`, `listing.txt` (full Ghidra listing),
`disp.txt` (simplified dispatcher) and `reach.py`.

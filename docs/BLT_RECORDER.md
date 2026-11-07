# BLT160 recorder: downloading finished tests (cycle records + voltage diagram)

Source: static RE of `BltView.exe` 2.6.16 (Delphi, Ghidra VAs, image base 0x400000), cross-checked against the
tester firmware AP211 v1.36 listing (`BLT_FIRMWARE_COMMANDS.md`). No hardware was touched.
Framing is in `BLT_PROTOCOL.md`; the live block (RI) is in `BLT_REGISTERS.md` §3.

Tags: **[proven]** = read in BltView decompile/disassembly at the quoted address; **[FW]** = read in the
firmware listing at the quoted address; **[inferred]** = reasoning, not verified.

Buffers used below: `REC` = cycle-record buffer `0x00f735f4` (`PTR_DAT_005e8784`), `PAGE` = graph-page buffer
`0x00f736b4` (`PTR_DAT_005e8718`), `RI` = live block `0x00f731e8`, `CFG` = settings block `0x00f73414`.
`N_CYC = DAT_005e862c = 150`, `N_PAGE = DAT_005e8630 = 250` [proven, read from the image].

---

## 0. Short answer / recipe for the panel

1. Poll `RI 0..0xBF` (3 × 64-byte frames). Take `ptrCyc = RI.u16[0x6E]`, `nAct = RI[0x6D]`,
   `ptrPage = RI.u32[0x72]`. Optionally read `RD 0x10` (serial), `RD 0x40` (ID), `RD 0x100` (settings).
2. Cycle records: for absolute index `n` from `ptrCyc - nAct + 1` to `ptrCyc` (inclusive), read
   `RD 0xF00 + (n mod 150)*0xC0`, 0xC0 bytes = **3 frames** of 0x40. Keep the record only if
   `REC.u16[0x6E] == (n & 0xFFFF)` (record carries its own index; see §1.2, §4). The slot at `ptrCyc` is the
   **current / in-progress** cycle and is rewritten by the tester on every state change.
3. A "test" = all records with the same `REC.u16[0x70]` (Pr_test). One record = one cycle `REC[0x41]` of it.
4. Graph pages: for absolute index `p` from `max(0, ptrPage-249)` to `ptrPage` (inclusive), read
   `RD 0x10000 + (p mod 250)*0x80`, 0x80 bytes = **2 frames**. Each page holds up to 20 samples; each sample
   carries the Pr_ciclo (`REC.u16[0x6E]`) of the cycle it belongs to. The diagram of a cycle = all samples
   whose tag equals that record's `0x6E`, in (p, k) order. Stop a page at the first sample with V = 0xFFFF.
5. Sample time = page header time (BCD `hh:mm DD/MM/YY`, seconds = 0) + k × `PAGE[0x7F]` **minutes**.
   V = u16 / 10 V; I = s16 / 10 A (negative = discharge).
6. Send only `RI` and `RD`. BLTView additionally sends `RE 0x0C` before a download; v1.36 firmware ignores
   it (no-op), so the panel can omit it. Nothing is written during or after the download.

---

## 1. BLTView download flow

### 1.1 Trigger and flags [proven]
* Button `BMemorizzaOld` (`TFOnLine.BMemorizzaOldClick` 0x5cacac, caption "Download dati"/"Interrompi").
  When idle it shows the customer/operator dialog `FClienti` (global `0x5eef3c`, `ShowModal` via vtable+0xE8),
  then:
  ```c
  FUN_0040ad54("select NCiclo,Pr_test,Pr_ciclo from TabOld where MTRScaricatore = '%s' order by Pr_ciclo desc", …);
  … FData+0x408 = field0 (NCiclo); FData+0x40c = field1 (Pr_test)      // newest stored row, -1/-1 if none
  *(param_1 + 0x1da8) = 0; *(param_1 + 0x1cad) = 0; *(param_1 + 0x1db0) = 1;
  ```
  Clicking again while a download runs sets the stop flags (`0x1da7=1`, `0x1da8=1` for cycles; `0x1da4=1` for graph).
* Dialog choice [proven, `TFClienti.InserisciClick` 0x57803c]:
  `FClienti+0x35c = RadioButton1.Checked` where RadioButton1 = **"Scarica cicli mancanti"** (download missing
  cycles) and RadioButton2 = "Scarica tutti i cicli" (all). Customer id `FClienti+0x34c` and operator id `+0x358`
  are **PC-database ids**, not tester data. RadioButton1 is re-checked afterwards.
* The work is done one step per poll tick inside `FUN_005c5c4c` (poll, every 900 ms "Gestione Tecnica" or
  5000 ms "Commerciale", see BLT_REGISTERS §2), after the normal `RI` read (+ every 10th tick `RD 0x40`, `RD 0x100`).

### 1.2 Cycle records (flag `0x1db0` → `0x1da6`) [proven, `FUN_005c5c4c` 0x5c6b70..0x5c7290]
1. **Prepare** (0x5c6b70): if FW-version date (`Copy(STMONVers,7,10)`) ≥ "23/06/2009", send
   `FUN_005d958c` = **`RE 0x0C`** (`FUN_005d8530(port,2,0xc,0,1,0,0)`), then set `0x1da6 = 1`.
   v1.36 firmware: `RE 0x0C` → "nothing", header-only ack [FW, BLT_FIRMWARE_COMMANDS §4.4].
2. **Range, computed once** (0x5c6f2b..0x5c7138). `last = TFOnLine+0x1cc8` = `Pr_ciclo` of the newest TabOld row of
   this tester (`FUN_00579930`: `select Autoincrementale,Pr_ciclo from TabOld where MTRScaricatore='%s' …`, -1 if
   none; loaded at connect `TFOnLine.FormShow` and after each download):
   ```c
   start = (last < 0) ? last + 1 : last;                      // NB: inclusive, re-reads the newest stored record
   if (RI.u16[0x6e] < start || FClienti.byte[0x35c] == 0) {   // pointer went back (reset) or "all cycles"
       start = (RI.u16[0x6e] + 1) % 150;  count = 150;
   } else {
       count = (last < 0) ? RI.u16[0x6e] + 1 : RI.u16[0x6e] + 1 - last;
       if (count > 150) { start = (RI.u16[0x6e] + 1) % 150; count = 150; }
   }
   if (FClienti.byte[0x35c] == 1 && count > RI[0x6d]) {         // "missing cycles": never more than active cycles
       start += count - RI[0x6d];  count = RI[0x6d];
   }
   ```
   So in "missing" mode BLTView reads absolute indices `[RI.6E - min(count,RI.6D) + 1 … RI.6E]`.
   The UI counter `FUN_005c7eac` 0x5c7421 shows the same number: `RI.6E - last + 1`, clamped to `RI.6D` and 150.
   The download button is only enabled when `RI.u16[0x6E] >= 2` (0x5c7412).
3. **One record per tick** (0x5c7146): `FUN_005d9a08(n)`:
   ```c
   iVar1 = __llmod(n, 150);
   FUN_005d8530(port, 1 /*RD*/, iVar1 * 0xc0 + 0xf00, 0, 0, 0, 0xc0);   // split into 3 × 0x40 frames
   FUN_005d7bf0(&DAT_00f735f4, rx, 0xd, 0xc0);
   ```
   then `FUN_0057a49c` stores it (§1.2.4), `0x1cc8 = REC.u16[0x6e]`, `n++`, `count--`; stops when `count < 1` or the
   user stops. At the end: `FUN_00579930` reloads `last`, and **`BAggiornaGrafico.Click`** is called
   (`MOV SI,0xffeb; CALL _CallDynaInst` at 0x5c727f) → graph download starts automatically (§1.3).
   A failed read → `FUN_005bb090` ("BLT160 NON COMUNICA", polling stops).
4. **Store / de-duplicate** `FUN_0057a49c` [proven]:
   ```c
   if (FData+0x40c == REC.u16[0x70]) {                 // same test as newest stored row
       if (REC[0x41] < FData+0x408) skip;              // older cycle number → ignored
       else FData+0x408 = REC[0x41];
   } else { FData+0x40c = REC.u16[0x70]; FData+0x408 = -1; }
   // not skipped:
   "select PR_CICLO,Autoincrementale from TabOld where MTRScaricatore = '%s' order by Autoincrementale" → last row
   if (REC.u16[0x6e] == last.PR_CICLO) TabOld.Locate("Autoincrementale; PR_CICLO") + Edit   // overwrite in place
   else { FUN_00579ad0(newLinkKey,"0",1) /*settings snapshot → TabProgramma*/; TabOld.Insert; }
   ```
   i.e. the newest slot is re-read every time and **updated** while it is still the same `Pr_ciclo`.
   New rows get `LinkKey = MAX(Autoincrementale)+1` (`FUN_005b6a14`), `IDCliente`/`IDOperatore` from the dialog,
   `MTRScaricatore` = serial (`RD 0x10`), `data_scaricoold` = PC clock.

### 1.3 Graph pages (flag `0x1da3`) [proven, `FUN_005c5c4c` 0x5c72c0..0x5c7639]
1. **Range, once**:
   ```c
   end   = DAT_00aabfa8 = RI.u32[0x72];          // MOV EAX,[EAX+0x72] at 0x5c72d5
   start = DAT_00aabfac = FUN_0057c244();        // "SELECT top 1 indice from TabGrafico where IDSmartic='%s' order by Indice desc", 0 if none
   if (250 + start < end) start = end - 250;
   if (FClienti.byte[0x35c] == 0) { FClienti.byte[0x35c] = 1; start = 0; end = 0xfa; }   // "all": slots 0..250
   ```
   `start` is inclusive (the newest stored page is re-read, because it may have been only partly filled).
2. **One page per tick** while `start <= end`: `FUN_005d9c78(start)`:
   ```c
   iVar1 = __llmod(n, 250);  memset(&DAT_00f736b4, 0, 0x80);
   FUN_005d8530(port, 1 /*RD*/, iVar1 * 0x80 + 0x10000, 0, 0, 0, 0x80);  // 2 × 0x40 frames
   ```
   then `FUN_0057c854(PAGE.u16[7], start)` stores the samples (§3.3) with `Indice = start` (absolute page number),
   `start++`. Stop flag `0x1da4` or `start > end` ends it.
   (The code at 0x5c7446 that compares `PAGE.u16[7]` with `RI.u32[0x72]` only feeds the live chart; it does not
   affect what is stored.)
3. A periodic timer `TGraficoAUTO` (`TFOnLine.TGraficoAUTOTimer` 0x5d62d8 sets flag `0x1db1`) repeats
   `RE 0x0C` + `BAggiornaGrafico.Click` ("Download grafico automatico ATTIVATO").

### 1.4 Everything BLTView sends around a download
| when | frame | effect on v1.36 [FW] | panel |
|---|---|---|---|
| every tick | `RI 0x000/0x040/0x080` ×0x40 | read RAM | needed (pointers) |
| every 10th tick | `RD 0x40` ×0x32, `RD 0x100` ×0x3E | read EEPROM | optional |
| before cycle download and on TGraficoAUTO | **`RE 0x0C`** (FW date ≥ 23/06/2009) | no-op, ack only | **omit** |
| per tick, cycles | `RD 0xF00+(n%150)*0xC0`, 3 frames | read EEPROM | needed |
| per tick, graph | `RD 0x10000+(p%250)*0x80`, 2 frames | read EEPROM | needed |

No `WD`, `WC` or other `RE` is sent by the download path. Do **not** use the other recorder buttons:
`RE 0x01` "Reset cicli" wipes the test record/alarms, `RE 0x02` "Reset grafico" (no-op on v1.36),
`RE 0x70` "Reset progressivo grafico" **reboots the tester into the bootloader** on v1.36.

---

## 2. Cycle record (0xC0 bytes at `0xF00 + slot*0xC0`)

### 2.1 It is a byte-for-byte snapshot of the live block
[FW] `FUN_011f2e` computes `addr = 0x0F00 + (RI.6E mod 0x96)*0xC0` (`MOVLW #0x96`, `MOVLW #0xc0`, `+0x0F`) and writes
three 64-byte EEPROM blocks whose source is RAM `0x0A00`, `0x0A40`, `0x0A80` (`MOVLW #0x40 / #0x00 / #0x0a` …,
`#0x40 / #0x80 / #0x0a`) — the RI block. It is called
* on every change of state `RI[0x6B]` or status byte `RI[0x6C]` (0xb00a..0xb02c), so the current slot is live;
* by `FUN_011f82` at the end of a cycle: write slot, then `RI.6E++`, `RI.6D = min(RI.6D+1, 150)`, save pointers
  (`0x293e`), re-initialise the record unless state = 5, and write the new slot.

So **the layout is the same as RI (BLT_REGISTERS §3)**, values frozen at the snapshot. BLTView's own mapping
`FUN_0057a49c` (record → TabOld) confirms it and names the bytes that the live decoder never used.

### 2.2 Field map (`FUN_0057a49c` writes TabOld; `FUN_00579ad0` writes TabProgramma) [proven unless tagged]
Little-endian. BCD = packed BCD (`FUN_005d8c4c` = `StrToInt(IntToHex(x,2))`).

| off | type | DB column | meaning / unit |
|---|---|---|---|
| 0x00–0x1D | | – | instantaneous measurements at snapshot time (RI §3: V, I, temps, relays …); not used |
| **0x1E** | u8 | `byte_config` / `sMODO_PRG` | **work mode** (index into mode list, §2.4) — becomes `CFGOLD+9` |
| **0x1F** | u16 | `Ah_nom_batt` / `sAHNOM` | **rated capacity, Ah** |
| 0x21 | u8 | `Ah_over_boost` / `sAH_MAG_RIC` | recharge surplus % (`CFG+0x13`) |
| **0x22** | u8 | `tens_nom_batt` / `sVBNOM` | **rated voltage, V** (Pb) |
| **0x23** | u8 | `corr_scarica` / `sISCAR_PRG` | **discharge current, A** (`CFG+5`) |
| **0x24** | u8 | `soglia_tens_car` / `s_VGAS` | **voltage threshold**, cV per 2-V cell (`CFG+0x1D`) |
| **0x25** | u8 | `soglia_tens_stop` / `sVSTOP` | **discharge stop voltage**, cV per 2-V cell (`CFG+0xD`) |
| 0x26/27/28 | BCD | `sec`,`min`,`hour` (+`ora_storico`) | snapshot time (RTC) |
| 0x29/2A/2B | BCD | `day`,`month`,`year` (+`data_storico`, year+2000) | snapshot date |
| **0x2C/0x2D** | BCD | `min_ciclo`, `hour_ciclo` (+`ora_ciclo`) | **discharge start time** — 0x2C = minute, 0x2D = hour (settles BLT_REGISTERS open point 4) |
| **0x2E/2F/30** | BCD | `day_ciclo`,`month_ciclo`,`year_ciclo` (+`data_ciclo`) | **discharge start date** DD MM YY |
| 0x31/0x32 | BCD | `min_car`, `hour_car` | charge start time |
| 0x33/34/35 | BCD | `day_car`,`month_car`,`year_car` | charge start date |
| 0x36 | u16 | `T1Car` | charge phase-1 time, min |
| 0x38 | u16 | `TCar` | total charge time, min |
| **0x3A** | u16 | `TScar` | **discharge time, min** |
| 0x3C | u16 | `TWCar` | pause after charge, min |
| **0x3E** | u16 | `TWScar` | **pause after discharge, min** |
| 0x40 | u8 | `NCCar` | number of charges |
| **0x41** | u8 | `NCScar`, `NCiclo` | **cycle number** |
| **0x42** | u16 | `Ah_in_batt_inizio_ciclo` | **capacity at cycle start, Ah** |
| **0x44** | u16 | `Ah_inizio_car` | **capacity at discharge end** (= at charge start), Ah |
| 0x46 | u16 | `Ah_fine_car` | capacity at charge end, Ah |
| 0x4A | u16 | `AhCarF1` | Ah charged in phase 1 |
| 0x4C | u16 | `AhCarT` | Ah charged total |
| **0x4E** | u16 | `AhScar` | **discharged capacity, Ah** |
| 0x50 | u16 | `Ah_autoscar` | self-discharge Ah |
| **0x52** | u8 | `Efficienza` | **efficiency %**, ≥ 0xFB = not available ("***") |
| **0x53** | u16 | `VBmin` | **minimum voltage**, /100, label "V/el" (V per 2-V cell [inferred from label]) |
| 0x55 | u16 | `VBmax` | max voltage in charge, /100 V/el |
| 0x57 | u16 | `corr_min_car` | end-of-charge current, /10 A |
| 0x59 | u16 | `corr_max_car` | max charge current, /10 A |
| **0x5B** | s8 | `temp_int_max_scar` | **max temperature on electronic card in discharge, °C** |
| 0x5C | s8 | `temp_batt_max_scar` | max battery temp in discharge, °C (shown if `CFG[0x37]&1`) |
| 0x5D | s8 | `temp_batt_max_car` | max battery temp in charge, °C |
| 0x5E | s8 | `temp_diss_max_scar` | max heatsink ("active section") temp in discharge, °C (shown if `CFG[0x37]&2`) |
| 0x5F–0x64 | 6×u8 | `byte_anom0..5` | active-anomaly bitmap at snapshot |
| **0x65–0x6A** | 6×u8 | `byte_mem_anom0..5` | **memorised anomalies — the list printed on the Info page** (§2.3) |
| 0x6B | u8 | `Stato_sistema` | state (RI §3 enum) |
| 0x6C | u8 | `ByteBitStato` | status bits |
| 0x6D | u8 | `Num_cicli_attivi` | active cycles at snapshot |
| **0x6E** | u16 | `Pr_ciclo` | **this record's own absolute index** (slot = value mod 150); links the graph |
| **0x70** | u16 | `Pr_test` | **test number** |
| 0x72 | u32 | `Pr_grafico` | graph-page pointer at snapshot |
| **0x76** | 16×ASCII | `id_batteria` | **battery id** (copy of ID block +0x01); empty → BLTView writes "ERR BLT 160: NO BAT" |
| **0x86** | 32×ASCII | `campo_note` | **note** (copy of ID block +0x11); empty → "ERR BLT 160: NO NOTE" |
| 0xA6 | u8 | `sNC_PRG` | programmed number of cycles (`CFG+0xB`) |
| **0xA7** | u16 | `sTSCAR_PRG` | **discharge time limit, min** (`CFG+7`); 6000 = unlimited ("** : **") |
| 0xA9 / 0xAB | u16 | `sTC_PRG_MAX` / `sTC_PRG_MIN` | max/min charge time (`CFG+0xF/+0x11`) |
| 0xAD / 0xAF | u16 | `sTPCAR_PRG` / `sTPSCAR_PRG` | programmed pause after charge / discharge (`CFG+0x15/+0x19`) |
| 0xB1 | u8 | `sTCGRAF` | graph sample time, min (`CFG+0x1B`) |
| **0xB2** | u8 | `sTipoBatteria` | **battery type**: 0 "000 - Pb", 1 "001 - NiCd" (`CFG+0x31`) |
| 0xB3 | u16 | `sVBNOM` when 0xB2 = 1 | rated voltage for NiCd (0.1 V [inferred from CFG+1]) |
| 0x96–0xA5, 0xB5–0xBF | | – | not read by BLTView |

Evidence lines (`FUN_0057a49c`):
`FUN_00518338(TabOld,"byte_config") … PTR_DAT_005e8784[0x1e]`, `"Ah_nom_batt" … *(undefined2*)(REC+0x1f)`,
`"tens_nom_batt" … REC[0x22]`, `"corr_scarica" … REC[0x23]`, `"soglia_tens_car" … REC[0x24]`,
`"soglia_tens_stop" … REC[0x25]`, `"min_ciclo" … FUN_005d8c4c(REC[0x2c])`, `"hour_ciclo" … REC[0x2d]`,
`"TScar" … REC+0x3a`, `"TWScar" … REC+0x3e`, `"Pr_ciclo" … REC+0x6e`, `"Pr_test" … REC+0x70`,
`"id_batteria" … FUN_00404e58(…, REC+0x76, 0x10)`, `"campo_note" … REC+0x86, 0x20`.
`FUN_00579ad0`: `if (REC[0xb2]==0) "sVBNOM"=REC[0x22]; else if (REC[0xb2]==1) "sVBNOM"=*(u16*)(REC+0xb3)`,
`"sMODO_PRG"=REC[0x1e]`, `"sTSCAR_PRG"=*(u16*)(REC+0xa7)`, `"sTipoBatteria"=REC[0xb2]`, `"sTCGRAF"=REC[0xb1]`.
`CaricaStrutturaParametri` 0x59b0dc maps them back: `"sTSCAR_PRG"→CFGOLD+7`, `"sMODO_PRG"→+9`, `"sVSTOP"→+0xd`,
`"s_VGAS"→+0x1d`, `"sTipoBatteria"→+0x31`, `"sISCAR_PRG"→+5`.

### 2.3 "Info - OFF LINE" report: every field and where it comes from
Print form `TFPrintINFOOLD.QRBeforePrint` 0x5831b8 sets the title `"Info - OFF LINE"` and copies the captions of the
`TFOffLine` "Info OLD" tab, which `AggPaginaDatiOld` 0x599914 and `AggPaginaParametri` 0x59ba48 fill from the
TabOld row (`CaricaStrutturaOld` 0x5984c0 rebuilds an RI-shaped struct `OLD` = `0x00f732b4`) and from the
TabProgramma row (`CFGOLD` = `0x00f734f4`). Because both rows were filled from the record, nearly all of it can
be computed from the 0xC0 record. English text = `Lingue\STRINGUK.TXT` id.

| report line (EN id) | value | source | format |
|---|---|---|---|
| title | "Info - OFF LINE" | constant | |
| BLT 160 serial | `MTRScaricatore` | **`RD 0x10`** (16 ASCII) | |
| BATTERY (370) | `id_batteria` | **record 0x76** (16 ASCII) | |
| CUSTOMER (367) | name of `IDCliente` | **PC database** (chosen in download dialog) | panel: own field / none |
| USER (803) | name of `IDOperatore` | **PC database** | panel: own field / none |
| NOTES (395) | `campo_note` | record 0x86 (32 ASCII) | |
| TEST NUMBER (826) | `Pr_test` | record u16 0x70 | `%d` |
| CYCLE NUMBER (372) | `NCiclo` | record 0x41 | `%d` |
| Download Date / Time (344) | `data_scaricoold` | **PC clock at download** | panel: its own RTC |
| Rated Voltage (199/109) | `OLD[0x22]` | record 0x22 | `%d` V |
| Rated Capacity (110) | `OLD.u16[0x1F]` | record 0x1F | `%d` Ah |
| Discharge Current (128) | `CFGOLD+5` | record 0x23 | `%d` A; label becomes "Maximum discharge current" (562) when mode = 7 |
| Discharge Time (130) limit | `CFGOLD+7` | record u16 0xA7 | `h:mm` (`FUN_005d89c0`: `"%d:%2d"`, spaces→'0'); 6000 → `"** : **"` |
| Discharge Stop Voltage (754) | `CFGOLD+0xD` | record 0x25 | `x.yy` (`FUN_005d8b20` `/100`, `"%d%s%2d"`) V/el; mode 7: label "Voltage Reference" (561), `/10` V |
| Modes of Operation (684) | `STModo.Items[CFGOLD+9]` | record 0x1E | list §2.4, e.g. 0 → "TEST -Efficiency Test" |
| Battery Type (884) | `FUN_005c373c(CFGOLD+0x31)` | record 0xB2 | 0 → `"000 - Pb"`, 1 → `"001 - NiCd"`, else empty |
| BLT model | `CBcfgBLT` from `CFGOLD+0x2F` bits 1..4 | **live `RD 0x100` +0x2F** at download | |
| Discharge Start (166) | `FUN_005d9d24(OLD[0x2e],OLD[0x2f],OLD[0x30]) + " " + FUN_005d9e48(OLD[0x2d],OLD[0x2c])` | record 0x2E,0x2F,0x30 / 0x2D,0x2C (BCD) | `DD/MM/YY hh:mm`, 2-digit zero-padded |
| Discharge Time (130) | `FUN_005d89c0(OLD.u16[0x3a])` | record u16 0x3A | `h:mm` |
| Pause Time After Discharge (163) | `FUN_005d89c0(OLD.u16[0x3e])` | record u16 0x3E | `h:mm` |
| Capacity at Cycle Start (167) | `OLD.u16[0x42]`, `FUN_005923fc(0x42, 0x1F)` | record 0x42, 0x1F | `%d` Ah, `%.1f` % = Ah·100.0/AhNom (0 if AhNom = 0) |
| Discharged Capacity (379) | `OLD.u16[0x4e]`, % | record 0x4E, 0x1F | same |
| Capacity at Discharge End (168) | `OLD.u16[0x44]`, % | record 0x44, 0x1F | same |
| Efficiency (745) | `OLD[0x52]` if `< 0xFB` else `"***"` | record 0x52 | `%d` % |
| Minimum Voltage (159) | `FUN_005d8b20(OLD.u16[0x53])` | record u16 0x53 | `x.yy` V/el |
| Maximum Temp. on Electronic Card (747) | `(char)OLD[0x5b]` | record 0x5B (s8) | `%d` °C |
| Battery Max. Temperature (748) | `(char)OLD[0x5c]` | record 0x5C | only if `CFG[0x37]&1` |
| Active Section Max Temp. (749) | `(char)OLD[0x5e]` | record 0x5E | only if `CFG[0x37]&2` |
| FAILURES (401) | `LBAnomalie` items | record **0x65..0x6A** | §2.3.1 |
| charge block (Dati di Carica) | 0x31..0x35, 0x36, 0x38, 0x3C, 0x44, 0x4A, 0x4C, 0x46, 0x55, 0x59, 0x57, 0x5D, `CFG+0x13` | record | printed only if visible (§2.3.2) |

Evidence: `AggPaginaDatiOld` 0x599914, e.g.
`FUN_005d9d24(OLD[0x2e],OLD[0x2f],OLD[0x30]); … FUN_005d9e48(OLD[0x2d],OLD[0x2c],&local_114); FUN_00455dbc(OldSTInfo_DataInizioScarica, …)`,
`FUN_005d89c0(*(u16*)(OLD+0x3a)) → OldSTInfo_timer_scar`, `FUN_005d89c0(*(u16*)(OLD+0x3e)) → OldSTInfo_timer_scar_car`,
`FUN_005923fc(*(u16*)(OLD+0x42), *(u16*)(OLD+0x1f)) → OldSTInfo_ah_batt_inizio_ciclop`,
`if ((byte)OLD[0x52] < 0xfb) … StaticText15 else "***"`, `FUN_005d8b20(*(u16*)(OLD+0x53)) → OldSTInfo_tens_min`,
`FUN_00409d88((int)(char)OLD[0x5b]) → OldSTInfo_TempIntMaxScar`.
`AggPaginaParametri` 0x59ba48: `STModo.ItemIndex = *(u16*)(CFGOLD+9)`, `Edit9 = CFGOLD+5`,
`if (*(short*)(CFGOLD+7)==6000) STTS="** : **" else FUN_005d89c0(CFGOLD+7)`,
`if (mode==7) FUN_005d8a60(CFGOLD+0xd) else FUN_005d8b20(CFGOLD+0xd)`.
`FUN_005d9e48` 0x5d9ecf: `PUSH [hour-string]; PUSH ":"; PUSH [minute-string]; LStrCatN` → "hh:mm".
`FUN_005923a8` 0x5923b9: `if (nom > 0) pct = (float)ah * 100.0 / nom`; printed with `"%.1f"` (0x59244c).

#### 2.3.1 Failures list [proven, `AggPaginaDatiOld` six loops at 0x59ad9c..0x59afcb, `FUN_0058cbcc` = bit test `(x & (1<<(k-1)))`]
```c
for (byte b = 0; b < 6; b++)            // REC[0x65 + b]  (byte_mem_anom0..5)
  for (k = 1; k <= 8; k++)
    if (REC[0x65+b] & (1 << (k-1)))  add(FUN_0057be60(8*b + k));   // text from TabAnomalie
```
Ids/texts: BLT_REGISTERS §3 table (1 EEPROM DIFETTOSA … 41 STOP TEST DA BATT). The print has 48 slots
(`xSOldAnom01…`). The page label is "Active Anomalies List" (750) but the bytes are the memorised ones.

#### 2.3.2 Which sections are printed [proven, `ScarCarOffLineVISIBLE` 0x599820]
```c
if (CFGOLD.u16[0xb] == 0 && LBCicli.Count < 2) { dis = 1; chg = 0; }   // CFGOLD+0xB = programmed cycles (rec 0xA6)
else if (OLD[0x41] == 0) { dis = 0; chg = 1; } else { dis = 1; chg = 1; }
if (!OLD[0x2e] || !OLD[0x2f] || !OLD[0x30]) dis = 0;   // no discharge start date
if (!OLD[0x33] || !OLD[0x34] || !OLD[0x35]) chg = 0;   // no charge start date
GroupBox6 (discharge).Visible = dis; GroupBox8 (charge).Visible = chg;
```
The print passes both flags to `func_0x00582a3c` (0x5831b8 end). For an efficiency test (mode 0, no charge)
only "DISCHARGE DATA" appears because the charge date bytes are zero.

### 2.4 Mode list (`STModo` items, `FUN_0058ec9c`) [proven]
| idx | IT (source) | EN (STRINGUK) |
|---|---|---|
| 0 | TEST -Test Efficienza | TEST -Efficiency Test |
| 1 | CICLI-Cicli Liberi | CYCL.-Free Cycles |
| 2 | TEST5-Test Efficienza 5h | TEST5-Efficiency test in 5h |
| 3 | TES10-Test Efficienza 10h | TES10-Efficiency test in 10h |
| 4 | C+T5 -N-1 Lib.+Test Eff.5h | C+T5 -N-1 Free test+Test Eff.5h |
| 5 | C+T10-N-1 Lib.+Test Eff.10h | C+T10-N-1 Free test+Test Eff.10h |
| 6 | ICOST corrente costante | ICOST constant current |
| 7 | VCOST tensione costante | VCOST constant voltage |

---

## 3. Graph page (0x80 bytes at `0x10000 + slot*0x80`)

### 3.1 Layout
| off | size | meaning | evidence |
|---|---|---|---|
| 0x00 | BCD | seconds of first sample (ignored by BLTView) | [FW] `FUN_01226a`: RAM `0x0C0E..0x0C14` (RTC ss mm hh dow DD MM YY) → `0x0700..0x0706` |
| 0x01 | BCD | minute of first sample | [proven] `FUN_0057c854` 0x57c8b6 `MOVZX [PAGE+1]` → EncodeDateTime minute |
| 0x02 | BCD | hour | 0x57c8a7 `[PAGE+2]` → hour |
| 0x03 | BCD | day of week (ignored) | [FW] |
| 0x04 | BCD | day | 0x57c8c9 `[PAGE+4]` |
| 0x05 | BCD | month | 0x57c8d8 `[PAGE+5]` |
| 0x06 | BCD | year − 2000 | 0x57c8e7 `[PAGE+6]`, `ADD AX,0x7d0` |
| 0x07 + 6k | u16 | sample k: **tag = Pr_ciclo of the running cycle** (`RI.6E` when sampled) | [FW] `FUN_012346`: `MOVFF 0x0a6e,POSTINC0` into `0x0707+6k`; [proven] stored as TabGrafico `NumeroCiclo` (0x57cbf1) |
| 0x09 + 6k | u16 | sample k: **battery voltage, 0.1 V** (copy of `RI.u16[2]`) | [FW] `MOVFF 0x0a02` at 0x01235e; [proven] `"Tens" = PAGE.u16[9+6k]*10`, shown `/100` |
| 0x0B + 6k | s16 | sample k: **current, 0.1 A**, discharge negative, charge positive | [FW] from `0x0368` = −discharge or +`RI[6]` (0x7326..0x7346); [proven] `"Corr" = Round((short)PAGE[0xb+6k])`, shown `/10` |
| 0x7F | u8 | **sample interval, minutes** (binary; copy of settings `+0x1B`) | [FW] `MOVFF 0x079f,0x077f` 0x01210c; [proven] `IncMinute(dt, PAGE[0x7f])` (0x57cc82, FMUL/FDIV 1440.0) |

k = 0..19 (7 + 20·6 = 0x7F). Unused samples are 0xFFFF in V and I [FW `FUN_012392`: SETF at +2/+4].
BLTView stops a page at the first sample with V = 0xFFFF (`CMP word ptr [EDX+EAX*2+0x9],0xffff` at 0x57c92c).
Its second test (`MOVSX` of I compared with `0xffff` at 0x57c910) can never be true (sign-extended −1 ≠ 0xFFFF).

### 3.2 How pages are written [FW, `FUN_012078` / `FUN_012108`]
* A seconds counter (`0x00F8`) is compared with `CFG+0x1B × 60`; on expiry one sample is appended (states 1–4 =
  discharge/pause/charge/pause, or 8 with a flag). The first sample of a page also copies the RTC into the header.
* After **every** sample the whole page buffer (RAM 0x0700..0x077F) is written to slot `RI.u32[0x72] mod 250`
  (`MOVLW #0xfa`, ×128 by 7 shifts, +0x10000). When the page holds 20 samples, `RI.u32[0x72]++` and the buffer's
  samples are reset to 0xFFFF.
* Therefore: pages `< RI.72` are complete; page `RI.72` is the one being filled. Right after the increment its slot
  still holds the page from 250 pages earlier until the next sample is written [inferred from the order of operations].

### 3.3 How BLTView links pages to a test [proven]
* Download (`FUN_0057c854`): each valid sample k becomes one TabGrafico row
  `IDSmartic=serial, NumeroCiclo=PAGE.u16[7+6k], Indice=absolute page no., Campione=k,
  day/month/year/hour/min = header + k·PAGE[0x7F] min, sec = BCD-decode(PAGE[0x7F]) (BLTView quirk), Tens=V·10, Corr=I`.
  It is inserted only if `Locate("IDSmartic;NumeroCiclo;Indice;Campione")` fails, so re-reading the partial page adds
  only the new samples.
* Display (`TFOffLine.CaricaStrutturaGraficoNEW` 0x59ce98, used by the "Grafico" tab and its print):
  ```
  "select * from TabGrafico where IDSmartic = '" + serial + "' and NumeroCiclo = " + IntToStr(OLD.u16[0x6e]) + " order by Indice,campione"
  ```
  → the diagram of a cycle = samples whose tag equals that record's **Pr_ciclo** (`0x6E`). Max 54 000 rows (older
  ones dropped). Nothing in the page names the test number; the test link is via the cycle record.
  A page can hold samples of two consecutive cycles (tag is per sample).

### 3.4 Voltage diagram axes and reference lines [proven, `CaricaStrutturaGraficoNEW`]
* X = `EncodeDate(year,month,day) + EncodeTime(hour,min,0,0)` of each sample (absolute date-time; axis label via
  `GraficoTENSGetAxisLabel` 0x5a4488 = DecodeDateTime → time and date strings).
* Voltage series 0: `Tens / 100.0` V (0x59e54c = 100.0). Current chart series 0: `Corr / 10.0` A (0x59e538 = 10.0).
* Constant lines (same value at every X):
  ```c
  _DAT_00794e94 = (double)OLD[0x22];                                         // series 1: Rated Voltage
  _DAT_00794e8c = (float)(OLD[0x22] * CFGOLD.u16[0x1d]) / 200.0;             // series 2: Voltage Threshold
  _DAT_00794eac = (float)(OLD[0x22] * CFGOLD.u16[0xd])  / 200.0;             // series 3: Discharge Stop Voltage
  _DAT_00794ea8 = -CFGOLD.u16[5];                                             // current chart series 1: −discharge current
  ```
  With the record: Rated = `rec[0x22]` V; Threshold = `rec[0x22]·rec[0x24]/200` V; Stop = `rec[0x22]·rec[0x25]/200` V;
  current line = `−rec[0x23]` A. (`/200` = cV per 2-V cell × number of cells / 100.) The live chart uses exactly these
  record bytes (`FUN_005bcea4`: `RI[0x24]/200.0*RI[0x22]`, `RI[0x25]/200.0*RI[0x22]`, `-RI[0x23]`; constant
  `_DAT_005be4dc = 200.0`).
  BLTView applies the `/200` formula also in mode 7, where `CFG+0xD` is an absolute voltage [proven, no mode test in
  this function]; the older `CaricaStrutturaGrafico` 0x59c798 used `(CFG+0x1D + 0x32)` instead.
* Y range = min/max of samples and lines. Legend texts: "Rated Voltage" (199), "Voltage Threshold" (470),
  "Voltage Diagram (V)" (226), "Current Diagram (A)" (225).

---

## 4. Reproducing the download on the ESP32

* **Commands**: `RI` (addr+len ≤ 0xC0) and `RD` only. Frames: max 0x40 payload bytes (firmware clamps `LLLL` to 0x40
  [FW 0x3536]). Address field is 24-bit (`AAAAAA`), e.g.
  `@63RD000F00004021\r`, `@63RD000F40004025\r`, `@63RD000F80004029\r` (slot 0);
  `@63RD010000004056\r`, `@63RD010040004052\r` (page slot 0). Reply = `!63RD` + echoed header + 128 hex + XX + CR
  (`2·0x40+15 = 143` chars before CR).
* **Rings**: cycle slots `0xF00 … 0x7F7F` (150 × 0xC0); graph slots `0x10000 … 0x17CFF` (250 × 0x80). Indices in RI
  are absolute counters; slot = index mod 150 / mod 250. `RD` range limit `addr16+len ≤ 0x17FFF` holds for both.
* **Which cycles**: absolute `n ∈ [ptrCyc − nAct + 1, ptrCyc]` (this is BLTView's "missing cycles" result with an empty
  database). After `RE 0x01` (reset) `ptrCyc` restarts at 0/1 and `nAct` at 0/1. Validate each record [inferred]:
  `rec.u16[0x6E] == n & 0xFFFF` (stale or erased slot otherwise), and not all 0xFF. The slot `ptrCyc` is in progress
  unless `rec[0x6B]` is 5 (FINE TEST) or 0 after a stop; BLTView stores it anyway and overwrites it later.
* **Which pages**: absolute `p ∈ [max(0, ptrPage − 249), ptrPage]`; for a given record keep samples with
  `tag == rec.u16[0x6E]`. To limit traffic, read pages from newest backwards and stop once a whole page has only tags
  `< rec.0x6E` [inferred: tags are non-decreasing in time]. Graph covers at most 250 × 20 samples
  (e.g. 5000 × 6 min ≈ 20.8 days at the default interval).
* **Timing** [proven for BLTView, inferred as safe for the panel]: BLTView reads one record or one page per poll tick
  (≥ 900 ms) interleaved with the `RI` poll; 150 records ≈ 2.3 min, 251 pages ≈ 3.8 min. Each frame gets 2 attempts;
  read timeout is set to 2501 ms per byte (effectively "wait"). For the panel: one request in flight, ~500 ms reply
  timeout, 2 retries, a short gap (≥ 20 ms) between frames. The I²C EEPROM read in the firmware is per byte with
  6 retries [FW 0x125CA]; no busy/NAK was seen for reads except range/I²C errors (`!63EE..`).
* **Consistency**: the tester keeps running while you read. A record or page may change between its frames
  (the current slot is rewritten on every state change / sample). Re-read the current slot and the last page at the end,
  or compare `RI.6E`/`RI.72` before and after.
* **Not needed for the report**: `RE 0x0C`, any `WD`/`WC`. Customer/operator/download time are PC-side values in
  BLTView; the panel must supply its own (or print blanks).

---

## 5. Open points
1. **[inferred]** `0x53/0x55` units (V/el ×100) — only the label "(V/el)" says so.
2. **[inferred]** NiCd rated voltage at `rec+0xB3` is 0.1 V (by analogy with `CFG+1`); BLTView's lines still use `rec[0x22]`.
3. **[open]** Erased-EEPROM content of never-written slots (0xFF assumed).
4. **[inferred]** The firmware's exact behaviour of the slot at `RI.6E` after a test is stopped (record re-initialised
   and written by `FUN_011f82` before `Pr_test++` at 0x2060..0x2066) — validate by `0x6E` and dates rather than trust it.
5. BLTView's stored `sec` column in TabGrafico is `BCD-decode(PAGE[0x7F])` while the time step uses the raw byte;
   identical for intervals < 10 min. Use the raw byte (minutes, binary).

Scratch evidence (session scratchpad): `all.c` (full decompile), `rec1.c` (`FUN_005b6a14`, `FUN_0057a49c`,
`FUN_00579930`), `rec2.c` (`CaricaStrutturaGrafico`, `PageControlChange`, `BStampaClick`, axis label, print forms,
`TFClienti`), `agg_a.c` (`AggPaginaDatiOld` annotated), `csgn.c` (`CaricaStrutturaGraficoNEW`), `d3.asm`/`d5.asm`/`d7.asm`
(disassembly), `fw/listing.txt` (firmware).

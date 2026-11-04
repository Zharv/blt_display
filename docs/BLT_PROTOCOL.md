# BLT160 / SmartIC serial protocol (reversed from BltView.exe 2.6.16)

ASCII, line-oriented, node-addressed, XOR-checksummed. Half-duplex request/response.
All multi-byte fields are **uppercase hex ASCII**, zero-padded (the builder replaces
every space produced by `%x` with `'0'`).

## Serial line
`\\.\COM<n>`, 8N1 typical. App supports 100/300/1200/2400/4800/9600/19200/38400/57600/115200 baud,
parity none/even/odd, 4–8 data bits, 1/1.5/2 stop. Default combo = 300 baud. Read timeout
= (param+1) ms/byte, write timeout const 1000 ms. `PurgeComm(TX)` before every send.
(open routine FUN_005d7890; send FUN_005d779c; recv-byte FUN_005d7808)

## Request frame  (host -> device)   builder FUN_005d7e8c
```
'@'  NN  CC  AAAAAA  LLLL  [DD...]  XX  '\r'
```
| field | size | meaning | format |
|-------|------|---------|--------|
| `@`   | 1  | start of request (0x40) | literal |
| NN    | 2  | node / device address   | `%2x` (default 0x63) |
| CC    | 2  | command, 2 ASCII letters | `%s` |
| AAAAAA| 6  | address / parameter      | `%6x` |
| LLLL  | 4  | payload length N (bytes) | `%4x` |
| DD    | 2·N| payload, one byte = 2 hex | `%2x` each |
| XX    | 2  | checksum                 | `%2x` |
| `\r`  | 1  | end (0x0D)               | literal |

Header format string in binary: `@%2x%s%6x%4x`; per-byte payload: `%2x`.

**Checksum** = XOR of every ASCII char from `@` up to (not incl.) XX, low byte, 2 hex.

Two special payload cases in the builder:
- CC=`RE` and AAAAAA=10 -> payload appended as a raw string, not hex bytes.
- CC=`GG` -> 3 payload bytes patched directly at fixed buffer offset 0x0d..0x0f.

## Response frame  (device -> host)   reader FUN_005d7d40, validator FUN_005d81c8
```
'!'  NN  CC  AAAAAA ..(13-char header)..  [DD...]  XX  '\r'
```
- Reader accumulates bytes until `'!'` (0x21) = start, then until `'\r'` (0x0D) = end
  (any `\n`/`\r` inside is stripped).
- **Payload starts at byte offset 13**; total length is validated as `N*2 + 15`
  (13-char echoed header + 2-char checksum).
- `[1:3]` = node echo; if it equals **`"EE"`** the frame is a **NAK / device error**.
- `[3:5]` = command echo, compared against the command that was sent.
- `[13 : 13+2N]` = payload, decoded 2-hex-per-byte (FUN_005d7bf0 / hex2val FUN_005d7b70).
- trailing 2 hex = XOR checksum over the whole frame minus those 2 chars.

### RX/TX error codes (string table, 1-based)
1 PORT NOT OPENED · 2 CREATE HANDLE · 3 GETDCB · 4 SETDCB · 5 SPEED · 6 PARITY ·
7 DATABITS · 8 STOPBITS · 9 TX ERROR · 10 RX TIMEOUT · 11 RX CHAR · 12 RX FRAMELEN ·
13 RX STX(no `!`) · 14 RX NODE · 15 RX CMD(echo mismatch) · 16 RX CHECKSUM ·
17 RX NAK(node==`EE`) · 18 UNDEFINED

## Command set (index -> code)
| idx | code | meaning (inferred) |
|-----|------|--------------------|
| 0 | `RI` | Read Info / identify |
| 1 | `RD` | Read Data (RAM/registers) |
| 2 | `RE` | Read EEPROM (addr=10 -> string form) |
| 3 | `WD` | Write Data |
| 4 | `WC` | Write Config / clock |
| 5 | `CI` | command, **no response validated** |
| 6 | `CP` | command, **no response validated** |
| 7 | `RW` | Read Work/recorder |
| 8 | `GG` | special (3-byte inline payload) |
| 9 | `RB` | Read Block |
|def| `??` | unknown placeholder |

## Transaction flow   FUN_005d8480(node, cmdIdx, addr, respLen, data, len)
1. build frame (node hard-coded 0x63)
2. retry up to 2x: OpenCom -> SendFrame -> RecvResponse -> (if cmd not CI/CP) ValidateResponse
3. counters at DAT_00f68eb0.. track total/ok/fail, last TX/RX, and each error class.
Large transfers: FUN_005d8530 chunks the payload into `(bufsize-0x10)/2`-byte blocks
(buffer 0x2001 = 8193 bytes) and loops per block.

## Worked examples (checksum computed by the same algorithm)
```
Read 0x10 bytes, node 0x63, cmd RD, addr 0x000001:
  @63RD000001001053\r
Write 6 bytes (12 34 09 11 26 00), cmd WD, addr 0:
  @63WD000000000612340911260059\r
```

#!/usr/bin/env python3
"""
In-process simulator of a BLT160 / SmartIC tester, for developing the display and
the session logic without hardware. It speaks the real framing (so blt_client and
the C firmware parser accept its responses), but the REGISTER MAP below is a
development placeholder: the actual on-device addresses/field layout live in the
PIC18 firmware (AP211) and are not reverse-engineered yet.

Usage as a transport for blt_client.BltClient:

    from blt_sim import BltTesterSim
    sim = BltTesterSim()
    cli = blt_client.BltClient(sim, node=0x63)
    r = cli.transact("RD", 0x000000, 0x10)

Run `python tools/blt_sim.py` for a short simulated discharge over the client.
"""
from __future__ import annotations

import struct
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
import blt_client as blt  # noqa: E402

# placeholder RD register map base; 16-byte live block
RD_LIVE_ADDR = 0x000000


class BltTesterSim:
    """Half-duplex transport: write() a request frame, read() the response bytes."""

    def __init__(self, node: int = blt.NODE_DEFAULT):
        self.node = node
        self._rx = bytearray()   # request being accumulated
        self._tx = bytearray()   # response waiting to be read
        # live discharge state (placeholder physics)
        self.v_mv = 53300        # 53.3 V start
        self.i_ma = -12000       # 12 A discharge
        self.cap_mah = 0         # discharged capacity
        self.temp_dc = 265       # 26.5 C
        self.state = 3           # 3 = discharging (matches BLT "Scarica")
        self.cycle = 1

    # --- discharge physics (called by the demo; real device does this itself) ---
    def step(self, dt_s: float):
        self.cap_mah += int(abs(self.i_ma) * dt_s / 3600.0)
        # crude voltage sag with discharged capacity
        self.v_mv = max(42000, 53300 - self.cap_mah // 3)
        self.temp_dc = min(400, 265 + self.cap_mah // 200)
        if self.v_mv <= 42000:
            self.state = 8  # done

    # --- response builders ---
    def _live_block(self) -> bytes:
        return struct.pack("<HhHhBB", self.v_mv, self.i_ma, self.cap_mah,
                           self.temp_dc, self.state, self.cycle) + b"\x00\x00\x00\x00"

    def _payload_for(self, cmd: str, addr: int, nbytes: int) -> bytes:
        if cmd == "RI":
            return b"BLT160\x01\x24"              # id + fw 1.36-ish (placeholder)
        if cmd == "RD":
            blk = self._live_block()
            if addr == RD_LIVE_ADDR:
                return blk[:nbytes] if nbytes <= len(blk) else blk + bytes(nbytes - len(blk))
            return bytes((addr + i) & 0xFF for i in range(nbytes))  # pattern
        if cmd == "RW":                            # one recorder cycle record
            return struct.pack("<HHHB", self.v_mv, abs(self.i_ma), self.cap_mah,
                               self.cycle) + bytes(max(0, nbytes - 7))
        if cmd in ("RE", "RB"):
            return bytes((addr + i) & 0xFF for i in range(nbytes))
        return bytes(nbytes)

    def _build_response(self, cmd: str, addr: int, payload: bytes) -> bytes:
        head = "!%02X%s%06X%02X" % (self.node & 0xFF, cmd, addr & 0xFFFFFF, 0)
        body = bytearray(head.encode("ascii"))
        body += "".join("%02X" % b for b in payload).encode("ascii")
        body += ("%02X" % blt.checksum(bytes(body))).encode("ascii")
        body += b"\r"
        return bytes(body)

    def _handle_request(self, frame: bytes):
        # minimal request decode (mirror of blt_client.build_frame layout)
        f = frame.rstrip(b"\r")
        if len(f) < 15:
            return
        try:
            cmd = f[3:5].decode("ascii")
            addr = int(f[5:11], 16)
            nbytes = int(f[11:15], 16)
        except Exception:
            return
        payload = self._payload_for(cmd, addr, nbytes)
        self._tx += self._build_response(cmd, addr, payload)

    # --- transport interface used by BltClient ---
    def reset_input_buffer(self):
        self._tx.clear()

    def write(self, data: bytes):
        for b in bytes(data):
            self._rx.append(b)
            if b == 0x0D:  # CR ends a request
                self._handle_request(bytes(self._rx))
                self._rx.clear()

    def read(self, n: int = 1) -> bytes:
        if not self._tx:
            return b""
        take = self._tx[:n]
        del self._tx[:n]
        return bytes(take)


def _demo():
    sim = BltTesterSim()
    cli = blt.BltClient(sim, node=sim.node, timeout=0.5)

    rid = cli.transact("RI", 0, 0, b"")
    print("ID:", rid["payload"])

    print("\n t(s)   V(V)   I(A)   Ah     T(C)  state")
    t = 0.0
    for _ in range(12):
        r = cli.transact("RD", RD_LIVE_ADDR, 0x10)
        if r["err"] != 0:
            print("  error:", blt.ERRORS[r["err"]]); break
        v, i, cap, temp, state, cycle = struct.unpack("<HhHhBB", r["payload"][:10])
        print(" %4.0f  %5.2f  %5.1f  %4d  %5.1f   %d"
              % (t, v / 1000, i / 1000, cap, temp / 10, state))
        sim.step(60.0)
        t += 60.0


if __name__ == "__main__":
    _demo()

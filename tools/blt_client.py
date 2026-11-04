#!/usr/bin/env python3
"""
Alfa Progetti BLT160 / SmartIC protocol, pure-Python mirror of firmware/components/blt_proto.
Used as the PC-side client and as the independent oracle for the C host tests.

Frame (see docs/BLT_PROTOCOL.md): '@' NN CC AAAAAA LLLL [DD...] XX '\r'
All hex fields UPPERCASE, zero-padded. XX = XOR of every byte from '@' up to XX.
"""
from __future__ import annotations

CMDS = ["RI", "RD", "RE", "WD", "WC", "CI", "CP", "RW", "GG", "RB"]
NODE_DEFAULT = 0x63

ERRORS = {
    0: "OK", 1: "PORT NOT OPENED", 2: "CREATE HANDLE", 3: "GETDCB", 4: "SETDCB",
    5: "SPEED", 6: "PARITY", 7: "DATABITS", 8: "STOPBITS", 9: "TX ERROR",
    10: "RX TIMEOUT", 11: "RX CHAR", 12: "RX FRAMELEN", 13: "RX STX",
    14: "RX NODE", 15: "RX CMD", 16: "RX CHECKSUM", 17: "RX NAK", 18: "UNDEFINED",
}


def checksum(data: bytes) -> int:
    x = 0
    for b in data:
        x ^= b
    return x & 0xFF


def build_frame(node: int, cmd: str, addr: int, nbytes: int, data: bytes = b"") -> bytes:
    """Build a request frame. For a read, pass nbytes and data=b''."""
    if cmd not in CMDS:
        raise ValueError(f"unknown command {cmd!r}")
    raw = (cmd == "RE" and addr == 10)
    head = "@%02X%s%06X%04X" % (node & 0xFF, cmd, addr & 0xFFFFFF, nbytes & 0xFFFF)
    body = bytearray(head.encode("ascii"))
    if data:
        if raw:
            body += data
        else:
            body += "".join("%02X" % b for b in data).encode("ascii")
    x = checksum(bytes(body))
    body += ("%02X" % x).encode("ascii")
    body += b"\r"
    return bytes(body)


def parse_response(frame: bytes, sent_cmd: str, sent_node: int = NODE_DEFAULT) -> dict:
    """Validate + decode a response. Returns dict with 'err' (int) and decoded fields."""
    f = frame.rstrip(b"\r\n")
    r = {"err": 0, "is_nak": False, "node_echo": None, "cmd_echo": None,
         "hdr_extra": b"", "payload": b"", "checksum_ok": False}
    if len(f) < 15 or (len(f) - 15) % 2 != 0:
        r["err"] = 12
        return r
    if f[0:1] != b"!":
        r["err"] = 13
        return r
    if f[1:3] == b"EE":
        r["is_nak"] = True
        r["err"] = 17
        return r
    try:
        node_echo = int(f[1:3], 16)
    except ValueError:
        r["err"] = 11
        return r
    r["node_echo"] = node_echo
    if node_echo != (sent_node & 0xFF):
        r["err"] = 14
        return r
    r["cmd_echo"] = f[3:5].decode("ascii", "replace")
    if f[3:5] != sent_cmd.encode("ascii"):
        r["err"] = 15
        return r
    try:
        int(f[5:], 16)
    except ValueError:
        r["err"] = 11
        return r
    want = int(f[-2:], 16)
    got = checksum(f[:-2])
    r["checksum_ok"] = (got == want)
    r["hdr_extra"] = bytes(int(f[5 + 2 * i:7 + 2 * i], 16) for i in range(4))
    n = (len(f) - 15) // 2
    r["payload"] = bytes(int(f[13 + 2 * i:15 + 2 * i], 16) for i in range(n))
    if not r["checksum_ok"]:
        r["err"] = 16
    return r


class BltClient:
    """Thin half-duplex client over any transport with read(n)/write(b)/reset_input()."""

    def __init__(self, transport, node: int = NODE_DEFAULT, timeout: float = 1.0):
        self.t = transport
        self.node = node
        self.timeout = timeout

    def transact(self, cmd: str, addr: int, nbytes: int, data: bytes = b"") -> dict:
        frame = build_frame(self.node, cmd, addr, nbytes, data)
        if hasattr(self.t, "reset_input_buffer"):
            self.t.reset_input_buffer()
        self.t.write(frame)
        resp = bytearray()
        import time
        deadline = time.time() + self.timeout
        while time.time() < deadline:
            b = self.t.read(1)
            if not b:
                continue
            if not resp and b != b"!":
                continue  # wait for STX
            resp += b
            if b == b"\r":
                break
        return parse_response(bytes(resp), cmd, self.node)


if __name__ == "__main__":
    # quick self-check against the two hand-verified frames
    assert build_frame(0x63, "RD", 0x000001, 0x10) == b"@63RD000001001053\r"
    assert build_frame(0x63, "WD", 0x000000, 6, bytes([0x12, 0x34, 0x09, 0x11, 0x26, 0x00])) \
        == b"@63WD000000000612340911260059\r"
    print("blt_client self-check OK")

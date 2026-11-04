#!/usr/bin/env python3
"""
Emit request-frame test vectors for the C host test. Each line:

    <CMD> <node_hex> <addr_hex> <nbytes_hex> <data_hex_or_dash> <expected_ascii>

`expected_ascii` is produced by tools/blt_client.py (an independent implementation
of the same spec), so a match with firmware/components/blt_proto is a real
cross-implementation check. The first two cases are the hand-verified frames.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import blt_client as blt  # noqa: E402

# (cmd, node, addr, nbytes, data)
CASES = [
    ("RD", 0x63, 0x000001, 0x10, b""),                                   # verified literal
    ("WD", 0x63, 0x000000, 6, bytes([0x12, 0x34, 0x09, 0x11, 0x26, 0x00])),  # verified literal
    ("RI", 0x63, 0x000000, 0x00, b""),
    ("RE", 0x63, 0x000005, 0x08, b""),
    ("RW", 0x63, 0x000100, 0x20, b""),
    ("RB", 0x63, 0x001000, 0x40, b""),
    ("WC", 0x63, 0x000000, 0x06, bytes([0x20, 0x26, 0x11, 0x04, 0x0C, 0x00])),  # clock-like
    ("WD", 0x01, 0x00ABCD, 3, bytes([0xDE, 0xAD, 0xBE])),                # node + addr variation
    ("RD", 0x63, 0xFFFFFF, 0x01, b""),                                   # max addr
]


def main() -> int:
    for cmd, node, addr, nbytes, data in CASES:
        frame = blt.build_frame(node, cmd, addr, nbytes, data)
        expected = frame.rstrip(b"\r").decode("ascii")
        dh = data.hex().upper() if data else "-"
        print("%s %X %X %X %s %s" % (cmd, node, addr, nbytes, dh, expected))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

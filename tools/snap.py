#!/usr/bin/env python3
"""Grab a screenshot of the running panel over the console UART (see firmware/main/snap.h).

    python tools/snap.py COM8 out.png [--tab N] [--log SECONDS]

--tab N   switch to tab N (0 OnLine, 1 Archive, 2 BMS, 3 Printer, 4 Settings) first
--chart M OnLine chart mode first (0 discharge, 1 cell imbalance)
--log S   just print the console for S seconds (no snapshot)
Needs pyserial (the ESP-IDF Python env has it). PNG is written with stdlib zlib.
"""
import argparse
import struct
import sys
import time
import zlib

import serial


def open_port(name, baud=115200):
    s = serial.Serial()
    s.port = name
    s.baudrate = baud
    s.timeout = 0.2
    s.rts = False          # don't reset the board on open
    s.dtr = False
    s.open()
    return s


def read_line(s, deadline):
    buf = bytearray()
    while time.time() < deadline:
        c = s.read(1)
        if not c:
            continue
        if c == b"\n":
            return buf.decode("latin-1").strip()
        buf += c
    return None


def decode(data, w, h):
    px = []
    i = 0
    n = len(data)
    while i < n and len(px) < w * h:
        hb = data[i]
        i += 1
        if hb & 0x80:
            v = data[i] | (data[i + 1] << 8)
            i += 2
            px.extend([v] * ((hb & 0x7F) + 1))
        else:
            cnt = hb + 1
            for k in range(cnt):
                px.append(data[i + 2 * k] | (data[i + 2 * k + 1] << 8))
            i += 2 * cnt
    if len(px) < w * h:
        px.extend([0] * (w * h - len(px)))
    return px


def write_png(path, px, w, h):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = px[y * w:(y + 1) * w]
        for v in row:
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            raw += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("out", nargs="?", default="snap.png")
    ap.add_argument("--tab", type=int)
    ap.add_argument("--chart", type=int)
    ap.add_argument("--log", type=float)
    ap.add_argument("--cmd", help="send this console command first (e.g. BMS, BMSOFF)")
    a = ap.parse_args()

    s = open_port(a.port)
    if a.cmd:
        s.write(b"\n" + a.cmd.encode() + b"\n")
    if a.log:
        end = time.time() + a.log
        while time.time() < end:
            d = s.read(4096)
            if d:
                sys.stdout.write(d.decode("utf-8", "replace"))
                sys.stdout.flush()
        return 0

    if a.tab is not None:
        s.write(b"\nTAB %d\n" % a.tab)
        time.sleep(1.5)
    if a.chart is not None:
        s.write(b"\nCHART %d\n" % a.chart)
        time.sleep(1.0)
    s.reset_input_buffer()
    s.write(b"\nSNAP\n")

    deadline = time.time() + 15
    baud = None
    while baud is None:
        line = read_line(s, deadline)
        if line is None:
            print("no SNAP-BAUD reply", file=sys.stderr)
            return 1
        if line.startswith("SNAP-ERR"):
            print(line, file=sys.stderr)
            return 1
        if line.startswith("SNAP-BAUD"):
            baud = int(line.split()[1])
    time.sleep(0.15)
    s.baudrate = baud

    deadline = time.time() + 10
    while True:
        line = read_line(s, deadline)
        if line is None:
            print("no SNAP-BEGIN", file=sys.stderr)
            s.baudrate = 115200
            return 1
        if "SNAP-BEGIN" in line:
            _, w, h, ln = line[line.index("SNAP-BEGIN"):].split()
            w, h, ln = int(w), int(h), int(ln)
            break

    s.timeout = 1.0
    data = bytearray()
    t0 = time.time()
    while len(data) < ln and time.time() - t0 < 120:
        data += s.read(min(65536, ln - len(data)))
    s.baudrate = 115200
    s.close()
    if len(data) < ln:
        print("short read %d/%d" % (len(data), ln), file=sys.stderr)
    write_png(a.out, decode(bytes(data), w, h), w, h)
    print("%s: %dx%d, %d bytes in %.1f s" % (a.out, w, h, ln, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())

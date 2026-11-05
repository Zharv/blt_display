#!/usr/bin/env python3
"""
Wrap an ESP32-C6 co-processor (ESP-Hosted slave) app image for staging in the
P4's "storage" partition, where firmware/main/cp_update.c picks it up.

Layout: b"CPFW" | u32 len | u32 crc32(image) | u32 0 | image

    python tools/make_cpfw.py coprocessor_c6/build/blt_c6_hosted.bin cpfw.bin
Then flash cpfw.bin at the storage partition offset (0x810000 here).
"""
import struct
import sys
import zlib


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    img = open(src, "rb").read()
    if not img or img[0] != 0xE9:
        print(f"{src}: not an ESP app image (first byte {img[:1].hex()})")
        return 1
    crc = zlib.crc32(img) & 0xFFFFFFFF   # same as esp_rom_crc32_le(0, ...)
    hdr = b"CPFW" + struct.pack("<III", len(img), crc, 0)
    with open(dst, "wb") as f:
        f.write(hdr + img)
    print(f"wrote {dst}: image {len(img)} bytes, crc32 {crc:08x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

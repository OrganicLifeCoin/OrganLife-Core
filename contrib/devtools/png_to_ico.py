#!/usr/bin/env python3
"""
Minimal PNG-to-ICO packer (no image decoding).

Usage:
  png_to_ico.py OUT.ico IN1.png [IN2.png ...]

Each input must be a PNG. The PNGs should already be resized to the desired
icon sizes (e.g. 16x16, 32x32, 48x48, 256x256). This tool simply packs the PNG
payloads into a multi-resolution .ico container.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def read_png_size(png_bytes: bytes) -> tuple[int, int]:
    if not png_bytes.startswith(PNG_SIGNATURE):
        raise ValueError("Not a PNG (bad signature)")
    if len(png_bytes) < 24:
        raise ValueError("PNG too small")
    # IHDR chunk starts at byte 8. Format: length(4), type(4), data(13), crc(4)
    # Width/height are the first 8 bytes of IHDR data.
    if png_bytes[12:16] != b"IHDR":
        raise ValueError("PNG missing IHDR at expected offset")
    width = struct.unpack(">I", png_bytes[16:20])[0]
    height = struct.unpack(">I", png_bytes[20:24])[0]
    return width, height


def ico_dim_byte(dim: int) -> int:
    if dim < 1 or dim > 256:
        raise ValueError(f"Invalid icon dimension: {dim}")
    return 0 if dim == 256 else dim


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("Usage: png_to_ico.py OUT.ico IN1.png [IN2.png ...]", file=sys.stderr)
        return 2

    out_path = Path(argv[1])
    in_paths = [Path(p) for p in argv[2:]]

    images: list[tuple[int, int, bytes]] = []
    for p in in_paths:
        data = p.read_bytes()
        w, h = read_png_size(data)
        images.append((w, h, data))

    # ICONDIR header
    # reserved(0), type(1=icon), count
    header = struct.pack("<HHH", 0, 1, len(images))

    entries: list[bytes] = []
    payloads: list[bytes] = []

    offset = 6 + (16 * len(images))
    for w, h, data in images:
        entry = struct.pack(
            "<BBBBHHII",
            ico_dim_byte(w),
            ico_dim_byte(h),
            0,  # color count
            0,  # reserved
            1,  # planes
            32,  # bit count
            len(data),
            offset,
        )
        entries.append(entry)
        payloads.append(data)
        offset += len(data)

    out_path.write_bytes(header + b"".join(entries) + b"".join(payloads))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))


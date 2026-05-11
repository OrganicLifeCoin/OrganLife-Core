#!/usr/bin/env python3
"""
Minimal PNG-to-ICNS packer (no image decoding).

Usage:
  png_to_icns.py OUT.icns IN1.png [IN2.png ...]

Each input must be a PNG that is already resized to a valid macOS icon size.
The PNG payloads are packed into an .icns container using modern chunk types.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

TYPE_BY_SIZE: dict[tuple[int, int], bytes] = {
    (16, 16): b"icp4",
    (32, 32): b"icp5",
    (64, 64): b"icp6",
    (128, 128): b"ic07",
    (256, 256): b"ic08",
    (512, 512): b"ic09",
    (1024, 1024): b"ic10",
}


def read_png_size(png_bytes: bytes) -> tuple[int, int]:
    if not png_bytes.startswith(PNG_SIGNATURE):
        raise ValueError("Not a PNG (bad signature)")
    if len(png_bytes) < 24:
        raise ValueError("PNG too small")
    if png_bytes[12:16] != b"IHDR":
        raise ValueError("PNG missing IHDR at expected offset")
    width = struct.unpack(">I", png_bytes[16:20])[0]
    height = struct.unpack(">I", png_bytes[20:24])[0]
    return width, height


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("Usage: png_to_icns.py OUT.icns IN1.png [IN2.png ...]", file=sys.stderr)
        return 2

    out_path = Path(argv[1])
    in_paths = [Path(p) for p in argv[2:]]

    chunks: list[bytes] = []
    seen_types: set[bytes] = set()

    for p in in_paths:
        data = p.read_bytes()
        w, h = read_png_size(data)
        t = TYPE_BY_SIZE.get((w, h))
        if t is None:
            raise ValueError(f"Unsupported icon PNG size: {w}x{h} ({p})")
        if t in seen_types:
            continue
        seen_types.add(t)
        chunks.append(t + struct.pack(">I", 8 + len(data)) + data)

    body = b"".join(chunks)
    out_path.write_bytes(b"icns" + struct.pack(">I", 8 + len(body)) + body)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))


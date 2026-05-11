#!/usr/bin/env python3
import struct
import sys
import zlib


def chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack("!I", len(data))
        + tag
        + data
        + struct.pack("!I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def clamp(x: int) -> int:
    if x < 0:
        return 0
    if x > 255:
        return 255
    return x


def write_png(path: str, width: int, height: int) -> None:
    raw = bytearray()
    for y in range(height):
        t = y / max(1, height - 1)
        # Base gradient (white -> subtle gray)
        r0, g0, b0 = 255, 255, 255
        r1, g1, b1 = 241, 245, 249
        r = int(r0 + (r1 - r0) * t)
        g = int(g0 + (g1 - g0) * t)
        b = int(b0 + (b1 - b0) * t)

        raw.append(0)  # filter type 0 (None)
        for x in range(width):
            dx = x / max(1, width - 1)
            navy = (30, 58, 138)  # #1E3A8A
            red = (178, 34, 52)  # #B22234
            strength = 0.10

            tint_r = navy[0] * (1 - dx) + red[0] * dx
            tint_g = navy[1] * (1 - dx) + red[1] * dx
            tint_b = navy[2] * (1 - dx) + red[2] * dx

            rr = int(r + strength * (tint_r - 128))
            gg = int(g + strength * (tint_g - 128))
            bb = int(b + strength * (tint_b - 128))
            raw.extend((clamp(rr), clamp(gg), clamp(bb)))

    png = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack("!IIBBBBB", width, height, 8, 2, 0, 0, 0)  # RGB8
    png += chunk(b"IHDR", ihdr)
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print("usage: make_background_png.py <out.png> <width> <height>", file=sys.stderr)
        return 2
    out, w, h = argv[1], int(argv[2]), int(argv[3])
    write_png(out, w, h)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))


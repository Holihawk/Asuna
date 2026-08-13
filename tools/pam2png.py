#!/usr/bin/env python3
"""Convert the render test's binary PAM (P7, RGB_ALPHA) output to PNG.

Stdlib only - zlib and struct are enough to emit a valid RGBA PNG, which keeps
the build free of an image-library dependency just for inspecting spike output.

    ./tools/pam2png.py build/phase0.pam build/phase0.png [--checker]

--checker composites the image over a grey checkerboard, which is the only
practical way to see whether the alpha channel is actually correct.
"""

import struct
import sys
import zlib


def read_pam(path):
    with open(path, "rb") as fh:
        blob = fh.read()
    if not blob.startswith(b"P7\n"):
        raise SystemExit(f"{path}: not a binary PAM file")
    end = blob.index(b"ENDHDR\n") + len(b"ENDHDR\n")
    header = {}
    for line in blob[3:end].decode("ascii").splitlines():
        parts = line.split()
        if len(parts) == 2:
            header[parts[0]] = parts[1]
    width, height = int(header["WIDTH"]), int(header["HEIGHT"])
    depth = int(header["DEPTH"])
    if depth != 4:
        raise SystemExit(f"{path}: expected DEPTH 4, got {depth}")
    data = blob[end:end + width * height * 4]
    if len(data) != width * height * 4:
        raise SystemExit(f"{path}: truncated pixel data")
    return width, height, bytearray(data)


def over_checkerboard(width, height, data, size=16):
    """Composite RGBA over a checkerboard so alpha is visible."""
    out = bytearray(len(data))
    for y in range(height):
        row = y * width * 4
        for x in range(width):
            i = row + x * 4
            a = data[i + 3] / 255.0
            bg = 153 if ((x // size) + (y // size)) % 2 == 0 else 102
            for c in range(3):
                out[i + c] = int(data[i + c] * a + bg * (1.0 - a))
            out[i + 3] = 255
    return out


def write_png(path, width, height, data):
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        raw += data[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    checker = "--checker" in sys.argv
    if len(args) != 2:
        raise SystemExit(__doc__)
    width, height, data = read_pam(args[0])
    if checker:
        data = over_checkerboard(width, height, data)
    write_png(args[1], width, height, data)
    print(f"wrote {args[1]} ({width}x{height})")


if __name__ == "__main__":
    main()

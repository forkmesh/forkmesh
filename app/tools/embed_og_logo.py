#!/usr/bin/env python3
"""Regenerate the ForkMesh mark embedded in src/og_card.py.

The social-preview card renderer (og_card.py) draws the logo from a
pre-scaled raw-RGBA blob baked into its source, because the Pyodide worker
has no Pillow and decoding/scaling the full-size PNG per request would be
far too slow. Run this after changing www/public/assets/logo.png:

    python3 tools/embed_og_logo.py

Pure stdlib: a minimal decoder for the one PNG shape logo.png uses
(8-bit RGBA, non-interlaced), an alpha-weighted box downscale, then a
zlib+base64 blob spliced between the _LOGO_B64 markers in og_card.py.
"""

import base64
import re
import struct
import sys
import zlib
from pathlib import Path

WORKER_DIR = Path(__file__).resolve().parents[1]
LOGO_PNG = WORKER_DIR.parent / "www" / "public" / "assets" / "logo.png"
OG_CARD = WORKER_DIR / "src" / "og_card.py"
TARGET_H = 112


def png_decode_rgba8(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos = 8
    idat = b""
    w = h = 0
    while pos < len(data):
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        pos += 8
        chunk = data[pos:pos + length]
        pos += length + 4  # skip CRC
        if ctype == b"IHDR":
            w, h, depth, color, _, _, interlace = struct.unpack(
                ">IIBBBBB", chunk)
            assert (depth, color, interlace) == (8, 6, 0), (
                "embed_og_logo only decodes 8-bit RGBA non-interlaced PNGs; "
                "got depth=%d color=%d interlace=%d" % (depth, color,
                                                        interlace))
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    raw = zlib.decompress(idat)
    stride = w * 4
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        filt = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if filt == 1:  # Sub
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 255
        elif filt == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif filt == 3:  # Average
            for i in range(stride):
                left = line[i - 4] if i >= 4 else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif filt == 4:  # Paeth
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, out


def box_scale_rgba(w, h, rgba, tw, th):
    """Alpha-weighted box downscale so transparent padding doesn't darken
    edge pixels."""
    out = bytearray(tw * th * 4)
    for ty in range(th):
        y0 = ty * h // th
        y1 = max(y0 + 1, (ty + 1) * h // th)
        for tx in range(tw):
            x0 = tx * w // tw
            x1 = max(x0 + 1, (tx + 1) * w // tw)
            r = g = b = a = n = 0
            for y in range(y0, y1):
                base = y * w * 4
                for x in range(x0, x1):
                    i = base + x * 4
                    alpha = rgba[i + 3]
                    r += rgba[i] * alpha
                    g += rgba[i + 1] * alpha
                    b += rgba[i + 2] * alpha
                    a += alpha
                    n += 1
            o = (ty * tw + tx) * 4
            if a:
                out[o] = r // a
                out[o + 1] = g // a
                out[o + 2] = b // a
            out[o + 3] = a // n
    return out


def main():
    w, h, rgba = png_decode_rgba8(LOGO_PNG.read_bytes())
    tw = round(w * TARGET_H / h)
    small = box_scale_rgba(w, h, rgba, tw, TARGET_H)
    blob = base64.b64encode(zlib.compress(bytes(small), 9)).decode()
    lines = [blob[i:i + 76] for i in range(0, len(blob), 76)]
    literal = "\n".join('    "%s"' % line for line in lines)

    src = OG_CARD.read_text(encoding="utf-8")
    src = re.sub(r"_LOGO_W = \d+", "_LOGO_W = %d" % tw, src, count=1)
    src = re.sub(r"_LOGO_H = \d+", "_LOGO_H = %d" % TARGET_H, src, count=1)
    src, n = re.subn(
        r"_LOGO_B64 = \(\n(?:    \"[A-Za-z0-9+/=]*\"\n)+\)",
        "_LOGO_B64 = (\n%s\n)" % literal, src, count=1)
    if n != 1:
        sys.exit("could not find the _LOGO_B64 literal in %s" % OG_CARD)
    OG_CARD.write_text(src, encoding="utf-8")
    print("embedded %dx%d logo (%d base64 chars) into %s"
          % (tw, TARGET_H, len(blob), OG_CARD.name))


if __name__ == "__main__":
    main()

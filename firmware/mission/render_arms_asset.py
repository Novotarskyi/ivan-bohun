#!/usr/bin/env python3
"""render_arms_asset.py - bake the Cossack with musket for the kobzar's header.

Source: assets_src/herb_viyska_zaporozkoho.svg (Herb Viyska Zaporozkoho,
Wikimedia Commons, public-domain heraldry; native 653x810 -> aspect 0.807).
Renders at 2x on the UI background colour, box-downsamples for clean edges,
emits main/arms_84x104.bin (little-endian RGB565) for EMBED_FILES.
"""
import struct
import subprocess
import zlib
from pathlib import Path

HERE = Path(__file__).parent
SVG = HERE / "assets_src" / "herb_viyska_zaporozkoho.svg"
OUT = HERE / "main" / "arms_84x104.bin"
W, H = 84, 104
BG = "#0a0a0c"

html = HERE / "_arms.html"
html.write_text(
    f'<!DOCTYPE html><html><body style="margin:0;background:{BG};'
    f'display:flex;align-items:center;justify-content:center;width:{W}px;height:{H}px">'
    f'<img src="{SVG.name}" style="max-width:100%;max-height:100%"></body></html>')
# the img src is relative: render from assets_src so the svg resolves
render_page = HERE / "assets_src" / "_arms.html"
html.rename(render_page)
PNG = HERE / "_arms.png"
subprocess.run([
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "--headless=new", f"--screenshot={PNG}", f"--window-size={W},{H}",
    "--force-device-scale-factor=2", "--hide-scrollbars", "--disable-gpu",
    f"file://{render_page}",
], check=True, capture_output=True)
render_page.unlink()

raw = PNG.read_bytes()
assert raw[:8] == b"\x89PNG\r\n\x1a\n"
pos, idat, ihdr = 8, [], None
while pos < len(raw):
    (length,) = struct.unpack(">I", raw[pos:pos+4])
    ctype = raw[pos+4:pos+8]
    if ctype == b"IHDR":
        ihdr = struct.unpack(">IIBBBBB", raw[pos+8:pos+8+length])
    elif ctype == b"IDAT":
        idat.append(raw[pos+8:pos+8+length])
    pos += 12 + length
w, h, depth, ct, _, _, interlace = ihdr
assert (w, h) == (W * 2, H * 2) and depth == 8 and ct in (2, 6) and interlace == 0, ihdr
bpp = 3 if ct == 2 else 4
stride = w * bpp
plain = zlib.decompress(b"".join(idat))

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)

rows = []
prev = bytearray(stride)
for y in range(h):
    f = plain[y * (stride + 1)]
    line = bytearray(plain[y * (stride + 1) + 1: (y + 1) * (stride + 1)])
    if f == 1:
        for i in range(bpp, stride):
            line[i] = (line[i] + line[i - bpp]) & 0xff
    elif f == 2:
        for i in range(stride):
            line[i] = (line[i] + prev[i]) & 0xff
    elif f == 3:
        for i in range(stride):
            left = line[i - bpp] if i >= bpp else 0
            line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xff
    elif f == 4:
        for i in range(stride):
            left = line[i - bpp] if i >= bpp else 0
            ul = prev[i - bpp] if i >= bpp else 0
            line[i] = (line[i] + paeth(left, prev[i], ul)) & 0xff
    prev = line
    rows.append(line)

# 2x2 box downsample -> RGB565 LE
out = bytearray(W * H * 2)
oi = 0
for y in range(H):
    r0, r1 = rows[y * 2], rows[y * 2 + 1]
    for x in range(W):
        i0 = x * 2 * bpp
        r = (r0[i0] + r0[i0 + bpp] + r1[i0] + r1[i0 + bpp]) >> 2
        g = (r0[i0+1] + r0[i0+1+bpp] + r1[i0+1] + r1[i0+1+bpp]) >> 2
        b = (r0[i0+2] + r0[i0+2+bpp] + r1[i0+2] + r1[i0+2+bpp]) >> 2
        v = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3)
        out[oi] = v & 0xff
        out[oi + 1] = v >> 8
        oi += 2

OUT.write_bytes(out)
PNG.unlink()
print(f"{OUT.name}: {len(out)} bytes ({W}x{H} RGB565 LE, 2x supersampled)")

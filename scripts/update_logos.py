#!/usr/bin/env python3
"""
Regenerate every asset made from the logo SVGs. Run it after editing assets/oshot.svg,
assets/oshot-tray.svg or assets/oshot-tray-24.svg, from any directory:

    python3 scripts/update_logos.py            # both logos
    python3 scripts/update_logos.py tray       # only the tray SVGs
    python3 scripts/update_logos.py logo       # only oshot.svg

Needs rsvg-convert (librsvg) and Pillow. Files whose pixels didn't change are left alone.

assets/oshot.svg      -> assets/oshot.png (64), assets/linux/oshot-256.png, assets/windows/oshot.ico,
                         include/oshot_png.h (color tray icon), OSHOT_LOGO in include/tool_icons.h
assets/oshot-tray.svg -> assets/windows/oshot-tray.ico (16, 20, 32), include/tray_icon_png.h
                         (white tray icon)
assets/oshot-tray-24.svg -> the 24x24 frame of oshot-tray.ico, ICON_TEXT_TOOLS in include/tool_icons.h

oshot-tray-24.svg is the tray logo redrawn on a 24px grid with 2px strokes, since the 16px design
can't be scaled by 1.5 with evenly spaced lines. Keep the two in sync by hand.
Created and tested with Claude Code
"""

import importlib.util
import io
import math
import re
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from PIL import Image

sys.dont_write_bytecode = True  # no scripts/__pycache__ from loading icon_generator.py

ROOT = Path(__file__).resolve().parent.parent
SVG_NS = "{http://www.w3.org/2000/svg}"

LOGO_SVG = ROOT / "assets/oshot.svg"
TRAY_SVG = ROOT / "assets/oshot-tray.svg"
TRAY24_SVG = ROOT / "assets/oshot-tray-24.svg"


def render(svg, size):
    """Render an SVG to a size x size RGBA image.

    Pixel-grid SVGs (shape-rendering="crispEdges", only <rect>s) at a scale that isn't a whole
    number are snapped by hand, because rsvg-convert would make some 1px lines 1px thick and
    others 2px. Every grid cell becomes the same number of pixels instead, and a rect covers the
    pixels from its first cell to its last, so lines that meet (corner arms) still line up."""
    root = ET.parse(svg).getroot()
    scale = size / float(root.get("width"))

    if root.get("shape-rendering") != "crispEdges" or scale == int(scale):
        png = subprocess.run(
            ["rsvg-convert", "-w", str(size), "-h", str(size), str(svg)], check=True, capture_output=True
        ).stdout
        return Image.open(io.BytesIO(png)).convert("RGBA")

    thickness = max(1, math.floor(scale + 0.5))
    cell = lambda u: math.floor(u * scale + 0.5)  # first pixel of grid cell u
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    for group in root.iter(SVG_NS + "g"):
        fill = group.get("fill").lstrip("#")
        rgba = tuple(int(fill[i : i + 2], 16) for i in (0, 2, 4)) + (255,)
        for rect in group.iter(SVG_NS + "rect"):
            values = [float(rect.get(k)) for k in ("x", "y", "width", "height")]
            if any(v != int(v) for v in values):
                sys.exit(f"error: {svg.name}: crispEdges rect off the pixel grid: {ET.tostring(rect).decode()}")
            x, y, w, h = (int(v) for v in values)
            x0, x1 = cell(x), cell(x + w - 1) + thickness
            y0, y1 = cell(y), cell(y + h - 1) + thickness
            for py in range(y0, y1):
                for px in range(x0, x1):
                    img.putpixel((px, py), rgba)
    return img


def pixel_list(img):
    # getdata() is deprecated since Pillow 13
    return list(img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata())


def same_pixels(data, img):
    """Whether the image file in data (PNG, ...) decodes to exactly img."""
    try:
        old = Image.open(io.BytesIO(data)).convert("RGBA")
    except Exception:
        return False
    return old.size == img.size and old.tobytes() == img.tobytes()


def png_bytes(img):
    """Lossless PNG, as a palette image when it has at most 256 colors (like the committed ones)."""
    pixels = pixel_list(img)
    colors = sorted(set(pixels))
    out = io.BytesIO()
    if len(colors) > 256:
        img.save(out, "PNG", optimize=True)
        return out.getvalue()

    index = {c: i for i, c in enumerate(colors)}
    pal = Image.new("P", img.size)
    pal.putpalette([v for c in colors for v in c], "RGBA")
    pal.putdata([index[c] for c in pixels])
    bits = next(b for b in (1, 2, 4, 8) if len(colors) <= 1 << b)
    pal.save(out, "PNG", optimize=True, bits=bits)
    data = out.getvalue()
    assert same_pixels(data, img)
    return data


def ico_bytes(frames):
    """An .ico with 32-bit BMP frames (BGRA plus the AND mask) and a PNG one for 256x256,
    like ImageMagick used to write. Newer ImageMagick makes the 256 frame BMP too (270 KiB)."""
    entries, blobs = b"", b""
    offset = 6 + 16 * len(frames)
    for img in frames:
        w, h = img.size
        if w >= 256:
            # Plain RGBA, since Windows' icon loader may not take palette PNGs
            out = io.BytesIO()
            img.save(out, "PNG", optimize=True)
            blob = out.getvalue()
        else:
            rows = [img.crop((0, y, w, y + 1)).tobytes() for y in range(h)][::-1]  # bottom-up
            bgra = b"".join(bytes(px[i] for i in (2, 1, 0, 3)) for row in rows for px in zip(*[iter(row)] * 4))
            stride = (w + 31) // 32 * 4
            mask = b""
            for row in rows:
                bits = 0
                for x in range(w):
                    bits = (bits << 1) | (row[x * 4 + 3] == 0)  # 1 = transparent
                bits <<= stride * 8 - w
                mask += bits.to_bytes(stride, "big")
            # biSizeImage counts only the color data, as ImageMagick writes it
            header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(bgra), 0, 0, 0, 0)
            blob = header + bgra + mask
        entries += struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(blob), offset)
        blobs += blob
        offset += len(blob)
    return struct.pack("<HHH", 0, 1, len(frames)) + entries + blobs


def ico_frames_match(path, frames):
    if not path.exists():
        return False
    try:
        for img in frames:
            old = Image.open(path)
            old.size = img.size
            old.load()
            if old.convert("RGBA").tobytes() != img.tobytes():
                return False
        return len(Image.open(path).info["sizes"]) == len(frames)
    except Exception:
        return False


def report(path, changed, what=""):
    print(f"  {'updated  ' if changed else 'unchanged'} {path.relative_to(ROOT)}{what}")


def write_png(path, img):
    changed = not (path.exists() and same_pixels(path.read_bytes(), img))
    if changed:
        path.write_bytes(png_bytes(img))
    report(path, changed)


def write_ico(path, svg, sizes, overrides={}):
    """overrides maps a size to another SVG drawn for it."""
    frames = [render(overrides.get(size, svg), size) for size in sizes]
    changed = not ico_frames_match(path, frames)
    if changed:
        path.write_bytes(ico_bytes(frames))
    report(path, changed)


def write_embedded_png(path, name, img, data=None):
    """Replace the byte array and length in a header like include/oshot_png.h, keeping the rest.
    data is the PNG to embed (default png_bytes(img)), skipped when the current one decodes to img."""
    text = path.read_text()
    pattern = rf"inline constexpr unsigned char {name}\[\] = \{{(.*?)\}};"
    match = re.search(pattern, text, flags=re.S)
    if not match or not re.search(rf"{name}_len = \d+;", text):
        sys.exit(f"error: {name} not found in {path}")
    old = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))
    if data is None and same_pixels(old, img):
        return report(path, False)
    data = data if data is not None else png_bytes(img)
    if data == old:
        return report(path, False)

    lines = []
    for i in range(0, len(data), 18):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i : i + 18])
        lines.append(f"    {chunk}" + ("," if i + 18 < len(data) else ""))
    array = f"inline constexpr unsigned char {name}[] = {{\n" + "\n".join(lines) + "\n};"
    text = re.sub(pattern, lambda _: array, text, flags=re.S)
    text = re.sub(rf"{name}_len = \d+;", f"{name}_len = {len(data)};", text)
    path.write_text(text)
    report(path, True)


def write_tool_icon(name, svg, use_rgba, size):
    """Regenerate one icon of include/tool_icons.h with icon_generator.py's loader."""
    spec = importlib.util.spec_from_file_location("icon_generator", ROOT / "scripts/icon_generator.py")
    gen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gen)

    pixels = gen.create_icon_from_image(str(svg), use_rgba, size)
    if not pixels:
        sys.exit(f"error: could not render {svg}")
    block = f"inline constexpr int {name}_W = {size};\ninline constexpr int {name}_H = {size};\n"
    block += gen.format_c_array(name, pixels)

    path = ROOT / "include/tool_icons.h"
    old = path.read_text()
    text, n = re.subn(rf"inline constexpr int {name}_W = \d+;.*?\}};", lambda _: block, old, flags=re.S)
    if n != 1:
        sys.exit(f"error: {name} not found in {path}")
    if text != old:
        path.write_text(text)
    report(path, text != old, f" ({name})")


def update_logo():
    print(f"{LOGO_SVG.relative_to(ROOT)}:")
    png = ROOT / "assets/oshot.png"
    write_png(png, render(LOGO_SVG, 64))
    write_png(ROOT / "assets/linux/oshot-256.png", render(LOGO_SVG, 256))
    write_ico(ROOT / "assets/windows/oshot.ico", LOGO_SVG, [256, 128, 64, 48, 32, 24, 20, 16])
    # The same bytes as assets/oshot.png, written by tray code on Linux/macOS
    write_embedded_png(ROOT / "include/oshot_png.h", "oshot_png", None, png.read_bytes())
    write_tool_icon("OSHOT_LOGO", LOGO_SVG, True, 32)


def update_tray():
    print(f"{TRAY_SVG.relative_to(ROOT)}, {TRAY24_SVG.relative_to(ROOT)}:")
    write_ico(ROOT / "assets/windows/oshot-tray.ico", TRAY_SVG, [32, 24, 20, 16], {24: TRAY24_SVG})
    write_embedded_png(ROOT / "include/tray_icon_png.h", "tray_icon_png", render(TRAY_SVG, 32))
    write_tool_icon("ICON_TEXT_TOOLS", TRAY24_SVG, False, 24)


def main():
    targets = sys.argv[1:] or ["logo", "tray"]
    if any(t not in ("logo", "tray") for t in targets):
        sys.exit(f"usage: {sys.argv[0]} [logo] [tray]")
    for target in targets:
        update_logo() if target == "logo" else update_tray()


if __name__ == "__main__":
    main()

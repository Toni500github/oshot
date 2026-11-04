#!/usr/bin/env python3
"""
Generate the white toolbar icons of include/tool_icons.h (24x24, transparent background).
Run it from any directory:

    python3 scripts/icon_generator.py                  # write include/tool_icons.h
    python3 scripts/icon_generator.py --preview x.png  # only draw both variants, 10x and 1x

The tool icons are drawn here as signed distance shapes, sampled SAMPLES x SAMPLES times per
pixel, in two variants with their own designs. AA_ICON_* keep the covered part of each pixel as
alpha (anti-aliased), PP_ICON_* set a pixel only when at least half of it is covered, so they
stay pixel-sharp like the text tools icon. The antialiased-icons option picks one of them through
TOOL_ICON(), both have the same ICON_*_W/_H. Only the logos come from SVGs (ICON_TEXT_TOOLS from
assets/oshot-tray-24.svg, OSHOT_LOGO from assets/oshot.svg), which needs rsvg-convert and Pillow
(so does --preview). scripts/update_logos.py uses create_icon_from_image() and format_c_array()
for the same two.

Pencil, Text, Copy, Save and TextTools icons are original, drawing rewritten with Claude Code
"""

import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SIZE = 24
STROKE = 2.0  # line width of the icons, like the 2px strokes of oshot-tray-24.svg
SAMPLES = 8  # samples per pixel and axis, so 64 levels of coverage

# Shapes are functions (x, y) -> signed distance in pixels, negative inside. The canvas spans
# 0..SIZE on both axes and pixel (x, y) covers [x, x+1] x [y, y+1], so edges on whole numbers
# are exact and only slanted or curved edges cover pixels partially.


def circle(cx, cy, r):
    return lambda x, y: math.hypot(x - cx, y - cy) - r


def ring(cx, cy, r, width=STROKE):
    """Circle outline, r is the middle of the stroke"""
    return lambda x, y: abs(math.hypot(x - cx, y - cy) - r) - width / 2


def box(x0, y0, x1, y1, radius=0.0):
    """Filled rectangle with optionally rounded corners"""
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hw, hh = (x1 - x0) / 2 - radius, (y1 - y0) / 2 - radius

    def sdf(x, y):
        dx, dy = abs(x - cx) - hw, abs(y - cy) - hh
        return math.hypot(max(dx, 0), max(dy, 0)) + min(max(dx, dy), 0) - radius

    return sdf


def box_outline(x0, y0, x1, y1, radius=0.0, width=STROKE):
    """Rectangle outline whose stroke lies inside x0..x1, y0..y1"""
    return subtract(box(x0, y0, x1, y1, radius), box(x0 + width, y0 + width, x1 - width, y1 - width,
                                                    max(radius - width, 0)))


def segment(ax, ay, bx, by, width=STROKE):
    """Line with round caps"""
    dx, dy = bx - ax, by - ay
    len_sq = dx * dx + dy * dy

    def sdf(x, y):
        t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / len_sq))
        return math.hypot(x - ax - t * dx, y - ay - t * dy) - width / 2

    return sdf


def polygon(points):
    """Filled polygon (exact distance, so it can be grown or outlined too)"""
    n = len(points)

    def sdf(x, y):
        best = math.inf
        inside = False
        for i in range(n):
            (ax, ay), (bx, by) = points[i], points[i - 1]
            ex, ey = bx - ax, by - ay
            t = max(0.0, min(1.0, ((x - ax) * ex + (y - ay) * ey) / (ex * ex + ey * ey)))
            best = min(best, math.hypot(x - ax - t * ex, y - ay - t * ey))
            if (ay > y) != (by > y) and x < ax + (y - ay) * ex / ey:
                inside = not inside
        return -best if inside else best

    return sdf


def rounded_polygon(points, radius):
    """Convex polygon with rounded corners. Its edges are moved in by radius, then the shape is
    grown back by radius, which rounds every corner"""
    n = len(points)
    # Points go clockwise on screen (y down), so the inside is to the right of each edge
    lines = []
    for i in range(n):
        (ax, ay), (bx, by) = points[i], points[(i + 1) % n]
        length = math.hypot(bx - ax, by - ay)
        nx, ny = -(by - ay) / length, (bx - ax) / length
        lines.append(((ax + nx * radius, ay + ny * radius), (bx - ax, by - ay)))

    inner = []
    for i in range(n):
        (px, py), (dx, dy) = lines[i - 1]
        (qx, qy), (ex, ey) = lines[i]
        t = ((qx - px) * ey - (qy - py) * ex) / (dx * ey - dy * ex)
        inner.append((px + dx * t, py + dy * t))
    return grow(polygon(inner), radius)


def outline(shape, width=STROKE):
    """Stroke along the inside of a shape's edge"""
    return lambda x, y: (lambda d: max(d, -d - width))(shape(x, y))


def union(*shapes):
    return lambda x, y: min(s(x, y) for s in shapes)


def subtract(shape, cut):
    return lambda x, y: max(shape(x, y), -cut(x, y))


def grow(shape, amount):
    return lambda x, y: shape(x, y) - amount


def pixel_shape(is_set):
    """Shape made of whole pixels, for icons drawn pixel by pixel. is_set(x, y) gets pixel indices"""
    return lambda x, y: -1 if is_set(math.floor(x), math.floor(y)) else 1


def rasterize(shape, antialias, size=SIZE, samples=SAMPLES):
    """Return the RGBA bytes of a white shape. A pixel is opaque when at least half of it is
    covered, or with antialias its alpha is the covered part"""
    offsets = [(i + 0.5) / samples for i in range(samples)]
    total = samples * samples
    pixels = []
    for y in range(size):
        for x in range(size):
            # Pixels far from any edge are fully in or out, which saves most of the samples
            d = shape(x + 0.5, y + 0.5)
            if d >= 0.75:
                hits = 0
            elif d <= -0.75:
                hits = total
            else:
                hits = sum(1 for sy in offsets for sx in offsets if shape(x + sx, y + sy) <= 0)
            if antialias:
                alpha = round(255 * hits / total)
            else:
                alpha = 255 if hits * 2 >= total else 0
            pixels.extend([255, 255, 255, alpha] if alpha else [0, 0, 0, 0])
    return pixels


def along(origin, angle, t, s):
    """Point at distance t along a direction from origin and s across it"""
    ux, uy = math.cos(angle), math.sin(angle)
    return (origin[0] + ux * t - uy * s, origin[1] + uy * t + ux * s)


# From the bottom left to the top right
DIAGONAL = -math.pi / 4

# Anti-aliased icons (AA_ICON_*)


def aa_circle():
    return ring(12, 12, 7)


def aa_circle_filled():
    return circle(12, 12, 7.5)


def aa_square():
    return box_outline(5, 5, 19, 19, radius=1.5)


def aa_rect_filled():
    return box(5, 5, 19, 19, radius=1.5)


def aa_line():
    return segment(6, 18, 18, 6)


def aa_arrow():
    # Points to the bottom left, where the arrow tool's head ends up when dragging that way
    tip = (5, 19)
    head = polygon([tip, along(tip, DIAGONAL, 7.5, 3.75), along(tip, DIAGONAL, 7.5, -3.75)])
    return union(segment(18, 6, 9, 15), head)


def aa_pencil():
    # pp_pencil's pixels as shapes, on the band x + y = 21.5..26.5 with a blunt tip, then a gap
    # and a square eraser
    body = polygon([(13.5, 8), (16, 10.5), (6.5, 20), (4, 20), (4, 17.5)])
    eraser = polygon([(17.5, 4), (20, 6.5), (17.5, 9), (15, 6.5)])
    return union(body, eraser)


def aa_text():
    return union(box(5, 5, 19, 7), box(11, 5, 13, 19))


def aa_counter_bubble():
    # "1" in a circle, the bubble the CounterBubble tool draws
    # Its stem on the ring's middle, with a flag and a long base centered under it
    one = union(box(11.25, 7.5, 12.75, 16.5), polygon([(11.25, 7.5), (12.75, 7.5), (11.25, 9.5), (9.5, 10.75),
                                                          (9.5, 9.25)]), box(9.5, 15, 14.5, 16.5))
    return union(ring(12, 12, 8.25, 1.5), one)


def aa_copy():
    front = box(8, 7, 20, 21, radius=2)
    back = box_outline(4, 3, 16, 17, radius=2)
    return union(outline(front), subtract(back, grow(front, 1.5)))


def aa_save():
    return pp_save()


# Pixel-perfect icons (PP_ICON_*)


def pp_circle():
    # Centered on the middle of pixel (12, 12), so it is an odd 17px wide and the counter
    # bubble's "1" (also odd) can sit exactly in its middle
    return ring(12.5, 12.5, 7.75, 1.5)


def pp_circle_filled():
    # As big as pp_circle. A radius ending inside a pixel would leave single pixels sticking
    # out at the top, bottom and sides
    return circle(12.5, 12.5, 8.5)


def pp_square():
    return box_outline(5, 5, 19, 19)


def pp_rect_filled():
    return box(5, 5, 19, 19)


def pp_line():
    return segment(6, 18, 18, 6)


def pp_arrow():
    # Points to the bottom left, where the arrow tool's head ends up when dragging that way
    # Its head is a right angle with straight sides, which stay sharp on the pixel grid
    head = polygon([(5, 19), (5, 12), (12, 19)])
    return union(segment(18, 6, 8, 16), head)


def pp_pencil():
    # Pixel by pixel, on the diagonal band x + y = 21..25. Tip and body end where x - y = 5,
    # then a gap and a square eraser
    def is_set(x, y):
        if not 21 <= x + y <= 25:
            return False
        return (x >= 4 and y <= 19 and x - y <= 5) or 9 <= x - y <= 13

    return pixel_shape(is_set)


def pp_text():
    return union(box(5, 5, 19, 7), box(11, 5, 13, 19))


def pp_counter_bubble():
    # "1" in a circle, the bubble the CounterBubble tool draws
    # 1px wide on the circle's middle column, with a flag and a base line centered under it
    one = union(box(12, 8, 13, 17), box(11, 9, 12, 10), box(11, 16, 14, 17))
    return union(pp_circle(), one)


def pp_copy():
    front = box(8, 7, 20, 21, radius=2)
    back = box_outline(4, 3, 16, 17, radius=2)
    return union(outline(front), subtract(back, grow(front, 1.5)))


def pp_save():
    # Floppy disk with its top right corner cut, a shutter and a hub
    body = rounded_polygon([(4, 4), (16, 4), (20, 8), (20, 21), (4, 21)], 2.5)
    return union(outline(body), box(7, 7, 14, 10), circle(12, 15, 2))


def create_icon_from_image(image_path, use_rgba=False, size=24, threshold=128):
    try:
        from PIL import Image
    except ImportError:
        print("PIL/Pillow not installed.")
        return None

    try:
        if image_path.endswith(".svg"):
            # Render SVGs with rsvg-convert. Small ones (pixel-grid icons like oshot-tray.svg) at the
            # largest whole multiple of their size that fits, so their pixels stay sharp, bigger ones
            # straight at the target size (sharper than downscaling a PNG)
            import io
            import subprocess
            import xml.etree.ElementTree as ET

            root = ET.parse(image_path).getroot()
            svg_w = int(float(root.get("width", str(size))))
            svg_h = int(float(root.get("height", str(size))))
            scale = min(size // svg_w, size // svg_h)
            if scale >= 1:
                cmd = ["rsvg-convert", "-w", str(svg_w * scale), "-h", str(svg_h * scale), image_path]
            else:
                cmd = ["rsvg-convert", "-w", str(size), "-h", str(size), image_path]
            img = Image.open(io.BytesIO(subprocess.run(cmd, check=True, capture_output=True).stdout))
        else:
            img = Image.open(image_path)
        img = img.convert("RGBA")

        if img.width <= size and img.height <= size:
            # Center smaller images instead of scaling them up, so their pixels stay sharp
            canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
            canvas.paste(img, ((size - img.width) // 2, (size - img.height) // 2))
            img = canvas
        else:
            img = img.resize((size, size), Image.Resampling.LANCZOS)

        # Decide per image, not per pixel, whether the shape is in the alpha channel,
        # or the opaque pixels of a transparent icon would go through the brightness path
        has_transparency = img.getchannel("A").getextrema()[0] < 255

        pixels = []
        for y in range(size):
            for x in range(size):
                r, g, b, a = img.getpixel((x, y))

                brightness = (r + g + b) // 3

                if use_rgba:
                    pixels.extend([r, g, b, a])
                else:
                    # Use alpha as the shape mask (handles dark-on-transparent icons),
                    # fall back to brightness for fully opaque images (dark-on-white, etc.)
                    if has_transparency:
                        # Shape is encoded in alpha, so snap opaque pixels to white
                        snapped_a = 255 if a >= threshold else 0
                        pixels.extend([snapped_a, snapped_a, snapped_a, snapped_a])
                    else:
                        # Fully opaque image. Dark pixels are the icon, light pixels are background
                        # Invert brightness so dark → white icon
                        white = 255 if brightness < (255 - threshold) else 0
                        snapped_a = white
                        pixels.extend([white, white, white, snapped_a])

        return pixels

    except Exception as e:
        print(f"Error loading image: {e}")
        return None


def format_c_array(name, pixels):
    size = len(pixels)
    lines = []
    lines.append(f"inline constexpr unsigned char {name}_RGBA[{size}] = {{")

    bytes_per_line = 16
    for i in range(0, size, bytes_per_line):
        line_chunks = []
        for j in range(i, min(i + bytes_per_line, size)):
            line_chunks.append(f"{pixels[j]:3d}")

        line = "    " + ", ".join(line_chunks)
        if i + bytes_per_line < size:
            line += ","
        lines.append(line)

    lines.append("};")
    return "\n".join(lines)


# In the order of include/tool_icons.h. Drawn icons are (name, anti-aliased shape, pixel-perfect
# shape), logos are (name, svg, use_rgba, size) and have one variant
ICONS = [
    ("ICON_CIRCLE", aa_circle, pp_circle),
    ("ICON_CIRCLE_FILLED", aa_circle_filled, pp_circle_filled),
    ("ICON_SQUARE", aa_square, pp_square),
    ("ICON_RECT_FILLED", aa_rect_filled, pp_rect_filled),
    ("ICON_LINE", aa_line, pp_line),
    ("ICON_PENCIL", aa_pencil, pp_pencil),
    ("ICON_ARROW", aa_arrow, pp_arrow),
    ("ICON_TEXT", aa_text, pp_text),
    ("ICON_TEXT_TOOLS", ROOT / "assets/oshot-tray-24.svg", False, 24),
    ("OSHOT_LOGO", ROOT / "assets/oshot.svg", True, 32),
    ("ICON_COUNTER_BUBBLE", aa_counter_bubble, pp_counter_bubble),
    ("ICON_COPY", aa_copy, pp_copy),
    ("ICON_SAVE", aa_save, pp_save),
]
DRAWN = [(name, aa, pp) for name, aa, pp, *logo in ICONS if not logo]

HEADER = """#pragma once

// clang-format off

// TOOL_ICON(antialiased, ICON_PENCIL) expands to the anti-aliased or pixel-perfect variant of an
// icon, then its width and height
#define TOOL_ICON(antialiased, name) ((antialiased) ? AA_##name##_RGBA : PP_##name##_RGBA), name##_W, name##_H

"""


def write_header():
    header = HEADER
    for name, *source in ICONS:
        if len(source) == 2:
            size = SIZE
            arrays = format_c_array(f"AA_{name}", rasterize(source[0](), antialias=True)) + "\n"
            arrays += format_c_array(f"PP_{name}", rasterize(source[1](), antialias=False))
        else:
            svg, use_rgba, size = source
            pixels = create_icon_from_image(str(svg), use_rgba, size)
            if not pixels:
                raise SystemExit(f"error: could not render {svg}")
            arrays = format_c_array(name, pixels)

        header += f"/* {name.removeprefix('ICON_').lower()} */\n"
        header += f"inline constexpr int {name}_W = {size};\n"
        header += f"inline constexpr int {name}_H = {size};\n"
        header += arrays + "\n\n"

    path = ROOT / "include/tool_icons.h"
    path.write_text(header)
    print(f"Generated {path.relative_to(ROOT)}")


def write_preview(path, zoom=10, gap=20):
    """Draw every icon on the toolbar's dark grey, anti-aliased above pixel-perfect, zoomed in and
    then at their real size"""
    from PIL import Image

    cell = SIZE * zoom + gap
    sheet = Image.new("RGB", (cell * len(DRAWN), cell * 2 + gap + SIZE), (40, 42, 48))
    for col, (name, aa, pp) in enumerate(DRAWN):
        for row, shape in enumerate((aa, pp)):
            pixels = rasterize(shape(), antialias=row == 0)
            icon = Image.frombytes("RGBA", (SIZE, SIZE), bytes(pixels))
            big = icon.resize((SIZE * zoom, SIZE * zoom), Image.Resampling.NEAREST)
            sheet.paste(big, (col * cell + gap // 2, row * cell + gap // 2), big)
            sheet.paste(icon, (col * cell + gap // 2 + row * (SIZE + 4), cell * 2 + gap // 2), icon)
    sheet.save(path)
    print(f"Wrote {path}")


def main():
    args = sys.argv[1:]
    if args[:1] == ["--preview"] and len(args) == 2:
        write_preview(args[1])
    elif not args:
        write_header()
    else:
        raise SystemExit(f"usage: {sys.argv[0]} [--preview FILE.png]")


if __name__ == "__main__":
    main()

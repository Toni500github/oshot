#!/usr/bin/env python3
"""
Generate minimal white outline icons for flameshot-like toolbar.
24x24 pixels, white outlines only, transparent background.
Generated with Deepseek
"""

def create_circle_pixels(thickness=1.5):
    """White circle outline"""
    size = 24
    pixels = []
    center = 11.5
    radius = 7.0
    half_thickness = thickness / 2.0
    
    for y in range(size):
        for x in range(size):
            dx = x - center
            dy = y - center
            dist = (dx*dx + dy*dy) ** 0.5
            
            # Circle outline with thickness
            dist_from_edge = abs(dist - radius)
            if dist_from_edge <= half_thickness:
                if dist_from_edge <= half_thickness - 0.5:
                    alpha = 255
                else:
                    alpha = int(255 * (half_thickness + 0.5 - dist_from_edge))
                pixels.extend([255, 255, 255, alpha])
            else:
                pixels.extend([0, 0, 0, 0])
    
    return pixels

def create_square_pixels(thickness=1.5):
    """White square outline"""
    size = 24
    pixels = []
    margin = 5
    half_thickness = thickness / 2.0
    
    for y in range(size):
        for x in range(size):
            # Check distance to each edge
            dist_to_left = abs(x - margin)
            dist_to_right = abs(x - (size - margin - 1))
            dist_to_top = abs(y - margin)
            dist_to_bottom = abs(y - (size - margin - 1))
            
            # Check if near any border
            near_left = dist_to_left <= half_thickness and y >= margin and y < size - margin
            near_right = dist_to_right <= half_thickness and y >= margin and y < size - margin
            near_top = dist_to_top <= half_thickness and x >= margin and x < size - margin
            near_bottom = dist_to_bottom <= half_thickness and x >= margin and x < size - margin
            
            if near_left or near_right or near_top or near_bottom:
                # Find minimum distance to any edge for anti-aliasing
                min_dist = min(
                    dist_to_left if near_left else 100,
                    dist_to_right if near_right else 100,
                    dist_to_top if near_top else 100,
                    dist_to_bottom if near_bottom else 100
                )
                
                if min_dist <= half_thickness - 0.5:
                    alpha = 255
                else:
                    alpha = int(255 * (half_thickness + 0.5 - min_dist))
                pixels.extend([255, 255, 255, alpha])
            else:
                pixels.extend([0, 0, 0, 0])
    
    return pixels

def create_filled_circle_pixels(radius=7.0):
    """White filled circle"""
    size = 24
    pixels = []
    center = 11.5

    for y in range(size):
        for x in range(size):
            dx = x - center
            dy = y - center
            dist = (dx * dx + dy * dy) ** 0.5

            if dist <= radius:
                # Simple edge smoothing
                if dist <= radius - 0.5:
                    alpha = 255
                else:
                    alpha = int(255 * (radius + 0.5 - dist))
                pixels.extend([255, 255, 255, alpha])
            else:
                pixels.extend([0, 0, 0, 0])

    return pixels

def create_line_pixels(thickness=1.5):
    """White diagonal line from top-right to bottom-left"""
    size = 24
    pixels = []
    half_thickness = thickness / 2.0
    
    for y in range(size):
        for x in range(size):
            # Line from (5,19) to (19,5) - top-right to bottom-left
            # Equation: y = -x + 24
            dist_to_line = abs(y + x - 24) / (2**0.5)
            
            if dist_to_line <= half_thickness and x >= 5 and x <= 19 and y >= 5 and y <= 19:
                if dist_to_line <= half_thickness - 0.5:
                    alpha = 255
                else:
                    alpha = int(255 * (half_thickness + 0.5 - dist_to_line))
                pixels.extend([255, 255, 255, alpha])
            else:
                pixels.extend([0, 0, 0, 0])
    
    return pixels

def create_filled_rectangle_pixels(margin=5):
    """White filled rectangle"""
    size = 24
    pixels = []

    for y in range(size):
        for x in range(size):
            if margin <= x < size - margin and margin <= y < size - margin:
                pixels.extend([255, 255, 255, 255])
            else:
                pixels.extend([0, 0, 0, 0])

    return pixels

def create_counter_bubble_pixels(circle_thickness=1.5):
    size = 24
    pixels = create_circle_pixels(circle_thickness)

    def dist_to_segment(px, py, ax, ay, bx, by):
        dx, dy = bx - ax, by - ay
        len_sq = dx * dx + dy * dy
        if len_sq == 0:
            return ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
        t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / len_sq))
        return ((px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2) ** 0.5

    def blend_pixel(idx, alpha):
        if alpha > pixels[idx + 3]:
            pixels[idx:idx + 4] = [255, 255, 255, alpha]

    def draw_segment(ax, ay, bx, by, seg_thickness=1.4):
        ht = seg_thickness / 2.0
        for y in range(size):
            for x in range(size):
                d = dist_to_segment(x, y, ax, ay, bx, by)
                if d <= ht:
                    alpha = 255 if d <= ht - 0.5 else int(255 * (ht + 0.5 - d))
                    blend_pixel((y * size + x) * 4, alpha)

    # "1" glyph, centered in the circle
    draw_segment(12,  7,   12, 15)   # Main vertical stroke
    draw_segment(10,  9,   12,  7)   # Top-left diagonal flag
    draw_segment(10, 16, 13.5, 16)   # Bottom serif

    return pixels

def create_icon_from_image(image_path, use_rgba=False, threshold=128):
    try:
        from PIL import Image
    except ImportError:
        print("PIL/Pillow not installed.")
        return None

    try:
        img = Image.open(image_path).convert("RGBA")
        img = img.resize((24, 24), Image.Resampling.LANCZOS)

        pixels = []
        for y in range(24):
            for x in range(24):
                r, g, b, a = img.getpixel((x, y))

                # Snap to pure black or white based on brightness
                brightness = (r + g + b) // 3
                white = 255 if brightness >= threshold else 0

                # Snap alpha to fully opaque or transparent
                a = 255 if a >= threshold else 0
                if use_rgba:
                    pixels.extend([r, g, b, a])
                else:
                    pixels.extend([white, white, white, a])

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

def main():
    icons = [
    #    ("ICON_CIRCLE", create_circle_pixels()),
    #    ("ICON_CIRCLE_FILLED", create_filled_circle_pixels()),
    #    ("ICON_SQUARE", create_square_pixels()),
    #    ("ICON_RECT_FILLED", create_filled_rectangle_pixels()),
    #    ("ICON_LINE", create_line_pixels()),
        ("ICON_COUNTER_BUBBLE", create_counter_bubble_pixels())
    ]
    
    header = """#pragma once

"""
    # Optional: Add external images
    external_images = [
    #    ("ICON_PENCIL", "/tmp/image.png"),
    #    ("ICON_ARROW", "/tmp/image2.png"),
    #    ("ICON_TEXT", "/tmp/text.png")
    #    ("OSHOT_LOGO", "./oshot.png", True)
    ]
    
    for name, image_path, use_rgba in external_images:
        pixels = create_icon_from_image(image_path, use_rgba)
        if pixels:
            icons.append((name, pixels))
    

    
    for name, pixels in icons:
        icon_name = name[5:].lower()
        header += f"/* {icon_name} */\n"
        header += f"inline constexpr int {name}_W = 24;\n"
        header += f"inline constexpr int {name}_H = 24;\n"
        header += format_c_array(name, pixels) + "\n\n"
    
    with open("tool_icons.h", "w") as f:
        f.write(header)
    
    print("Generated tool_icons.h")

if __name__ == "__main__":
    main()

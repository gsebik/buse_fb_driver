#!/usr/bin/env python3
"""Test patterns for 144x19 1bpp framebuffer debugging
   Updated: No mirroring (matches new driver)
"""

import sys

WIDTH = 144
HEIGHT = 19
FB_SIZE = WIDTH * HEIGHT // 8  # 342 bytes

def pixel_to_byte_bit(x, y):
    """Convert x,y to byte index and bit position (no mirroring)"""
    idx = y * WIDTH + x
    return idx >> 3, idx & 7

def create_fb():
    return bytearray(FB_SIZE)

def set_pixel(fb, x, y):
    if 0 <= x < WIDTH and 0 <= y < HEIGHT:
        byte_idx, bit = pixel_to_byte_bit(x, y)
        fb[byte_idx] |= (1 << bit)

def pattern_half_panel_only():
    """Light up only the half panel (columns 128-143)"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(128, 144):
            set_pixel(fb, x, y)
    return fb

def pattern_full_panels_only():
    """Light up only full panels (columns 0-127)"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(0, 128):
            set_pixel(fb, x, y)
    return fb

def pattern_all_on():
    """All pixels on"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            set_pixel(fb, x, y)
    return fb

def pattern_vertical_stripes():
    """Vertical stripes every 8 columns"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if (x // 8) % 2 == 0:
                set_pixel(fb, x, y)
    return fb

def pattern_horizontal_stripes():
    """Horizontal stripes every 2 rows"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if y % 2 == 0:
                set_pixel(fb, x, y)
    return fb

def pattern_panel_id():
    """Different pattern for each panel to identify order"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            panel = x // 32 if x < 128 else 4
            if panel == 0:
                set_pixel(fb, x, y)
            elif panel == 1 and y < 10:
                set_pixel(fb, x, y)
            elif panel == 2 and y >= 9:
                set_pixel(fb, x, y)
            elif panel == 3 and (x % 32) < 16:
                set_pixel(fb, x, y)
            elif panel == 4:
                set_pixel(fb, x, y)
    return fb

def pattern_single_column(col):
    """Light up a single column"""
    fb = create_fb()
    for y in range(HEIGHT):
        set_pixel(fb, col, y)
    return fb

def pattern_column_march():
    """Single column - specify column as arg"""
    col = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    return pattern_single_column(col)

def pattern_half_panel_cols():
    """Light up half panel columns one at a time based on arg"""
    col = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    return pattern_single_column(128 + col)

def pattern_group_test():
    """Show which group each column belongs to (group 0 only)"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            col_in_panel = x % 32 if x < 128 else (x - 128)
            grp = col_in_panel % 4
            if grp == 0:
                set_pixel(fb, x, y)
    return fb

def pattern_half_panel_group(grp_num):
    """Test specific group in half panel"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(128, 144):
            col_in_panel = x - 128
            grp = col_in_panel % 4
            if grp == grp_num:
                set_pixel(fb, x, y)
    return fb

def pattern_diagonal():
    """Diagonal lines across the display"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if (x + y) % 8 == 0:
                set_pixel(fb, x, y)
    return fb

def pattern_diagonal_thick():
    """Thicker diagonal lines"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if (x + y) % 8 < 2:
                set_pixel(fb, x, y)
    return fb

def pattern_diagonal_reverse():
    """Diagonal lines going the other way"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if (x - y) % 8 == 0:
                set_pixel(fb, x, y)
    return fb

def pattern_crosshatch():
    """Crosshatch pattern (both diagonals)"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if (x + y) % 8 == 0 or (x - y) % 8 == 0:
                set_pixel(fb, x, y)
    return fb

def pattern_single_diagonal():
    """Single diagonal line - specify offset as arg"""
    offset = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    fb = create_fb()
    for y in range(HEIGHT):
        x = (y + offset) % WIDTH
        set_pixel(fb, x, y)
    return fb

def pattern_checkerboard():
    """Checkerboard pattern"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if (x + y) % 2 == 0:
                set_pixel(fb, x, y)
    return fb

def pattern_grid():
    """Grid pattern (every 8 pixels)"""
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if x % 8 == 0 or y % 8 == 0:
                set_pixel(fb, x, y)
    return fb

patterns = {
    'half': pattern_half_panel_only,
    'full': pattern_full_panels_only,
    'all': pattern_all_on,
    'vstripe': pattern_vertical_stripes,
    'hstripe': pattern_horizontal_stripes,
    'panels': pattern_panel_id,
    'col': pattern_column_march,
    'halfcol': pattern_half_panel_cols,
    'group': pattern_group_test,
    'hg0': lambda: pattern_half_panel_group(0),
    'hg1': lambda: pattern_half_panel_group(1),
    'hg2': lambda: pattern_half_panel_group(2),
    'hg3': lambda: pattern_half_panel_group(3),
    'diag': pattern_diagonal,
    'diag2': pattern_diagonal_thick,
    'diagr': pattern_diagonal_reverse,
    'cross': pattern_crosshatch,
    'line': pattern_single_diagonal,
    'check': pattern_checkerboard,
    'grid': pattern_grid,
}

if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in patterns:
        print(f"Usage: {sys.argv[0]} <pattern> [arg]")
        print(f"Patterns: {', '.join(patterns.keys())}")
        print("\n  half    - half panel only (cols 128-143)")
        print("  full    - full panels only (cols 0-127)")
        print("  all     - all pixels on")
        print("  vstripe - vertical stripes")
        print("  hstripe - horizontal stripes")
        print("  panels  - identify each panel")
        print("  col N   - single column N")
        print("  halfcol N - half panel column N (0-15)")
        print("  group   - show group 0 columns")
        print("  hg0-hg3 - half panel group 0-3")
        print("  diag    - diagonal lines")
        print("  diag2   - thick diagonal lines")
        print("  diagr   - reverse diagonal")
        print("  cross   - crosshatch")
        print("  line N  - single diagonal at offset N")
        print("  check   - checkerboard")
        print("  grid    - grid pattern")
        sys.exit(1)

    fb = patterns[sys.argv[1]]()

    with open('/dev/fb0', 'wb') as f:
        f.write(fb)

    print(f"Wrote {len(fb)} bytes to /dev/fb0")

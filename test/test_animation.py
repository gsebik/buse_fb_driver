#!/usr/bin/env python3
"""Animated test - moving dot to check for panel/group issues"""

import time
import sys

WIDTH = 144
HEIGHT = 19
FB_SIZE = WIDTH * HEIGHT // 8

def create_fb():
    return bytearray(FB_SIZE)

def set_pixel(fb, x, y):
    """Set pixel - driver handles all hardware transformations"""
    if 0 <= x < WIDTH and 0 <= y < HEIGHT:
        idx = y * WIDTH + x
        byte_idx = idx >> 3
        bit = idx & 7
        fb[byte_idx] |= (1 << bit)

def write_fb(fb):
    with open('/dev/fb0', 'wb') as f:
        f.write(fb)

def test_horizontal_sweep():
    """Move a vertical line across the screen"""
    print("Horizontal sweep test - line should move smoothly left to right")
    for x in range(WIDTH):
        fb = create_fb()
        for y in range(HEIGHT):
            set_pixel(fb, x, y)
        write_fb(fb)
        time.sleep(0.03)

def test_ball_bounce():
    """Bouncing ball like pong"""
    print("Bouncing ball test - should move smoothly")
    x, y = 10.0, 9.0
    dx, dy = 2.0, 1.0

    for _ in range(300):
        fb = create_fb()

        # Draw ball (2x2)
        ix, iy = int(x), int(y)
        set_pixel(fb, ix, iy)
        set_pixel(fb, ix+1, iy)
        set_pixel(fb, ix, iy+1)
        set_pixel(fb, ix+1, iy+1)

        write_fb(fb)

        # Move ball
        x += dx
        y += dy

        # Bounce
        if x <= 0 or x >= WIDTH - 2:
            dx = -dx
        if y <= 0 or y >= HEIGHT - 2:
            dy = -dy

        time.sleep(0.116)

def test_group_check():
    """Light up one column at a time in each group to check ordering"""
    print("Group check - columns should light up in order 0,1,2,3,4,5,6,7...")
    for x in range(32):  # First panel only
        fb = create_fb()
        for y in range(HEIGHT):
            set_pixel(fb, x, y)
        write_fb(fb)
        print(f"Column {x} (group {x % 4})")
        time.sleep(0.2)

def test_four_columns():
    """Show 4 adjacent columns to check group ordering"""
    print("Showing columns 0,1,2,3 - should be adjacent left to right")
    fb = create_fb()
    for x in range(4):
        for y in range(HEIGHT):
            set_pixel(fb, x, y)
    write_fb(fb)
    input("Press Enter to continue...")

    print("Showing columns 4,5,6,7")
    fb = create_fb()
    for x in range(4, 8):
        for y in range(HEIGHT):
            set_pixel(fb, x, y)
    write_fb(fb)
    input("Press Enter to continue...")

def test_diagonal_fill():
    """Fill screen diagonally from 0,0 - helps visualize pixel order"""
    print("Diagonal fill from (0,0) - watch for any jumping or mirroring")
    fb = create_fb()

    # Fill by diagonal (x + y = constant)
    for diag in range(WIDTH + HEIGHT - 1):
        for y in range(HEIGHT):
            x = diag - y
            if 0 <= x < WIDTH:
                set_pixel(fb, x, y)
        write_fb(fb)
        time.sleep(0.02)

    time.sleep(1)

def test_pixel_march():
    """Light up one pixel at a time, row by row"""
    print("Pixel march - one pixel at a time from (0,0)")
    for y in range(HEIGHT):
        for x in range(WIDTH):
            fb = create_fb()
            set_pixel(fb, x, y)
            write_fb(fb)
            time.sleep(0.01)

def test_row_fill():
    """Fill row by row from top"""
    print("Row fill from top - watch for any issues")
    fb = create_fb()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            set_pixel(fb, x, y)
        write_fb(fb)
        time.sleep(0.1)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: test_animation.py <test>")
        print("  sweep  - horizontal line sweep")
        print("  ball   - bouncing ball")
        print("  group  - column by column")
        print("  four   - 4 columns at a time")
        print("  diagfill - diagonal fill from 0,0")
        print("  pixel  - single pixel march")
        print("  row    - row by row fill")
        sys.exit(1)

    test = sys.argv[1]
    if test == 'sweep':
        test_horizontal_sweep()
    elif test == 'ball':
        test_ball_bounce()
    elif test == 'group':
        test_group_check()
    elif test == 'four':
        test_four_columns()
    elif test == 'diagfill':
        test_diagonal_fill()
    elif test == 'pixel':
        test_pixel_march()
    elif test == 'row':
        test_row_fill()
    else:
        print(f"Unknown test: {test}")

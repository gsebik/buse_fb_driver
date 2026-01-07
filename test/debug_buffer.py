#!/usr/bin/env python3
"""Debug: show what buffer values SHOULD be for each group/panel"""

WIDTH = 144
HEIGHT = 19
PANELS = 5
PANEL_COLS = 32
FULL_PANEL_WIDTH = 128
GROUPS = 4
COLS_PER_GROUP = PANEL_COLS // GROUPS  # 8
REGS_PER_COL = 3
DATA_BYTES_PER_PANEL = COLS_PER_GROUP * REGS_PER_COL  # 24
PANEL_BYTES = DATA_BYTES_PER_PANEL + 1  # 25
GROUP_BYTES = PANELS * PANEL_BYTES  # 125
FRAME_BYTES = GROUPS * GROUP_BYTES  # 500

print(f"PANEL_BYTES={PANEL_BYTES}, GROUP_BYTES={GROUP_BYTES}, FRAME_BYTES={FRAME_BYTES}")
print()

# Simulate the driver's pixel mapping for a single lit pixel
def map_pixel(x, y):
    x_mirrored = WIDTH - 1 - x

    y_rev = HEIGHT - 1 - y
    reg = y_rev // 8
    bit = 7 - (y_rev % 8)

    if x < FULL_PANEL_WIDTH:
        phys_panel = x // PANEL_COLS
        buffer_panel = 3 - phys_panel  # reversed
        col_in_panel = x % PANEL_COLS
    else:
        buffer_panel = 4
        col_in_panel = x - FULL_PANEL_WIDTH

    grp = col_in_panel % GROUPS
    cp = (col_in_panel // GROUPS) ^ 0x01
    base = grp * GROUP_BYTES + buffer_panel * PANEL_BYTES
    data_offset = base + 1 + cp * REGS_PER_COL + reg

    return {
        'x': x,
        'y': y,
        'buffer_panel': buffer_panel,
        'col_in_panel': col_in_panel,
        'grp': grp,
        'cp': cp,
        'base': base,
        'group_byte_pos': base,
        'group_byte_val': grp,
        'data_pos': data_offset,
        'data_bit': bit,
    }

# Show mapping for half panel columns (128-143)
print("=== HALF PANEL MAPPING (x=128-143) ===")
print("x    | col_in | grp | cp | base | group_byte_pos | data_pos")
print("-" * 65)
for x in range(128, 144):
    m = map_pixel(x, 0)
    print(f"{x:3d}  | {m['col_in_panel']:6d} | {m['grp']:3d} | {m['cp']:2d} | {m['base']:4d} | {m['group_byte_pos']:14d} | {m['data_pos']:8d}")

print()
print("=== GROUP BYTE POSITIONS (first byte of each panel in each group) ===")
for grp in range(GROUPS):
    print(f"Group {grp}:", end=" ")
    for panel in range(PANELS):
        pos = grp * GROUP_BYTES + panel * PANEL_BYTES
        print(f"panel{panel}@{pos}", end=" ")
    print()

print()
print("=== FULL PANEL MAPPING (sample: x=0,32,64,96) ===")
for x in [0, 32, 64, 96]:
    m = map_pixel(x, 0)
    print(f"x={x:3d} -> buffer_panel={m['buffer_panel']}, grp={m['grp']}, base={m['base']}")

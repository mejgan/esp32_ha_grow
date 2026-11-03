#!/usr/bin/env python3
"""Parametric 3D-printable enclosure for the GrowBox-C6 controller.

    pip install manifold3d numpy
    python case/growbox_case.py                 # every part -> case/stl/
    python case/growbox_case.py --check         # also check that nothing inside collides

Parts:
    base.stl            box with perfboard standoffs, cable holes, USB-C, DC jack, vents, keyholes
    lid_<display>.stl   lid with the window and mounts for one display type (see DISPLAYS)

Inside sits a standard 7 x 9 cm perfboard carrying the ESP32-C6 DevKit (USB end toward the left wall), the
MOSFETs, the 5 V buck converter and the sensor connectors. All sizes are in mm; the parameters below are the
ones you would change. Display presets are typical module sizes: measure yours and adjust if they differ.

Print orientation is already right in the STLs: base open side up, lid outer face down. No supports needed
(wall holes are teardrops).
"""

import argparse
import math
import os
import sys
from dataclasses import dataclass, field

import numpy as np
from manifold3d import CrossSection, JoinType, Manifold, OpType

# ---------------------------------------------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------------------------------------------

WALL = 2.4            # wall thickness
FLOOR = 2.4           # base floor thickness
LID_T = 2.4           # lid plate thickness
CORNER_R = 4.0        # outer corner radius
INNER_W = 120.0       # inside width  (X)
INNER_D = 86.0        # inside depth  (Y, bottom wall with the cables at Y = 0)
INNER_H = 52.0        # inside height (Z, floor top to lid underside)
FIT = 0.3             # clearance for parts that slide into each other

# Lid screws: M3 x 10-12 countersunk, self-tapping into the corner pillars (or heat-set inserts).
PILLAR = 8.0          # corner pillar size
SCREW_PILOT_D = 2.6   # self-tapping M3; use 4.0 (depth 6) for M3 heat-set inserts
SCREW_PILOT_DEPTH = 12.0
SCREW_CLEAR_D = 3.4
SCREW_HEAD_D = 6.6    # countersink diameter at the lid surface

# Lid alignment lip.
LIP_T = 1.6
LIP_H = 3.0

# 7 x 9 cm perfboard (holes 2 mm from the edges), long side along X.
PB_W, PB_D, PB_T = 90.0, 70.0, 1.6
PB_X, PB_Y = 9.0, 12.0          # lower-left corner, inside coordinates
PB_HOLE_INSET = 2.0
PB_STANDOFF_H = 6.0
PB_STANDOFF_D = 5.5
PB_PILOT_D = 1.8                # M2 self-tapping
PB_PARTS_H = 17.0               # tallest part on the board (DevKit on female headers ~16 mm)

# ESP32-C6-DevKitC-1 on the perfboard: top-left, USB-C end at the left board edge.
DEVKIT_W, DEVKIT_D = 54.0, 25.5
DEVKIT_Y_FROM_TOP = 2.0
USB_SLOT_W, USB_SLOT_H = 28.0, 12.0   # both USB-C ports of the DevKit
USB_SLOT_Z = 17.0                     # slot center above the floor (female headers ~19, soldered ~13)

# Cable holes in the bottom wall: PG7 gland (12.5 mm) or GX12 aviation connector (12 mm).
# Column per floor: lower = LED strip, upper = sensors.
CABLE_HOLE_D = 12.5
CABLE_X = (40.0, 64.0, 88.0)
CABLE_Z = (11.0, 33.0)
CABLE_NUT_R = 11.0              # PG7 nut across corners / 2, for the collision check
CABLE_INSIDE = 10.0             # gland length inside the wall

# DC jack (right wall). 11 mm for the common DC-022 5.5x2.1 panel jack, 8 mm for DC-099 metal jacks.
DC_JACK_D = 11.0
DC_JACK_Y, DC_JACK_Z = 20.0, 14.0
DC_JACK_BODY = (16.0, 12.0)     # length inside, diameter (collision check)

# Panel push button in the lid, wired in parallel to BOOT (GPIO9 to GND).
BUTTON_D = 12.0
BUTTON_X, BUTTON_Y = 16.0, 11.0
BUTTON_BODY = (22.0, 14.0)      # depth below the lid, diameter (collision check)

# Wall-mount keyholes in the floor (screw head up to 7.5 mm, shank up to 4 mm).
KEYHOLE_X = (20.0, 100.0)
KEYHOLE_Y = 47.0
KEYHOLE_HEAD_D, KEYHOLE_SLOT_W, KEYHOLE_SLOT_L = 8.0, 4.2, 7.0

LABEL_PIXEL = 0.6               # 5x7 font pixel for engraved labels
LABEL_DEPTH = 0.6

SEGMENTS = 64


@dataclass
class Display:
    title: str
    pcb: tuple                  # module PCB width, height
    glass: tuple                # visible glass / LCD frame width, height
    glass_offset: tuple = (0.0, 0.0)   # glass center relative to PCB center (+y = toward the header)
    glass_h: float = 2.0        # PCB front face to glass front face
    holes: tuple = ()           # mounting holes relative to PCB center
    pilot_d: float = 1.7        # standoff pilot: 1.7 M2, 2.1 M2.5, 2.5 M3 self-tapping
    standoff_d: float = 5.0     # standoff outer diameter (slim where the glass is close to the holes)
    back_depth: float = 8.0     # PCB back side: pins, wires, I2C backpack (collision check)
    window: tuple = None        # lid window, default glass + 1 mm
    center_y: float = INNER_D / 2 + 6.0
    notes: str = ""


def _corners(dx, dy):
    return ((-dx, -dy), (dx, -dy), (-dx, dy), (dx, dy))


DISPLAYS = {
    "none": Display("No display", pcb=(0, 0), glass=(0, 0)),
    "ssd1306_096": Display(
        "SSD1306 0.96in 128x64 OLED (4-pin I2C)", pcb=(27.3, 27.8), glass=(26.7, 19.3),
        glass_h=2.0, holes=_corners(11.75, 12.0), pilot_d=1.7, standoff_d=4.0, back_depth=8.0),
    "ssd1306_091": Display(
        "SSD1306 0.91in 128x32 OLED (no mounting holes)", pcb=(38.0, 12.0), glass=(30.0, 11.5),
        glass_offset=(2.5, 0), glass_h=1.6, back_depth=8.0, notes="held by corner guides, fix with hot glue"),
    "sh1106_130": Display(
        "SH1106 1.3in 128x64 OLED (4-pin I2C)", pcb=(35.4, 33.5), glass=(34.5, 23.0),
        glass_h=2.2, holes=_corners(15.2, 14.25), pilot_d=2.1, standoff_d=4.0, back_depth=8.0),
    "lcd1602": Display(
        "HD44780 16x2 LCD + PCF8574 backpack", pcb=(80.0, 36.0), glass=(71.3, 24.3), glass_h=7.0,
        holes=_corners(37.5, 15.5), pilot_d=2.5, back_depth=16.0),
    "lcd2004": Display(
        "HD44780 20x4 LCD + PCF8574 backpack", pcb=(98.0, 60.0), glass=(97.0, 40.0), glass_h=9.0,
        holes=_corners(46.5, 27.5), pilot_d=2.5, back_depth=14.0),
    "ili9341_28": Display(
        "ILI9341 2.8in 320x240 SPI TFT (landscape)", pcb=(86.0, 50.0), glass=(69.2, 50.0), glass_h=3.8,
        holes=_corners(40.0, 22.0), pilot_d=2.5, back_depth=10.0, window=(66.0, 47.0)),
    "st7789_130": Display(
        "ST7789 1.3in 240x240 IPS TFT (no mounting holes)", pcb=(27.8, 39.0), glass=(26.2, 29.2),
        glass_offset=(0, -2.5), glass_h=2.3, back_depth=10.0, notes="held by corner guides, fix with hot glue"),
}

# ---------------------------------------------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------------------------------------------

# Inside coordinates (origin at the inner bottom-left corner of the floor top) to base coordinates.
OX, OY, OZ = WALL, WALL, FLOOR
OUT_W, OUT_D = INNER_W + 2 * WALL, INNER_D + 2 * WALL
BASE_H = FLOOR + INNER_H
LID_Z = BASE_H                     # lid underside in assembled coordinates


def box(x0, y0, z0, x1, y1, z1):
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])


def ibox(x0, y0, z0, x1, y1, z1):
    """Box in inside coordinates."""
    return box(x0 + OX, y0 + OY, z0 + OZ, x1 + OX, y1 + OY, z1 + OZ)


def cyl_z(x, y, z0, h, d, d2=None, seg=SEGMENTS):
    c = Manifold.cylinder(h, d / 2, (d2 if d2 is not None else d) / 2, seg)
    return c.translate([x, y, z0])


def rounded_rect(w, d, r):
    if r <= 0:
        return CrossSection.square([w, d])
    return CrossSection.square([w - 2 * r, d - 2 * r]).translate([r, r]).offset(r, JoinType.Round,
                                                                                 circular_segments=SEGMENTS)


def teardrop_x(y, z, d, length, x0):
    """Hole along X with a 45-degree point upward, printable in a vertical wall without supports."""
    r = d / 2
    pts = [(r * math.cos(a), r * math.sin(a)) for a in np.linspace(0, 2 * math.pi, SEGMENTS, endpoint=False)]
    pts += [(0, r * math.sqrt(2))]
    cs = CrossSection.hull_points(pts)                 # in (y, z)
    m = cs.extrude(length)                             # along +Z
    # (u, v, w) = (y, z, x): map u->Y, v->Z, w->X
    m = m.transform([[0, 0, 1, 0], [1, 0, 0, 0], [0, 1, 0, 0]])
    return m.translate([x0, y, z])


def teardrop_y(x, z, d, length, y0):
    """Hole along Y with a 45-degree point upward."""
    r = d / 2
    pts = [(r * math.cos(a), r * math.sin(a)) for a in np.linspace(0, 2 * math.pi, SEGMENTS, endpoint=False)]
    pts += [(0, r * math.sqrt(2))]
    cs = CrossSection.hull_points(pts)                 # in (x, z)
    m = cs.extrude(length)
    # (u, v, w) = (x, z, y)
    m = m.transform([[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0]])
    return m.translate([x, y0, z])


def union(parts):
    parts = [p for p in parts if p is not None and not p.is_empty()]
    if not parts:
        return Manifold()
    return Manifold.batch_boolean(parts, OpType.Add)


# 5x7 font for labels (same glyphs as the firmware's display font), only the characters used here.
FONT = {
    "F": (0x7F, 0x09, 0x09, 0x09, 0x01), "1": (0x00, 0x42, 0x7F, 0x40, 0x00), "2": (0x42, 0x61, 0x51, 0x49, 0x46),
    "3": (0x21, 0x41, 0x45, 0x4B, 0x31), "G": (0x3E, 0x41, 0x49, 0x49, 0x7A), "R": (0x7F, 0x09, 0x19, 0x29, 0x46),
    "O": (0x3E, 0x41, 0x41, 0x41, 0x3E), "W": (0x3F, 0x40, 0x38, 0x40, 0x3F), "B": (0x7F, 0x49, 0x49, 0x49, 0x36),
    "X": (0x63, 0x14, 0x08, 0x14, 0x63), "L": (0x7F, 0x40, 0x40, 0x40, 0x40), "E": (0x7F, 0x49, 0x49, 0x49, 0x41),
    "D": (0x7F, 0x41, 0x41, 0x22, 0x1C), "S": (0x46, 0x49, 0x49, 0x49, 0x31), "N": (0x7F, 0x04, 0x08, 0x10, 0x7F),
    "U": (0x3F, 0x40, 0x40, 0x40, 0x3F), "C": (0x3E, 0x41, 0x41, 0x41, 0x22), "+": (0x08, 0x08, 0x3E, 0x08, 0x08),
    "-": (0x08, 0x08, 0x08, 0x08, 0x08), " ": (0, 0, 0, 0, 0), "V": (0x1F, 0x20, 0x40, 0x20, 0x1F),
    "T": (0x01, 0x01, 0x7F, 0x01, 0x01), "P": (0x7F, 0x09, 0x09, 0x09, 0x06),
}


def text_pixels(text, pixel, depth):
    """Text as voxels in a local frame: u to the right, v up, w = depth (0..depth). Width = 6 * pixel per char."""
    cubes = []
    for i, ch in enumerate(text):
        cols = FONT[ch]
        for c, bits in enumerate(cols):
            for row in range(7):
                if bits & (1 << row):
                    u = (i * 6 + c) * pixel
                    v = (6 - row) * pixel
                    cubes.append(Manifold.cube([pixel + 0.01, pixel + 0.01, depth]).translate([u, v, 0]))
    return union(cubes)


def text_width(text, pixel):
    return (len(text) * 6 - 1) * pixel


# ---------------------------------------------------------------------------------------------------------------
# Base
# ---------------------------------------------------------------------------------------------------------------

PILLAR_XY = [(0.0, 0.0), (INNER_W - PILLAR, 0.0), (0.0, INNER_D - PILLAR), (INNER_W - PILLAR, INNER_D - PILLAR)]


def pillar_centers():
    return [(x + PILLAR / 2, y + PILLAR / 2) for x, y in PILLAR_XY]


def pb_holes():
    i = PB_HOLE_INSET
    return [(PB_X + i, PB_Y + i), (PB_X + PB_W - i, PB_Y + i), (PB_X + i, PB_Y + PB_D - i),
            (PB_X + PB_W - i, PB_Y + PB_D - i)]


def devkit_rect():
    y1 = PB_Y + PB_D - DEVKIT_Y_FROM_TOP
    return (PB_X, y1 - DEVKIT_D, PB_X + DEVKIT_W, y1)


def make_base():
    shell = rounded_rect(OUT_W, OUT_D, CORNER_R).extrude(BASE_H)
    cavity = rounded_rect(INNER_W, INNER_D, max(CORNER_R - WALL, 0.5)).extrude(INNER_H + 1).translate([OX, OY, OZ])
    body = shell - cavity

    adds = []
    # Lid screw pillars.
    for x, y in PILLAR_XY:
        adds.append(ibox(x - 0.5, y - 0.5, 0, x + PILLAR, y + PILLAR, INNER_H))
    # Perfboard standoffs.
    for x, y in pb_holes():
        adds.append(cyl_z(x + OX, y + OY, OZ - 0.01, PB_STANDOFF_H + 0.01, PB_STANDOFF_D))
    body = union([body] + adds) ^ shell  # keep everything inside the outer shell

    cuts = []
    # Pilot holes.
    for x, y in pillar_centers():
        cuts.append(cyl_z(x + OX, y + OY, BASE_H - SCREW_PILOT_DEPTH, SCREW_PILOT_DEPTH + 0.1, SCREW_PILOT_D))
    for x, y in pb_holes():
        cuts.append(cyl_z(x + OX, y + OY, OZ + 1.0, PB_STANDOFF_H, PB_PILOT_D))

    # Bottom wall (Y = 0): cable holes + labels.
    for x in CABLE_X:
        for z in CABLE_Z:
            cuts.append(teardrop_y(x + OX, z + OZ, CABLE_HOLE_D, WALL + 2, -1))
    for i, x in enumerate(CABLE_X):
        label = f"F{i + 1}"
        t = text_pixels(label, LABEL_PIXEL, LABEL_DEPTH + 0.01)
        # local u -> X, v -> Z, w -> into the wall (+Y); outer face at Y = 0
        t = t.transform([[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0]])
        z0 = CABLE_Z[1] + 8.7 + 1.6
        cuts.append(t.translate([x + OX - text_width(label, LABEL_PIXEL) / 2, -0.01, z0 + OZ]))
    lbl = "LED"
    t = text_pixels(lbl, LABEL_PIXEL * 0.8, LABEL_DEPTH + 0.01).transform([[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0]])
    cuts.append(t.translate([CABLE_X[0] - 24 + OX, -0.01, CABLE_Z[0] - 2.0 + OZ]))
    lbl = "SENS"
    t = text_pixels(lbl, LABEL_PIXEL * 0.8, LABEL_DEPTH + 0.01).transform([[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0]])
    cuts.append(t.translate([CABLE_X[0] - 26 + OX, -0.01, CABLE_Z[1] - 2.0 + OZ]))

    # Left wall (X = 0): USB-C slot at the DevKit end, low vents.
    x0, y0, x1, y1 = devkit_rect()
    yc = (y0 + y1) / 2
    usb = rounded_rect(USB_SLOT_W, USB_SLOT_H, 2.0).extrude(WALL + 2)
    usb = usb.transform([[0, 0, 1, 0], [1, 0, 0, 0], [0, 1, 0, 0]])  # (y, z, x)
    cuts.append(usb.translate([-1, yc - USB_SLOT_W / 2 + OY, USB_SLOT_Z - USB_SLOT_H / 2 + OZ]))
    for i in range(6):
        cuts.append(ibox(-WALL - 1, 14 + i * 5.0, 3, 1, 16 + i * 5.0, 21))

    # Right wall (X = W): DC jack, high vents.
    cuts.append(teardrop_x(DC_JACK_Y + OY, DC_JACK_Z + OZ, DC_JACK_D, WALL + 2, OX + INNER_W - 1))
    for i in range(8):
        cuts.append(ibox(INNER_W - 1, 30 + i * 5.0, 28, INNER_W + WALL + 1, 32 + i * 5.0, 46))

    # Top wall (Y = D): high vents.
    for i in range(12):
        x = 30 + i * 5.0
        cuts.append(ibox(x, INNER_D - 1, 30, x + 2, INNER_D + WALL + 1, 46))

    # Floor: wall-mount keyholes (slot toward the top wall: hang, then slide down).
    for x in KEYHOLE_X:
        cuts.append(cyl_z(x + OX, KEYHOLE_Y + OY, -1, FLOOR + 2, KEYHOLE_HEAD_D))
        slot = CrossSection.batch_hull([CrossSection.circle(KEYHOLE_SLOT_W / 2, SEGMENTS),
                                  CrossSection.circle(KEYHOLE_SLOT_W / 2, SEGMENTS).translate([0, KEYHOLE_SLOT_L])])
        cuts.append(slot.extrude(FLOOR + 2).translate([x + OX, KEYHOLE_Y + OY, -1]))

    return body - union(cuts)


# ---------------------------------------------------------------------------------------------------------------
# Lid (assembled coordinates: underside at Z = LID_Z, outer face up)
# ---------------------------------------------------------------------------------------------------------------

def display_center(d):
    return INNER_W / 2, d.center_y


LEDGE = 2.5


def glass_rect(d, margin=0.0):
    cx, cy = display_center(d)
    gx, gy = cx + d.glass_offset[0], cy + d.glass_offset[1]
    return (gx - d.glass[0] / 2 - margin, gy - d.glass[1] / 2 - margin,
            gx + d.glass[0] / 2 + margin, gy + d.glass[1] / 2 + margin)


def ledge_rects(d):
    """LEDGE x LEDGE squares in the PCB corners that do not overlap the glass (inside coordinates)."""
    cx, cy = display_center(d)
    hx, hy = d.pcb[0] / 2, d.pcb[1] / 2
    g = glass_rect(d, 0.3)
    rects = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            x0, x1 = sorted((cx + sx * hx, cx + sx * (hx - LEDGE)))
            y0, y1 = sorted((cy + sy * hy, cy + sy * (hy - LEDGE)))
            if x1 <= g[0] or x0 >= g[2] or y1 <= g[1] or y0 >= g[3]:
                rects.append((x0, y0, x1, y1))
    return rects


def make_lid(d: Display):
    plate = rounded_rect(OUT_W, OUT_D, CORNER_R).extrude(LID_T).translate([0, 0, LID_Z])

    # Alignment lip just inside the walls, interrupted at the corner pillars.
    lip_out = rounded_rect(INNER_W - 2 * FIT, INNER_D - 2 * FIT, max(CORNER_R - WALL - FIT, 0.5))
    lip_in = CrossSection.square([INNER_W - 2 * FIT - 2 * LIP_T, INNER_D - 2 * FIT - 2 * LIP_T]).translate([LIP_T, LIP_T])
    lip = (lip_out - lip_in).extrude(LIP_H).translate([OX + FIT, OY + FIT, LID_Z - LIP_H])
    for x, y in PILLAR_XY:
        lip -= ibox(x - 1.5, y - 1.5, INNER_H - LIP_H - 1, x + PILLAR + 1.5, y + PILLAR + 1.5, INNER_H + 1)

    adds = [plate, lip]
    cuts = []

    # Corner screws, countersunk.
    for x, y in pillar_centers():
        cuts.append(cyl_z(x + OX, y + OY, LID_Z - 1, LID_T + 2, SCREW_CLEAR_D))
        sink = (SCREW_HEAD_D - SCREW_CLEAR_D) / 2
        cuts.append(cyl_z(x + OX, y + OY, LID_Z + LID_T - sink, sink + 0.01, SCREW_CLEAR_D, SCREW_HEAD_D))

    # Button.
    cuts.append(cyl_z(BUTTON_X + OX, BUTTON_Y + OY, LID_Z - 1, LID_T + 2, BUTTON_D))

    # Display.
    if d.pcb[0] > 0:
        cx, cy = display_center(d)
        gx, gy = cx + d.glass_offset[0], cy + d.glass_offset[1]
        win = d.window or (d.glass[0] + 1.0, d.glass[1] + 1.0)
        cuts.append(ibox(gx - win[0] / 2, gy - win[1] / 2, INNER_H - 1, gx + win[0] / 2, gy + win[1] / 2,
                         INNER_H + LID_T + 1))
        # Shallow recess around the window on the outer face.
        cuts.append(box(gx - win[0] / 2 - 0.8 + OX, gy - win[1] / 2 - 0.8 + OY, LID_Z + LID_T - 0.8,
                        gx + win[0] / 2 + 0.8 + OX, gy + win[1] / 2 + 0.8 + OY, LID_Z + LID_T + 1))
        stand_h = d.glass_h + 0.3   # glass sits just behind the lid
        if d.holes:
            for hx, hy in d.holes:
                x, y = cx + hx + OX, cy + hy + OY
                adds.append(cyl_z(x, y, LID_Z - stand_h, stand_h + 0.01, max(d.standoff_d, d.pilot_d + 2.2)))
                cuts.append(cyl_z(x, y, LID_Z - stand_h - 1, stand_h + 1.5, d.pilot_d))
        else:
            # Corner guides around the PCB; the module is pressed in and fixed with a dab of hot glue.
            pw, ph = d.pcb[0] + 2 * FIT, d.pcb[1] + 2 * FIT
            gh = stand_h + 1.6 + 2.0
            for sx in (-1, 1):
                for sy in (-1, 1):
                    ex, ey = cx + sx * pw / 2, cy + sy * ph / 2
                    lx0, lx1 = sorted((ex, ex + sx * 1.6))
                    ly0, ly1 = sorted((ey - sy * 6, ey + sy * 1.6))
                    adds.append(ibox(lx0, ly0, INNER_H - gh, lx1, ly1, INNER_H + 0.01))
                    lx0, lx1 = sorted((ex - sx * 6, ex + sx * 1.6))
                    ly0, ly1 = sorted((ey, ey + sy * 1.6))
                    adds.append(ibox(lx0, ly0, INNER_H - gh, lx1, ly1, INNER_H + 0.01))
            # Corner ledges the PCB front rests on (only where the PCB extends past the glass), so the glass
            # sits just behind the lid.
            for gx0, gy0, gx1, gy1 in ledge_rects(d):
                adds.append(ibox(gx0, gy0, INNER_H - stand_h, gx1, gy1, INNER_H + 0.01))

    # Name, engraved on the outer face at the top edge (reads correctly from the outside).
    name = "GROWBOX"
    t = text_pixels(name, LABEL_PIXEL, LABEL_DEPTH + 0.01)
    t = t.translate([OX + INNER_W / 2 - text_width(name, LABEL_PIXEL) / 2, OY + INNER_D - 9.0,
                     LID_Z + LID_T - LABEL_DEPTH])
    cuts.append(t)

    return union(adds) - union(cuts)


def lid_print_orientation(lid):
    """Outer face down on the bed: rotate 180 degrees about X, then put it on Z = 0."""
    m = lid.rotate([180, 0, 0])
    (x0, y0, z0, x1, y1, z1) = m.bounding_box()
    return m.translate([0, -y0, -z0])


# ---------------------------------------------------------------------------------------------------------------
# Collision check with the parts inside (assembled coordinates)
# ---------------------------------------------------------------------------------------------------------------

def inner_parts(d: Display):
    parts = {}
    x0, y0 = PB_X, PB_Y
    parts["perfboard"] = ibox(x0, y0, PB_STANDOFF_H, x0 + PB_W, y0 + PB_D, PB_STANDOFF_H + PB_T)
    parts["board parts"] = ibox(x0 + 0.5, y0 + 0.5, PB_STANDOFF_H + PB_T, x0 + PB_W - 0.5, y0 + PB_D - 0.5,
                                PB_STANDOFF_H + PB_T + PB_PARTS_H)
    for i, x in enumerate(CABLE_X):
        for j, z in enumerate(CABLE_Z):
            c = Manifold.cylinder(CABLE_INSIDE, CABLE_NUT_R, CABLE_NUT_R, 6).rotate([-90, 0, 0])
            parts[f"gland F{i + 1}{'ab'[j]}"] = c.translate([x + OX, OY, z + OZ])
    L, D = DC_JACK_BODY
    parts["DC jack"] = Manifold.cylinder(L, D / 2, D / 2, 32).rotate([0, -90, 0]).translate(
        [OX + INNER_W, DC_JACK_Y + OY, DC_JACK_Z + OZ])
    dpt, bd = BUTTON_BODY
    parts["button"] = cyl_z(BUTTON_X + OX, BUTTON_Y + OY, LID_Z - dpt, dpt, bd)
    if d.pcb[0] > 0:
        cx, cy = display_center(d)
        top = INNER_H - d.glass_h - 0.3
        parts["display"] = ibox(cx - d.pcb[0] / 2, cy - d.pcb[1] / 2, top - 1.6 - d.back_depth,
                                cx + d.pcb[0] / 2, cy + d.pcb[1] / 2, top)
        g = glass_rect(d)
        parts["display glass"] = ibox(g[0], g[1], top, g[2], g[3], INNER_H - 0.3)
    return parts


def check(base, lid, d: Display, name):
    problems = []
    parts = inner_parts(d)
    solid = union([base, lid])
    names = list(parts)
    for i, a in enumerate(names):
        ov = (parts[a] ^ solid).volume()
        # The display PCB is supposed to touch its standoffs / guides; allow that contact only.
        allowed = 0.6 if a == "display" else 0.05
        if ov > allowed:
            problems.append(f"{a} intersects the case ({ov:.2f} mm3)")
        for b in names[i + 1:]:
            if {a, b} == {"display", "display glass"}:
                continue
            ov = (parts[a] ^ parts[b]).volume()
            if ov > 0.01:
                problems.append(f"{a} intersects {b} ({ov:.2f} mm3)")
    if d.pcb[0] > 0 and not d.holes and len(ledge_rects(d)) < 2:
        problems.append("fewer than 2 corner ledges hold the display")
    # The display must fit inside the walls.
    if d.pcb[0] > 0:
        cx, cy = display_center(d)
        if cx - d.pcb[0] / 2 < FIT or cx + d.pcb[0] / 2 > INNER_W - FIT or \
                cy - d.pcb[1] / 2 < FIT or cy + d.pcb[1] / 2 > INNER_D - FIT:
            problems.append("display PCB does not fit inside the walls")
    status = "OK" if not problems else "PROBLEMS"
    print(f"  check {name}: {status}")
    for p in problems:
        print(f"    - {p}")
    return not problems


# ---------------------------------------------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------------------------------------------

def write_stl(m: Manifold, path):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3].astype(np.float32)
    t = np.asarray(mesh.tri_verts).astype(np.int64)
    tri = v[t]
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    n = (n / np.where(ln == 0, 1, ln)).astype(np.float32)
    rec = np.zeros(len(t), dtype=[("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")])
    rec["n"] = n
    rec["v"] = tri
    with open(path, "wb") as f:
        header = b"GrowBox-C6 enclosure, generated by case/growbox_case.py"
        f.write(header.ljust(80, b" "))
        f.write(np.uint32(len(t)).tobytes())
        f.write(rec.tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "stl"))
    ap.add_argument("--display", action="append", choices=sorted(DISPLAYS), help="only these lids (repeatable)")
    ap.add_argument("--check", action="store_true", help="check the inside parts for collisions")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    base = make_base()
    if base.status().name != "NoError" if hasattr(base.status(), "name") else False:
        print("base geometry error", base.status())
    write_stl(base, os.path.join(args.out, "base.stl"))
    print(f"base.stl: {OUT_W:.1f} x {OUT_D:.1f} x {BASE_H:.1f} mm, {base.volume() / 1000:.1f} cm3")

    ok = True
    for name in args.display or DISPLAYS:
        d = DISPLAYS[name]
        lid = make_lid(d)
        write_stl(lid_print_orientation(lid), os.path.join(args.out, f"lid_{name}.stl"))
        print(f"lid_{name}.stl: {d.title}")
        if args.check:
            ok &= check(base, lid, d, name)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

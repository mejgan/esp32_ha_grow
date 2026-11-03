#!/usr/bin/env python3
"""Draws docs/board_layout.png: a suggested layout of the 7 x 9 cm perfboard inside the case (top view, cable side
at the bottom, power/USB side on the right). Positions match case/growbox_case.py. Needs Pillow.

    python docs/make_board_layout.py
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "case"))
import growbox_case as gc  # noqa: E402

S = 2       # supersampling
MM = 7.0    # pixels per mm
PAD = 40


def font(size, bold=False):
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    for d in ("/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/TTF", "/Library/Fonts", "C:/Windows/Fonts"):
        p = os.path.join(d, name)
        if os.path.exists(p):
            return ImageFont.truetype(p, size * S)
    return ImageFont.load_default(size * S)


def main():
    inner_w, inner_d = gc.INNER_W, gc.INNER_D
    W = int(inner_w * MM + 2 * PAD + 330)
    H = int(inner_d * MM + 2 * PAD + 150)
    img = Image.new("RGB", (W * S, H * S), (255, 255, 255))
    d = ImageDraw.Draw(img)
    f_title, f_head, f_txt, f_small = font(20, True), font(13, True), font(12), font(10)

    def P(x_mm, y_mm):
        """Inside-case coordinates (mm, y up) to pixels."""
        return ((PAD + x_mm * MM) * S, (PAD + 50 + (inner_d - y_mm) * MM) * S)

    def rect(x0, y0, x1, y1, fill, outline=(60, 60, 60), width=2):
        a, b = P(x0, y1), P(x1, y0)
        d.rectangle([*a, *b], fill=fill, outline=outline, width=width * S)

    def label(x, y, text, f=f_txt, fill=(0, 0, 0), anchor="mm"):
        d.text(P(x, y), text, font=f, fill=fill, anchor=anchor)

    d.text((PAD * S, 14 * S), "Suggested perfboard layout (top view, inside the case)", font=f_title, fill=(0, 0, 0))

    # Case inside outline, walls.
    rect(0, 0, inner_w, inner_d, (247, 247, 247), (120, 120, 120), 3)
    label(inner_w / 2, -15, "bottom wall: cable glands (LED lower row, sensors/camera upper row)", f_small,
          (90, 90, 90))
    for x, y in gc.PILLAR_XY:
        rect(x, y, x + gc.PILLAR, y + gc.PILLAR, (210, 210, 210), (150, 150, 150), 1)

    # Glands.
    for i, x in enumerate(gc.CABLE_X):
        d.ellipse([*P(x - 6, 3), *P(x + 6, -9)], outline=(120, 120, 120), width=2 * S)
        label(x, -3, f"F{i + 1}", f_head, (90, 90, 90))

    # Perfboard.
    px, py, pw, ph = gc.PB_X, gc.PB_Y, gc.PB_W, gc.PB_D
    rect(px, py, px + pw, py + ph, (200, 225, 200), (60, 120, 60), 2)
    for gx in range(int(px) + 2, int(px + pw), 3):
        for gy in range(int(py) + 2, int(py + ph), 3):
            cx, cy = P(gx, gy)
            d.point((cx, cy), fill=(140, 170, 140))
    for hx, hy in gc.pb_holes():
        d.ellipse([*P(hx - 1.2, hy + 1.2), *P(hx + 1.2, hy - 1.2)], fill=(255, 255, 255), outline=(60, 120, 60))

    # Parts on the board (board coordinates converted to inside coordinates).
    def part(bx, by, w, h, title, sub="", color=(255, 255, 255)):
        x0, y0 = px + bx, py + by
        rect(x0, y0, x0 + w, y0 + h, color)
        label(x0 + w / 2, y0 + h / 2 + (2 if sub else 0), title, f_head)
        if sub:
            label(x0 + w / 2, y0 + h / 2 - 3, sub, f_small, (60, 60, 60))

    x0, y0, x1, y1 = gc.devkit_rect()
    rect(x0, y0, x1, y1, (60, 90, 160))
    label((x0 + x1) / 2, (y0 + y1) / 2 + 2, "ESP32-C6-DevKitC-1", f_head, (255, 255, 255))
    label((x0 + x1) / 2, (y0 + y1) / 2 - 3, "on female headers, USB end left", f_small, (230, 230, 255))
    part(60, 50, 26, 16, "buck 12V->5V", "MP1584 / Mini-360", (255, 240, 220))
    for k in range(3):
        part(57 + k * 11, 28, 9, 14, f"Q{k + 1}", "MOSFET", (235, 235, 235))
    x0g, y0g = px + 57, py + 21
    rect(x0g, y0g, x0g + 31, y0g + 6, (250, 250, 250))
    label(x0g + 15.5, y0g + 3, "100R + 10k per gate", f_small)
    part(2, 26, 44, 10, "AUX headers", "display I2C, button, USB-C, room sensor", (255, 255, 255))
    cols = [x - px for x in gc.CABLE_X]
    for i, c in enumerate(cols):
        part(min(max(c - 10, 1), pw - 21), 10, 20, 7, f"F{i + 1} sensors", "", (255, 250, 210))
        part(min(max(c - 4.5, 1), pw - 13.5), 1, 9, 7, f"F{i + 1} LED", "", (255, 225, 225))

    # Off-board parts on the right wall.
    jy, uy = gc.DC_JACK_Y, gc.USBC_Y
    rect(inner_w - 14, jy - 5, inner_w, jy + 5, (255, 220, 220))
    label(inner_w - 7, jy, "DC jack", f_small)
    rect(inner_w - gc.USBC_BOARD[1], uy - gc.USBC_BOARD[0] / 2, inner_w, uy + gc.USBC_BOARD[0] / 2,
         (220, 220, 240))
    label(inner_w - gc.USBC_BOARD[1] / 2, uy, "USB-C", f_small)

    # Notes on the right.
    nx = (PAD + inner_w * MM + 40) * S
    notes = [
        ("Power", ["DC jack + -> fuse -> buck IN+ and 12 V rail", "buck OUT 5.0 V -> DevKit 5V", "thick wire (18 AWG)",
                   "for 12 V and GND to the MOSFETs and", "LED terminals: perfboard strips can't", "carry the strip current"]),
        ("LED channels", ["Q1-Q3 logic-level N-MOSFETs", "(IRLZ44N, IRLB8721, AO3400)", "gate: 100R from GPIO19/20/21",
                          "10k gate to GND", "source GND, drain -> strip -", "strip + -> 12 V"]),
        ("Connectors", ["per floor: 2-pin screw terminal (LED)", "and an 8-pin (12 with a camera)",
                        "JST-XH or screw terminal (sensors)", "sensors on a Cat5 / 12-core cable,",
                        "each signal twisted with GND", "pins: docs/WIRING.md"]),
        ("Right wall", ["DC jack and the USB-C breakout", "(cradle in the case) wire to the board"]),
    ]
    y = (PAD + 50) * S
    for head, lines in notes:
        d.text((nx, y), head, font=f_head, fill=(0, 0, 0))
        y += 20 * S
        for line in lines:
            d.text((nx, y), line, font=f_txt, fill=(50, 50, 50))
            y += 17 * S
        y += 14 * S

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "board_layout.png")
    img.resize((W, H), Image.LANCZOS).save(out, optimize=True)
    print("wrote", out)


if __name__ == "__main__":
    main()

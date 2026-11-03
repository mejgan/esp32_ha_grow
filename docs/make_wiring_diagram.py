#!/usr/bin/env python3
"""Draws docs/wiring.png, the overview of what connects to which ESP32-C6 pin (default pins). Needs Pillow.

    python docs/make_wiring_diagram.py
"""

import os

from PIL import Image, ImageDraw, ImageFont

S = 2  # supersampling
W, H = 1500, 940

COLORS = {
    "power": (214, 39, 40),
    "gnd": (40, 40, 40),
    "i2c": (31, 119, 180),
    "lp_i2c": (23, 190, 207),
    "analog": (255, 127, 14),
    "digital": (148, 103, 189),
    "pwm": (188, 160, 0),
    "spi": (44, 160, 44),
    "usb": (127, 127, 127),
}

# (pin label, side, signal kind). Order = top to bottom on the board drawing.
LEFT = [
    ("3V3", "power"), ("GPIO0", "analog"), ("GPIO1", "analog"), ("GPIO2", "analog"), ("GPIO10", "digital"),
    ("GPIO6", "lp_i2c"), ("GPIO7", "lp_i2c"), ("GPIO3", "spi"), ("GPIO4", "spi"), ("GPIO5", "spi"),
    ("GPIO11", "spi"), ("GND", "gnd"),
]
RIGHT = [
    ("5V", "power"), ("GPIO18", "digital"), ("GPIO19", "pwm"), ("GPIO20", "pwm"), ("GPIO21", "pwm"),
    ("GPIO22", "i2c"), ("GPIO23", "i2c"), ("GPIO9", "digital"), ("GPIO15", "digital"), ("GPIO13", "usb"),
    ("GPIO12", "usb"), ("GND", "gnd"),
]

# Modules: title, lines, side, list of (module pin text, board pin label)
MODULES_LEFT = [
    ("Floor 1 - capacitive soil", ["VCC 3V3, GND"], [("AOUT", "GPIO0")]),
    ("Floor 2 - YL-69 / FC-28 soil", ["GND; DO unused", "powered from GPIO10 only while reading"],
     [("AO", "GPIO1"), ("VCC", "GPIO10")]),
    ("Floor 3 - TEMT6000 light", ["VCC 3V3, GND"], [("SIG", "GPIO2")]),
    ("Floor 2 - AHT10 #1 (LP I2C)", ["VIN 3V3, GND", "address 0x38"], [("SDA", "GPIO6"), ("SCL", "GPIO7")]),
    ("SPI TFT (optional)", ["ST7789 / ILI9341", "VCC 3V3, GND, BL 3V3, RST 3V3"],
     [("DC", "GPIO3"), ("SCL/SCK", "GPIO4"), ("SDA/MOSI", "GPIO5"), ("CS", "GPIO11")]),
]
MODULES_RIGHT = [
    ("Floor 1 - DHT11", ["VCC 3V3, GND", "10k pull-up DATA to 3V3"], [("DATA", "GPIO18")]),
    ("3x logic-level MOSFET", ["gate via 100R, 10k gate-GND", "drain -> LED strip -, strip + -> 12V"],
     [("Gate F1", "GPIO19"), ("Gate F2", "GPIO20"), ("Gate F3", "GPIO21")]),
    ("I2C bus (floor 3 + extras)", ["Floor 3 AHT10 #2 (0x38) + BMP180 (0x77)",
                                    "optional: OLED/LCD display, room sensor", "all VCC 3V3, GND"],
     [("SDA", "GPIO22"), ("SCL", "GPIO23")]),
    ("Panel push button", ["other leg to GND (parallel to BOOT)"], [("pin", "GPIO9")]),
    ("Room DHT22/DHT11 (optional)", ["VCC 3V3, GND, 10k pull-up"], [("DATA", "GPIO15")]),
    ("USB-C breakout (case socket)", ["GND to GND; VBUS via 1N5819 to 5V (opt.)"],
     [("D+", "GPIO13"), ("D-", "GPIO12")]),
]


def font(size, bold=False):
    names = ["DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"]
    for n in names:
        for d in ("/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/TTF", "/Library/Fonts", "C:/Windows/Fonts"):
            p = os.path.join(d, n)
            if os.path.exists(p):
                return ImageFont.truetype(p, size * S)
    return ImageFont.load_default(size * S)


def main():
    img = Image.new("RGB", (W * S, H * S), (255, 255, 255))
    d = ImageDraw.Draw(img)
    f_title = font(22, True)
    f_head = font(14, True)
    f_txt = font(12)
    f_pin = font(13, True)

    def P(x, y):
        return (x * S, y * S)

    d.text(P(30, 20), "GrowBox-C6 wiring (default pins, change them in menuconfig -> Grow Box)", font=f_title,
           fill=(0, 0, 0))

    # Power path at the top.
    py = 70
    boxes = [("12 V DC jack", 30), ("buck 12V -> 5V (>= 1 A)", 300), ("DevKit 5V pin", 620)]
    for text, x in boxes:
        d.rounded_rectangle([*P(x, py), *P(x + 230, py + 40)], radius=8 * S, outline=COLORS["power"], width=2 * S)
        d.text(P(x + 12, py + 11), text, font=f_head, fill=(0, 0, 0))
    for a, b in ((260, 300), (530, 620)):
        d.line([*P(a, py + 20), *P(b, py + 20)], fill=COLORS["power"], width=3 * S)
    d.line([*P(145, py + 40), *P(145, py + 70), *P(1060, py + 70), *P(1060, py + 40)], fill=COLORS["power"],
           width=3 * S)
    d.rounded_rectangle([*P(940, py), *P(1250, py + 40)], radius=8 * S, outline=COLORS["power"], width=2 * S)
    d.text(P(952, py + 11), "LED strips + (all three)", font=f_head, fill=(0, 0, 0))
    d.text(P(870, py + 46), "12 V", font=f_txt, fill=COLORS["power"])
    d.text(P(30, py + 80), "One common GND for everything: supply -, buck, DevKit, MOSFET sources, sensors.",
           font=f_txt, fill=(0, 0, 0))

    # Board.
    bx0, bx1, by0 = 620, 860, 200
    pitch = 52
    rows = max(len(LEFT), len(RIGHT))
    by1 = by0 + 40 + rows * pitch
    d.rounded_rectangle([*P(bx0, by0), *P(bx1, by1)], radius=12 * S, fill=(235, 242, 250), outline=(60, 60, 60),
                        width=2 * S)
    d.text(P(bx0 + 30, by0 + 8), "ESP32-C6-DevKitC-1", font=f_head, fill=(0, 0, 0))
    pin_y = {}
    for i, (label, kind) in enumerate(LEFT):
        y = by0 + 50 + i * pitch
        d.ellipse([*P(bx0 - 5, y - 5), *P(bx0 + 5, y + 5)], fill=COLORS[kind])
        d.text(P(bx0 + 10, y - 9), label, font=f_pin, fill=COLORS[kind])
        pin_y[("L", label)] = y
    for i, (label, kind) in enumerate(RIGHT):
        y = by0 + 50 + i * pitch
        d.ellipse([*P(bx1 - 5, y - 5), *P(bx1 + 5, y + 5)], fill=COLORS[kind])
        tw = d.textlength(label, font=f_pin) / S
        d.text(P(bx1 - 10 - tw, y - 9), label, font=f_pin, fill=COLORS[kind])
        pin_y[("R", label)] = y
    kind_of = {lbl: k for lbl, k in LEFT + RIGHT}

    def draw_modules(mods, side):
        """Draws the module boxes and returns their wires: (module x, module y, board y, color)."""
        wires = []
        y = by0 - 10
        for title, lines, pins in mods:
            h = 26 + 16 * len(lines) + 20 * len(pins) + 8
            x0, x1 = (30, 400) if side == "L" else (1080, 1470)
            d.rounded_rectangle([*P(x0, y), *P(x1, y + h)], radius=8 * S, outline=(90, 90, 90), width=2 * S,
                                fill=(250, 250, 250))
            d.text(P(x0 + 10, y + 6), title, font=f_head, fill=(0, 0, 0))
            for k, line in enumerate(lines):
                d.text(P(x0 + 10, y + 26 + 16 * k), line, font=f_txt, fill=(70, 70, 70))
            for k, (mpin, bpin) in enumerate(pins):
                py_ = y + 26 + 16 * len(lines) + 20 * k + 8
                color = COLORS[kind_of[bpin]]
                if side == "L":
                    tx = x1 - 10 - d.textlength(mpin, font=f_pin) / S
                    d.text(P(tx, py_ - 9), mpin, font=f_pin, fill=color)
                    d.ellipse([*P(x1 - 5, py_ - 5), *P(x1 + 5, py_ + 5)], fill=color)
                    wires.append((x1, py_, pin_y[("L", bpin)], color))
                else:
                    d.text(P(x0 + 10, py_ - 9), mpin, font=f_pin, fill=color)
                    d.ellipse([*P(x0 - 5, py_ - 5), *P(x0 + 5, py_ + 5)], fill=color)
                    wires.append((x0, py_, pin_y[("R", bpin)], color))
            y += h + 14
        return wires

    def route(wires, side):
        """Gives every wire a vertical lane between the modules and the board: no two wires share a lane where
        their vertical runs overlap, and lanes are picked to minimize crossings."""
        if side == "L":
            lane_x = [415 + 13 * i for i in range(15)]      # index 0 = nearest the modules
        else:
            lane_x = [1065 - 13 * i for i in range(15)]
        placed = []  # (lane index, module y, board y)

        def crossings(lane, my, by):
            n = 0
            for pl, pm, pb in placed:
                lo, hi = sorted((pm, pb))
                plo, phi = sorted((my, by))
                if pl < lane and lo < my < hi:      # our module-side run crosses their vertical
                    n += 1
                if pl > lane and lo < by < hi:      # our board-side run crosses their vertical
                    n += 1
                if lane < pl and plo < pm < phi:    # their module-side run crosses our vertical
                    n += 1
                if lane > pl and plo < pb < phi:    # their board-side run crosses our vertical
                    n += 1
            return n

        order = sorted(range(len(wires)), key=lambda i: -abs(wires[i][2] - wires[i][1]))
        lanes = {}
        for i in order:
            _, my, by, _ = wires[i]
            lo, hi = sorted((my, by))
            best = None
            for lane in range(len(lane_x)):
                if any(pl == lane and not (hi + 6 < min(pm, pb) or lo - 6 > max(pm, pb)) for pl, pm, pb in placed):
                    continue
                cost = (crossings(lane, my, by), lane)
                if best is None or cost < best[0]:
                    best = (cost, lane)
            lanes[i] = best[1]
            placed.append((best[1], my, by))
        board_x = bx0 if side == "L" else bx1
        for i, (mx, my, by, color) in enumerate(wires):
            x = lane_x[lanes[i]]
            d.line([*P(mx, my), *P(x, my), *P(x, by), *P(board_x, by)], fill=color, width=3 * S)

    route(draw_modules(MODULES_LEFT, "L"), "L")
    route(draw_modules(MODULES_RIGHT, "R"), "R")

    # Legend.
    ly = by1 + 30
    x = 30
    for kind, label in (("power", "power"), ("gnd", "GND"), ("i2c", "I2C"), ("lp_i2c", "LP I2C"),
                        ("analog", "analog (ADC1)"), ("digital", "digital"), ("pwm", "PWM"), ("spi", "SPI"),
                        ("usb", "USB")):
        d.line([*P(x, ly + 8), *P(x + 30, ly + 8)], fill=COLORS[kind], width=4 * S)
        d.text(P(x + 36, ly), label, font=f_txt, fill=(0, 0, 0))
        x += 60 + d.textlength(label, font=f_txt) / S

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wiring.png")
    img.resize((W, H), Image.LANCZOS).save(out, optimize=True)
    print("wrote", out)


if __name__ == "__main__":
    main()

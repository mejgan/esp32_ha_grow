#!/usr/bin/env python3
"""Renders preview images of the enclosure (orthographic, flat shaded, with edge lines). Needs numpy and Pillow.

    python case/render_preview.py            # -> case/preview/*.png
"""

import math
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import growbox_case as gc  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def mesh_arrays(m):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3].astype(np.float64)
    t = np.asarray(mesh.tri_verts).astype(np.int64)
    return v[t]  # (n, 3, 3)


def view_matrix(azimuth_deg, elevation_deg):
    az, el = math.radians(azimuth_deg), math.radians(elevation_deg)
    # Camera looks from direction (cos el * sin az, -cos el * cos az, sin el) toward the origin.
    rz = np.array([[math.cos(az), -math.sin(az), 0], [math.sin(az), math.cos(az), 0], [0, 0, 1]])
    rx = np.array([[1, 0, 0], [0, math.sin(el), math.cos(el)], [0, -math.cos(el), math.sin(el)]])
    return rx @ rz  # world -> camera (x right, y up, z toward the viewer)


def render(objects, azimuth=-35, elevation=28, width=1000, height=750, margin=30, bg=(245, 246, 248)):
    """objects: list of (Manifold, (r, g, b)). Returns a PIL image."""
    R = view_matrix(azimuth, elevation)
    tris, cols = [], []
    for m, color in objects:
        t = mesh_arrays(m)
        tris.append(t)
        cols.append(np.tile(np.array(color, dtype=np.float64), (len(t), 1)))
    tris = np.concatenate(tris) @ R.T
    cols = np.concatenate(cols)

    lo = tris.reshape(-1, 3).min(axis=0)
    hi = tris.reshape(-1, 3).max(axis=0)
    scale = min((width - 2 * margin) / (hi[0] - lo[0]), (height - 2 * margin) / (hi[1] - lo[1]))
    ox = (width - (hi[0] - lo[0]) * scale) / 2
    oy = (height - (hi[1] - lo[1]) * scale) / 2
    px = (tris[:, :, 0] - lo[0]) * scale + ox
    py = height - ((tris[:, :, 1] - lo[1]) * scale + oy)
    pz = tris[:, :, 2]

    n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    n = n / np.where(ln == 0, 1, ln)
    light = np.array([-0.35, 0.55, 0.76])
    light /= np.linalg.norm(light)
    shade = 0.35 + 0.65 * np.clip(np.abs(n @ light), 0, 1)

    zbuf = np.full((height, width), -np.inf)
    img = np.zeros((height, width, 3))
    img[:] = bg
    ids = np.full((height, width), -1, dtype=np.int64)

    for i in range(len(tris)):
        x0, x1, x2 = px[i]
        y0, y1, y2 = py[i]
        minx, maxx = int(max(math.floor(min(x0, x1, x2)), 0)), int(min(math.ceil(max(x0, x1, x2)), width - 1))
        miny, maxy = int(max(math.floor(min(y0, y1, y2)), 0)), int(min(math.ceil(max(y0, y1, y2)), height - 1))
        if minx > maxx or miny > maxy:
            continue
        den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(den) < 1e-12:
            continue
        xs, ys = np.meshgrid(np.arange(minx, maxx + 1) + 0.5, np.arange(miny, maxy + 1) + 0.5)
        a = ((y1 - y2) * (xs - x2) + (x2 - x1) * (ys - y2)) / den
        b = ((y2 - y0) * (xs - x2) + (x0 - x2) * (ys - y2)) / den
        c = 1 - a - b
        inside = (a >= -1e-6) & (b >= -1e-6) & (c >= -1e-6)
        if not inside.any():
            continue
        z = a * pz[i, 0] + b * pz[i, 1] + c * pz[i, 2]
        sub = zbuf[miny:maxy + 1, minx:maxx + 1]
        upd = inside & (z > sub)
        sub[upd] = z[upd]
        img[miny:maxy + 1, minx:maxx + 1][upd] = cols[i] * shade[i]
        ids[miny:maxy + 1, minx:maxx + 1][upd] = i

    # Edge lines: depth jumps and creases between faces with different normals.
    edge = np.zeros((height, width), dtype=bool)
    finite = np.isfinite(zbuf)
    zb = np.where(finite, zbuf, lo[2] - 1000)
    nn = np.zeros((height, width, 3))
    nn[ids >= 0] = n[ids[ids >= 0]]
    for dy, dx in ((0, 1), (1, 0)):
        za, zc = zb[:-dy or None, :-dx or None], zb[dy:, dx:]
        na, nc = nn[:-dy or None, :-dx or None], nn[dy:, dx:]
        e = (np.abs(za - zc) > 1.2) | ((na * nc).sum(axis=2) < 0.85)
        edge[:-dy or None, :-dx or None] |= e
    img[edge] = img[edge] * 0.35
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))


def tint(c):
    return tuple(int(v) for v in c)


def main():
    out = os.path.join(HERE, "preview")
    os.makedirs(out, exist_ok=True)
    base = gc.make_base()
    body = (232, 236, 240)
    lid_c = (120, 196, 140)
    pcb_c = (40, 120, 70)

    # Assembled-ish: base with the lid lifted, the perfboard and display envelopes for reference.
    d = gc.DISPLAYS["ssd1306_096"]
    lid = gc.make_lid(d).translate([0, 0, 35])
    parts = gc.inner_parts(d)
    objs = [(base, body), (lid, lid_c), (parts["perfboard"], pcb_c),
            (parts["display"].translate([0, 0, 35]), (30, 30, 40))]
    render(objs, azimuth=-35, elevation=30).save(os.path.join(out, "exploded.png"))

    # Base from the cable side and from the USB side.
    render([(base, body)], azimuth=-20, elevation=22).save(os.path.join(out, "base_cables.png"))
    render([(base, body)], azimuth=-120, elevation=22).save(os.path.join(out, "base_ports.png"))
    # Close-up of the USB-C cradle from inside (front walls cut away), with the breakout board in place.
    if gc.USB_PORT == "panel":
        cut = gc.box(gc.OX + gc.INNER_W - 22, gc.OY + gc.USBC_Y - 16, -1, gc.OUT_W + 1, gc.OY + gc.USBC_Y + 16,
                     gc.OZ + gc.USBC_Z + 12)
        parts = gc.inner_parts(gc.DISPLAYS["none"])
        render([(base ^ cut, body), (parts["usb board"], pcb_c), (parts["usb socket"], (150, 150, 160))],
               azimuth=110, elevation=35, width=800, height=600).save(os.path.join(out, "usb_cradle.png"))
    render([(base, body)], azimuth=150, elevation=-40).save(os.path.join(out, "base_back.png"))

    # Every lid from the outside, plus one from the inside.
    tiles = []
    for name, disp in gc.DISPLAYS.items():
        img = render([(gc.make_lid(disp), lid_c)], azimuth=0, elevation=90, width=500, height=380, margin=12)
        tiles.append((name, img))
    w, h = 500, 380
    cols = 4
    rows = math.ceil(len(tiles) / cols)
    sheet = Image.new("RGB", (cols * w, rows * (h + 24)), (255, 255, 255))
    from PIL import ImageDraw
    draw = ImageDraw.Draw(sheet)
    for i, (name, img) in enumerate(tiles):
        x, y = (i % cols) * w, (i // cols) * (h + 24)
        sheet.paste(img, (x, y + 24))
        draw.text((x + 10, y + 6), f"lid_{name}.stl", fill=(0, 0, 0))
    sheet.save(os.path.join(out, "lids.png"))

    inside = gc.make_lid(gc.DISPLAYS["lcd2004"])
    render([(inside, lid_c)], azimuth=20, elevation=-55).save(os.path.join(out, "lid_inside_lcd2004.png"))
    print("previews written to", out)


if __name__ == "__main__":
    main()

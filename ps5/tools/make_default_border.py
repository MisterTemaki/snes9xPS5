#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds frontend/assets/borders/default.png: the border that is built into the app (Settings > Border > Default frame).

An original design, drawn here pixel by pixel: nothing is copied from any other border or bezel. A dark graphite
console cabinet, a bevel around a rounded glass opening, a speaker grille on the left, two dials and a power light on
the right, and an empty name plate at the bottom left.

The picture goes in the opening. The file follows the border format of the app (docs/borders in the README):
1920x1080, a palette PNG with a tRNS chunk (the format of The Bezel Project's files), the opening fully clear. Just
outside the opening is a 6 pixel soft edge, from nearly clear to opaque, so the glass fades into the cabinet. The
opening is the 1398x1038 rectangle at (261, 21) with 36 pixel rounded corners; the four corner pieces are opaque
cabinet that the app redraws over the picture every frame.

The output is deterministic (same bytes on every run with the same Pillow) and the script checks what it wrote.

  python3 tools/make_default_border.py [--out frontend/assets/borders/default.png] [--preview picture.png]   (from ps5/)

Needs numpy and Pillow, which is why the PNG is checked in: the build does not run this.
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

W, H = 1920, 1080
WIN_X, WIN_Y, WIN_W, WIN_H = 261, 21, 1398, 1038  # the opening (the window of the border)
CORNER = 36.0  # radius of the glass corners
SOFT = 6.0  # the soft edge outside the opening
CLEAR_BELOW = 16  # an alpha below this is "clear" to the app (it finds the window with it)
LIP = (9, 9, 12)  # the colour of the soft edge: the dark lip around the glass
COLORS = 214  # palette entries for the dark cabinet
BRIGHT = 24  # and for its few bright details (the pointers, the light): quantised apart so the cabinet cannot take their colours
BRIGHT_ABOVE = 110  # a pixel with a channel above this is a bright detail
FILE_LIMIT = 150 * 1024
LIGHT = np.array([-0.6, -0.8])  # light from the top left

X, Y = np.meshgrid(np.arange(W, dtype=np.float32) + 0.5, np.arange(H, dtype=np.float32) + 0.5)


def smoothstep(a, b, v):
    t = np.clip((v - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def round_rect_sdf(cx, cy, hw, hh, r):
    """Distance to a rounded rectangle: negative inside, positive outside."""
    qx = np.abs(X - cx) - (hw - r)
    qy = np.abs(Y - cy) - (hh - r)
    outside = np.hypot(np.maximum(qx, 0.0), np.maximum(qy, 0.0))
    inside = np.minimum(np.maximum(qx, qy), 0.0)
    return outside + inside - r


def disc_sdf(cx, cy, r):
    return np.hypot(X - cx, Y - cy) - r


def coverage(sdf):
    """Anti-aliased mask of a shape from its distance field."""
    return np.clip(0.5 - sdf, 0.0, 1.0)


def mix(base, color, amount):
    """base (H, W, 3) moved towards color by amount (H, W)."""
    return base + (np.array(color, dtype=np.float32) - base) * amount[..., None]


def draw():
    # ---- the cabinet: graphite, a little lighter at the top and towards the opening ----------------------------
    d_win = round_rect_sdf(WIN_X + WIN_W / 2, WIN_Y + WIN_H / 2, WIN_W / 2, WIN_H / 2, CORNER)  # <0 in the opening
    gy, gx = np.gradient(d_win)
    norm = np.maximum(np.hypot(gx, gy), 1e-6)
    shade = -(gx * LIGHT[0] + gy * LIGHT[1]) / norm  # +1 where the bevel slope faces the light (recessed opening)

    v = 38.0 - 14.0 * (Y / H) + 6.0 * (1.0 - smoothstep(30.0, 300.0, d_win))
    cab = np.stack([v * 0.95, v * 0.99, v * 1.08], axis=-1)

    # the cabinet's own edge: a dark line at the screen edge, a faint highlight just inside it
    d_edge = np.minimum(np.minimum(X, W - X), np.minimum(Y, H - Y))
    cab = mix(cab, (6, 6, 9), 1.0 - smoothstep(0.0, 4.0, d_edge))
    cab = mix(cab, (70, 74, 86), 0.5 * (smoothstep(3.0, 4.0, d_edge) - smoothstep(5.0, 6.0, d_edge)))

    # ---- the bevel around the opening: dark lip, then a sloped ring, then a thin highlight ----------------------
    lip = 1.0 - smoothstep(SOFT + 0.5, SOFT + 6.0, d_win)  # 1 on the lip, 0 outside it
    cab = mix(cab, LIP, lip)
    t = np.clip((d_win - (SOFT + 6.0)) / 18.0, 0.0, 1.0)  # across the sloped ring
    ring = (t > 0) & (t < 1)
    ring_v = 36.0 + 20.0 * shade * np.sin(np.pi * t)
    ring_color = np.stack([ring_v * 0.95, ring_v * 0.99, ring_v * 1.08], axis=-1)
    cab = mix(cab, ring_color, ring.astype(np.float32) * smoothstep(0.0, 0.08, t) * (1.0 - smoothstep(0.92, 1.0, t)))
    hi = np.exp(-(((d_win - (SOFT + 25.0)) / 1.1) ** 2))
    hi_v = 54.0 + 40.0 * np.clip(shade, 0.0, 1.0)
    cab = cab +(hi[..., None] * (hi_v[..., None] - 36.0)) * np.array([0.95, 0.99, 1.08], dtype=np.float32)

    # ---- left: a speaker grille, 16 slits ---------------------------------------------------------------------
    for i in range(16):
        cy = 364 + 22 * i + 11
        slit = round_rect_sdf(116.0, cy, 60.0, 4.5, 4.5)
        edge = round_rect_sdf(116.0, cy + 1.5, 60.0, 4.5, 4.5)
        cab = mix(cab, (62, 66, 76), coverage(edge) * (1.0 - coverage(slit)) * 0.8)  # lit lower lip
        cab = mix(cab, (4, 4, 6), coverage(slit))

    # ---- left, bottom: an empty name plate, sunk into the cabinet -------------------------------------------------
    plate = round_rect_sdf(130.0, 988.0, 90.0, 28.0, 6.0)
    cab = mix(cab, (22, 23, 28), coverage(plate))
    inner_shadow = coverage(plate) * (1.0 - coverage(round_rect_sdf(131.0, 989.0, 90.0, 28.0, 6.0)))  # top left lip
    cab = mix(cab, (10, 10, 13), inner_shadow)
    lit_lip = coverage(round_rect_sdf(130.0, 988.0, 91.0, 29.0, 7.0)) * (1.0 - coverage(plate)) * (Y > 988)
    cab = mix(cab, (66, 70, 82), lit_lip * 0.7)

    # ---- right: two dials and a power light ---------------------------------------------------------------------
    for cy in (330.0, 480.0):
        cx = 1805.0
        well = disc_sdf(cx, cy, 44.0)
        cab = mix(cab, (7, 7, 10), coverage(well))
        rim_lit = coverage(disc_sdf(cx + 0.8, cy + 1.2, 44.0)) * (1.0 - coverage(well))
        cab = mix(cab, (62, 66, 78), rim_lit * 0.8)
        knob = disc_sdf(cx, cy, 36.0)
        face = 66.0 - 22.0 * ((X - cx) * 0.6 + (Y - cy) * 0.8 + 36.0) / 72.0  # lighter towards the light
        face = np.clip(face, 30.0, 80.0)
        face_color = np.stack([face * 0.95, face * 0.99, face * 1.08], axis=-1)
        cab = mix(cab, face_color, coverage(knob))
        cab = mix(cab, (14, 14, 18), coverage(disc_sdf(cx, cy, 36.0)) * (1.0 - coverage(disc_sdf(cx, cy, 33.5))))
        # the pointer: a short bar at 40 degrees to the left of up
        ang = np.deg2rad(-40.0)
        ux, uy = np.sin(ang), -np.cos(ang)
        along = (X - cx) * ux + (Y - cy) * uy
        across = np.abs(-(X - cx) * uy + (Y - cy) * ux)
        bar = np.clip(0.5 - np.maximum(across - 2.2, np.maximum(8.0 - along, along - 28.0)), 0.0, 1.0)
        cab = mix(cab, (196, 200, 210), bar)
    led = disc_sdf(1805.0, 640.0, 8.0)
    cab = mix(cab, (6, 6, 9), coverage(disc_sdf(1805.0, 640.0, 13.0)))
    glow = np.clip(1.0 - disc_sdf(1805.0, 640.0, 8.0) / 14.0, 0.0, 1.0) * (led > 0)
    cab = mix(cab, (26, 70, 44), glow * 0.7)
    cab = mix(cab, (52, 205, 112), coverage(led))
    cab = mix(cab, (190, 255, 215), coverage(disc_sdf(1803.0, 637.5, 2.6)) * 0.7)

    # ---- alpha: clear in the opening, a soft edge outside it, opaque everywhere else --------------------------------
    u = np.clip((d_win - 0.5) / SOFT, 0.0, 1.0)
    alpha = np.where(d_win <= 0.0, 0.0, CLEAR_BELOW + (255.0 - CLEAR_BELOW) * (1.0 - (1.0 - u) ** 2))
    alpha = np.where(d_win > SOFT + 0.5, 255.0, alpha)
    return np.clip(cab, 0, 255).round().astype(np.uint8), alpha


def to_palette(rgb, alpha):
    """A palette image and its per-entry alpha: the cabinet's colours, the bright details', the soft edge's alphas, one clear entry."""
    steps = 17  # the soft edge in steps of 17 (255 / 15): 14 palette entries, none below CLEAR_BELOW
    a = np.where((alpha > 0) & (alpha < 255), np.maximum(steps, np.round(alpha / steps) * steps), alpha).astype(np.uint8)
    soft = (a > 0) & (a < 255)
    clear = a == 0
    bright = (rgb.max(axis=-1) > BRIGHT_ABOVE) & ~soft & ~clear
    plain = rgb.copy()
    plain[soft | clear | bright] = LIP  # the quantiser sees the dark cabinet only
    pal = Image.fromarray(plain).quantize(colors=COLORS, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    idx = np.array(pal, dtype=np.uint8)
    colors = list(pal.getpalette()[: COLORS * 3])
    colors += [0] * (COLORS * 3 - len(colors))

    pixels = rgb[bright].reshape(-1, 1, 3)
    bpal = Image.fromarray(pixels).quantize(colors=BRIGHT, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    bcolors = list(bpal.getpalette()[: BRIGHT * 3])
    bcolors += [0] * (BRIGHT * 3 - len(bcolors))
    idx[bright] = np.array(bpal, dtype=np.uint8).reshape(-1) + COLORS
    colors += bcolors

    table = [255] * (COLORS + BRIGHT)
    for lv in range(steps, 255, steps):  # 17 .. 238
        colors += list(LIP)
        table.append(lv)
        idx[soft & (a == lv)] = len(table) - 1
    colors += [0, 0, 0]
    table.append(0)
    idx[clear] = len(table) - 1
    out = Image.fromarray(idx, mode="P")
    out.putpalette(colors)
    return out, bytes(table)


def check(path, out_size):
    """Reads the file back the way the app does and checks what the format needs."""
    img = Image.open(path)
    assert img.size == (W, H) and img.mode == "P", "expected a 1920x1080 palette PNG"
    assert "transparency" in img.info, "no tRNS chunk"
    a = np.array(img.convert("RGBA"))[..., 3]
    assert a[H // 2, W // 2] < CLEAR_BELOW, "the centre is not clear"
    ys, xs = np.nonzero(a < CLEAR_BELOW)
    box = (int(xs.min()), int(ys.min()), int(xs.max()) - int(xs.min()) + 1, int(ys.max()) - int(ys.min()) + 1)
    assert box == (WIN_X, WIN_Y, WIN_W, WIN_H), f"the opening is {box}, expected {(WIN_X, WIN_Y, WIN_W, WIN_H)}"
    assert out_size <= FILE_LIMIT, f"{out_size} bytes is over the {FILE_LIMIT} limit"
    soft = int(((a > 0) & (a < 255)).sum())
    return box, soft


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="frontend/assets/borders/default.png")
    ap.add_argument("--preview", help="also write this PNG: the border over a test picture, to look at the result")
    args = ap.parse_args()

    rgb, alpha = draw()
    img, table = to_palette(rgb, alpha)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    img.save(args.out, optimize=True, transparency=table)
    size = os.path.getsize(args.out)
    box, soft = check(args.out, size)
    print(f"{args.out}: {size} bytes, opening {box[0]},{box[1]} {box[2]}x{box[3]}, {soft} soft pixels")

    if args.preview:
        pic = np.zeros((H, W, 3), dtype=np.uint8)
        gx = ((X - WIN_X) / WIN_W).clip(0, 1)
        gy = ((Y - WIN_Y) / WIN_H).clip(0, 1)
        pic[..., 0] = 40 + 180 * gx
        pic[..., 1] = 60 + 140 * gy
        pic[..., 2] = 120 + 90 * (1 - gx)
        over = Image.open(args.out).convert("RGBA")
        base = Image.fromarray(pic).convert("RGBA")
        base.alpha_composite(over)
        base.convert("RGB").save(args.preview)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Draws the installer's wizard pictures into installer/images.

    python tools/installer/make_wizard_images.py

wizard-<pct>.png is the tall picture on the last Setup page, small-<pct>.png the icon in the
top corner of the other pages. One file per Windows display scale, so Setup never has to
stretch a picture. Needs Pillow; the only input is icon.png.
"""
import os

from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "installer", "images")
SCALES = (100, 125, 150, 175, 200, 250)

BG_TOP = (21, 24, 30)       # theme kSurface
BG_BOTTOM = (15, 17, 21)    # theme kBg
GRID = (42, 47, 58)         # theme kBorder
GOOD = (61, 220, 132)       # theme kGood
BAD = (255, 92, 92)         # theme kBad
ACCENT = (76, 141, 255)     # the icon's blue

# A short frame time trace: steady, one stutter, steady again.
TRACE = [0.50, 0.48, 0.52, 0.49, 0.51, 0.47, 0.50, 0.53, 0.49, 0.50, 0.12, 0.46,
         0.51, 0.49, 0.52, 0.48, 0.50, 0.51, 0.47, 0.50, 0.49, 0.52, 0.50]


def icon(size):
    return Image.open(os.path.join(ROOT, "icon.png")).convert("RGBA").resize((size, size), Image.LANCZOS)


def wizard(scale):
    ss = 4  # draw large, then shrink: smooth lines without an anti-aliasing library
    w, h = round(164 * scale / 100), round(314 * scale / 100)
    W, H = w * ss, h * ss
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    for y in range(H):
        t = y / (H - 1)
        d.line([(0, y), (W, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)))

    # Soft blue glow behind the icon.
    glow = Image.new("L", (W, H), 0)
    gd = ImageDraw.Draw(glow)
    cx, cy, r = W // 2, int(H * 0.36), int(W * 0.62)
    for i in range(r, 0, -ss * 2):
        gd.ellipse([cx - i, cy - i, cx + i, cy + i], fill=int(46 * (1 - i / r) ** 2))
    im.paste(Image.new("RGB", (W, H), ACCENT), (0, 0), glow)

    size = int(W * 0.5)
    ic = icon(size)
    im.paste(ic, (cx - size // 2, cy - size // 2), ic)

    # Frame time graph along the bottom, like the overlay's own.
    left, right = int(W * 0.12), int(W * 0.88)
    top, bottom = int(H * 0.70), int(H * 0.86)
    for gy in (top, (top + bottom) // 2, bottom):
        d.line([(left, gy), (right, gy)], fill=GRID, width=ss)
    pts = [(left + (right - left) * i / (len(TRACE) - 1), top + (bottom - top) * v) for i, v in enumerate(TRACE)]
    d.line(pts, fill=GOOD, width=int(ss * 2.2 * scale / 100), joint="curve")
    spike = pts[TRACE.index(min(TRACE))]
    d.line([(spike[0], top - ss * 3), (spike[0], bottom)], fill=BAD, width=ss)
    return im.resize((w, h), Image.LANCZOS)


def small(scale):
    s = round(58 * scale / 100)
    return icon(s * 4).resize((s, s), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    for scale in SCALES:
        wizard(scale).save(os.path.join(OUT, f"wizard-{scale}.png"), optimize=True)
        small(scale).save(os.path.join(OUT, f"small-{scale}.png"), optimize=True)
    print("wrote", OUT)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Builds the README images in docs/images from the app's own renderer.

    python tools/docs/make_images.py [path/to/FPSOverlay.exe]

The exe draws the settings pages and the overlay with sample data (--shots, --demo-frames).
This script paints a made-up dusk landscape behind the overlay, frames the settings
screenshots, and writes the animated demo. Needs Pillow and NumPy; fonts come from Windows.
"""
import glob
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "docs", "images")
FONTS = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size)


def run(exe, *args):
    subprocess.run([exe, *args], check=True, stdout=subprocess.DEVNULL)


# ── Scene ───────────────────────────────────────────────────────────────────

def gradient(t, stops):
    pos = np.array([p for p, _ in stops])
    cols = np.array([c for _, c in stops], dtype=float)
    return np.stack([np.interp(t, pos, cols[:, k]) for k in range(3)], -1)


def midpoint_ridge(n, rng, rough):
    size = 1
    while size < n:
        size *= 2
    pts = np.zeros(size + 1)
    pts[0], pts[-1] = rng.uniform(-0.4, 0.4, 2)
    step, scale = size, 1.0
    while step > 1:
        half = step // 2
        for i in range(half, size, step):
            pts[i] = (pts[i - half] + pts[i + half]) / 2 + rng.uniform(-1, 1) * scale
        scale *= rough
        step = half
    line = np.interp(np.linspace(0, size, n), np.arange(size + 1), pts)
    return (line - line.min()) / (np.ptp(line) + 1e-9)


def scene(W, H, sun=(0.70, 0.56), seed=11):
    """A stylized dusk landscape: gradient sky, stars, a low sun and hazy mountain layers."""
    rng = np.random.default_rng(seed)
    ys = (np.arange(H) / H)[:, None]
    xs = (np.arange(W) / W)[None, :]
    sky = gradient(np.repeat(ys, W, 1), [
        (0.00, (8, 10, 28)), (0.28, (24, 20, 62)), (0.46, (70, 36, 96)),
        (0.56, (150, 64, 98)), (0.64, (232, 122, 86)), (1.00, (250, 170, 110))])
    img = sky

    # Stars, fading out toward the horizon.
    stars = rng.random((H, W)) < 0.0009
    twinkle = rng.uniform(0.35, 1.0, (H, W)) * np.clip(1 - ys / 0.42, 0, 1)
    img = img + (stars * twinkle * 200)[..., None]

    # Sun with a wide glow.
    cx, cy = sun[0] * W, sun[1] * H
    d = np.sqrt((xs * W - cx) ** 2 + (ys * H - cy) ** 2)
    glow = np.exp(-(d / (0.30 * W)) ** 2)
    img = img + glow[..., None] * np.array([120, 70, 40]) * 0.9
    r = 0.055 * W
    disk = np.clip(r - d + 0.5, 0, 1)
    img = img * (1 - disk[..., None]) + disk[..., None] * np.array([255, 222, 160])

    # Mountain layers, far to near: base row, height, roughness, color, haze strength.
    layers = [
        (0.63, 0.13, 0.55, (126, 62, 104), 0.55),
        (0.70, 0.15, 0.52, (78, 40, 86), 0.45),
        (0.80, 0.14, 0.50, (40, 26, 60), 0.35),
        (0.93, 0.12, 0.48, (16, 13, 30), 0.20),
    ]
    rows = np.arange(H)[:, None]
    for base, amp, rough, col, haze in layers:
        line = midpoint_ridge(W, rng, rough)
        broad = 0.5 + 0.5 * np.sin(np.linspace(0, 2 * np.pi, W) * rng.uniform(0.6, 1.4) + rng.uniform(0, 6.28))
        top = (base - amp * (0.65 * line + 0.35 * broad)) * H
        depth = rows - top[None, :]
        cover = np.clip(depth + 0.5, 0, 1)                        # anti-aliased ridge edge
        fade = np.exp(-np.clip(depth, 0, None) / (0.05 * H))       # haze at the top of each layer
        horizon = gradient(np.clip(top / H, 0, 1)[None, :].repeat(H, 0), [
            (0.0, (150, 64, 98)), (0.6, (232, 122, 86)), (1.0, (232, 122, 86))])
        shade = np.array(col, dtype=float) * (1 - haze * fade[..., None]) + horizon * (haze * fade[..., None])
        img = img * (1 - cover[..., None]) + shade * cover[..., None]

    # Vignette.
    v = 1 - 0.35 * (((xs - 0.5) * 1.3) ** 2 + ((ys - 0.5) * 1.6) ** 2)
    img = img * np.clip(v, 0, 1)[..., None]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


# ── Helpers ─────────────────────────────────────────────────────────────────

def rounded_mask(size, radius, outline=0):
    """Anti-aliased rounded rectangle (drawn at 4x, then reduced). outline > 0 gives a ring."""
    k = 4
    m = Image.new("L", (size[0] * k, size[1] * k), 0)
    d = ImageDraw.Draw(m)
    if outline:
        d.rounded_rectangle((0, 0, size[0] * k - 1, size[1] * k - 1), radius * k, outline=255, width=outline * k)
    else:
        d.rounded_rectangle((0, 0, size[0] * k - 1, size[1] * k - 1), radius * k, fill=255)
    return m.resize(size, Image.LANCZOS)


def tint(size, color, mask):
    """A solid color layer shaped by mask, for alpha compositing (ImageDraw overwrites alpha)."""
    layer = Image.new("RGBA", size, color[:3] + (0,))
    a = mask.point(lambda v: v * color[3] // 255)
    layer.putalpha(a)
    return layer


def with_shadow(img, radius, pad, blur=26, offset=10, strength=150):
    """Rounded corners, a hairline border and a soft drop shadow on a transparent canvas."""
    w, h = img.size
    canvas = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    shadow = Image.new("L", canvas.size, 0)
    ImageDraw.Draw(shadow).rounded_rectangle((pad, pad + offset, pad + w, pad + h + offset), radius, fill=strength)
    shadow = shadow.filter(ImageFilter.GaussianBlur(blur))
    canvas.paste(Image.new("RGBA", canvas.size, (0, 0, 0, 255)), (0, 0), shadow)
    canvas.paste(img.convert("RGBA"), (pad, pad), rounded_mask((w, h), radius))
    canvas.alpha_composite(tint((w, h), (255, 255, 255, 34), rounded_mask((w, h), radius, outline=1)), (pad, pad))
    return canvas


def save(img, name, **kw):
    path = os.path.join(OUT, name)
    img.save(path, **kw)
    print(f"  {os.path.relpath(path, ROOT)}  {img.size[0]}x{img.size[1]}  {os.path.getsize(path) // 1024} KB")


def make_gif(background, frames, pos, name, ms=70):
    """GIF with one shared palette: 96 colors for the scene, 160 for the overlay.
    The scene is dithered once, so its pixels are the same in every frame and only the
    overlay area is stored per frame."""
    bg = background.convert("RGB")
    w, h = frames[0].size
    mosaic = Image.new("RGB", (w * 4, h))
    for i, f in enumerate(frames[:: max(1, len(frames) // 4)][:4]):
        tile = bg.crop((pos[0], pos[1], pos[0] + w, pos[1] + h)).convert("RGBA")
        tile.alpha_composite(f)
        mosaic.paste(tile.convert("RGB"), (i * w, 0))

    def colors(img, n, method):
        p = img.quantize(n, method=method, dither=Image.Dither.NONE).getpalette()
        return (p + [0] * (3 * n))[:3 * n]

    # Max coverage keeps small, saturated areas (the green FPS number) exact; median cut
    # suits the smooth sky.
    palette = Image.new("P", (1, 1))
    palette.putpalette(colors(bg, 96, Image.Quantize.MEDIANCUT) + colors(mosaic, 160, Image.Quantize.MAXCOVERAGE))
    base = bg.quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG).convert("RGBA")
    out = []
    for f in frames:
        im = base.copy()
        im.alpha_composite(f, pos)
        out.append(im.convert("RGB").quantize(palette=palette, dither=Image.Dither.NONE))
    path = os.path.join(OUT, name)
    out[0].save(path, save_all=True, append_images=out[1:], duration=ms, loop=0, optimize=False)
    print(f"  {os.path.relpath(path, ROOT)}  {out[0].size[0]}x{out[0].size[1]}  {len(out)} frames  "
          f"{os.path.getsize(path) // 1024} KB")


def load_frames(folder):
    return [Image.open(p).convert("RGBA") for p in sorted(glob.glob(os.path.join(folder, "frame*.png")))]


# ── Images ──────────────────────────────────────────────────────────────────

def banner(hud, icon):
    W, H = 1280, 640
    img = scene(W, H, sun=(0.80, 0.60), seed=5).convert("RGBA")
    # Darken the left side for the text.
    xs = np.linspace(0, 1, W)[None, :].repeat(H, 0)
    shade = (np.clip(1.0 - xs / 0.62, 0, 1) ** 1.3 * 225).astype(np.uint8)
    img.alpha_composite(Image.merge("RGBA", [Image.new("L", (W, H), 9), Image.new("L", (W, H), 10),
                                             Image.new("L", (W, H), 22), Image.fromarray(shade, "L")]))
    d = ImageDraw.Draw(img)
    x = 84
    img.alpha_composite(icon.resize((104, 104), Image.LANCZOS), (x, 112))
    d.text((x - 4, 226), "FPS Overlay", font=font("segoeuib.ttf", 92), fill=(255, 255, 255))
    d.text((x, 350), "Accurate FPS, frame times and hardware stats", font=font("segoeui.ttf", 31), fill=(214, 214, 236))
    d.text((x, 392), "for every Windows game.", font=font("segoeui.ttf", 31), fill=(214, 214, 236))
    chips = [["DirectX 9\u201312 \u00b7 Vulkan \u00b7 OpenGL", "1% and 0.1% lows"],
             ["No injection", "About 1% CPU", "C++20 \u00b7 Dear ImGui"]]
    f = font("seguisb.ttf", 21)
    y = 470
    for row in chips:
        cx = x
        for text in row:
            size = (int(d.textlength(text, font=f)) + 32, 42)
            img.alpha_composite(tint(size, (255, 255, 255, 24), rounded_mask(size, 21)), (cx, y))
            img.alpha_composite(tint(size, (255, 255, 255, 50), rounded_mask(size, 21, outline=1)), (cx, y))
            d.text((cx + 16, y + 7), text, font=f, fill=(236, 236, 250))
            cx += size[0] + 12
        y += 56
    # The HUD image already has its own rounded, semi-transparent panel; give it a soft shadow.
    pad = 40
    shadow = Image.new("L", (hud.size[0] + 2 * pad, hud.size[1] + 2 * pad), 0)
    shadow.paste(hud.split()[3].point(lambda a: 170 if a > 8 else 0), (pad, pad + 12))
    shadow = shadow.filter(ImageFilter.GaussianBlur(22))
    hx, hy = W - hud.size[0] - 70, (H - hud.size[1]) // 2
    img.paste(Image.new("RGBA", shadow.size, (0, 0, 0, 255)), (hx - pad, hy - pad), shadow)
    img.alpha_composite(hud, (hx, hy))
    save(img.convert("RGB"), "banner.png", optimize=True)


def layout_card(hud, size, pos, seed, sun, name):
    bg = scene(*size, sun=sun, seed=seed).convert("RGBA")
    bg.alpha_composite(hud, pos)
    save(with_shadow(bg, 14, 28, blur=20, offset=8, strength=120), name, optimize=True)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "Release-user", "FPSOverlay.exe")
    if not os.path.isfile(exe):
        sys.exit(f"exe not found: {exe}")
    os.makedirs(OUT, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="fpso-docs-")
    print("rendering with", exe)
    run(exe, "--shots", os.path.join(tmp, "shots"), "--scale", "1.5")
    run(exe, "--demo-frames", os.path.join(tmp, "vertical"), "--count", "90", "--scale", "130", "--accent", "1")
    run(exe, "--demo-frames", os.path.join(tmp, "full"), "--count", "1", "--scale", "150", "--accent", "1", "--all")
    run(exe, "--demo-frames", os.path.join(tmp, "full-small"), "--count", "1", "--scale", "125", "--accent", "1", "--all")
    run(exe, "--demo-frames", os.path.join(tmp, "horizontal"), "--count", "1", "--layout", "horizontal", "--scale", "115")
    run(exe, "--demo-frames", os.path.join(tmp, "bar"), "--count", "1", "--layout", "bar", "--scale", "115", "--accent", "4")

    print("writing", os.path.relpath(OUT, ROOT))
    icon = Image.open(os.path.join(ROOT, "icon.png")).convert("RGBA")
    banner(load_frames(os.path.join(tmp, "full"))[0], icon)

    frames = load_frames(os.path.join(tmp, "vertical"))
    make_gif(scene(1040, 585, sun=(0.72, 0.56), seed=11), frames, (10, 10), "demo.gif")

    vertical = load_frames(os.path.join(tmp, "full-small"))[0]
    layout_card(vertical, (vertical.size[0] + 260, vertical.size[1] + 40), (12, 12), 3, (0.86, 0.66),
                "layout-vertical.png")
    for name, seed in (("horizontal", 21), ("bar", 8)):
        hud = load_frames(os.path.join(tmp, name))[0]
        W = hud.size[0] + 40
        layout_card(hud, (W, hud.size[1] + 150), ((W - hud.size[0]) // 2, 6), seed, (0.55, 0.92),
                    f"layout-{name}.png")

    for page in ("overlay", "appearance", "sensors", "hotkeys", "general", "about", "welcome"):
        shot = Image.open(os.path.join(tmp, "shots", f"settings-{page}.png")).convert("RGBA")
        save(with_shadow(shot, 12, 40), f"settings-{page}.png", optimize=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generates the LiveArea images in sce_sys/ (VitaCast) and mirror/sce_sys/
(VitaCast Mirror). Requires Pillow.

The Vita only accepts 8-bit palettised PNGs for these files, so every image
is quantised before saving.
"""

import math
import os

from PIL import Image, ImageDraw, ImageFont

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BG_TOP = (24, 36, 64)
BG_BOTTOM = (10, 12, 18)
RECEIVER = {"dir": "sce_sys", "accent": (92, 156, 255), "title": "VitaCast",
            "subtitle": "Cast receiver for PS Vita"}
MIRROR = {"dir": "mirror/sce_sys", "accent": (64, 200, 180), "title": "VitaCast Mirror",
          "subtitle": "Show your Vita on a TV"}
WHITE = (240, 242, 245)


def gradient(w, h):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(BG_TOP[i] * (1 - t) + BG_BOTTOM[i] * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def cast_glyph(draw, cx, cy, size, color):
    """A screen outline with broadcast arcs rising from its lower-left corner."""
    w, h = size, size * 0.72
    x0, y0 = cx - w / 2, cy - h / 2
    t = max(2, int(size * 0.075))
    # top, right, bottom-right edges and a short left stub
    draw.rectangle([x0, y0, x0 + w, y0 + t], fill=color)
    draw.rectangle([x0 + w - t, y0, x0 + w, y0 + h], fill=color)
    draw.rectangle([x0 + w * 0.45, y0 + h - t, x0 + w, y0 + h], fill=color)
    draw.rectangle([x0, y0, x0 + t, y0 + h * 0.35], fill=color)
    ox, oy = x0, y0 + h
    for ring in range(3):
        r = h * (0.22 + 0.24 * ring)
        draw.arc([ox - r, oy - r, ox + r, oy + r], start=270, end=360, fill=color, width=t)
    d = t * 1.1
    draw.ellipse([ox - d * 0.2, oy - d * 1.8, ox + d * 1.6, oy], fill=color)


def font(size):
    for name in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        "/Library/Fonts/Arial Bold.ttf",
    ):
        if os.path.exists(name):
            return ImageFont.truetype(name, size)
    return ImageFont.load_default()


def save(img, app, rel):
    path = os.path.join(REPO, app["dir"], rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.convert("RGB").quantize(colors=255, method=Image.Quantize.MEDIANCUT).save(path, optimize=True)
    print("wrote", os.path.relpath(path))


def icon(app):
    s = 128
    img = gradient(s, s)
    d = ImageDraw.Draw(img)
    cast_glyph(d, s / 2, s / 2 + 2, 78, app["accent"])
    save(img, app, "icon0.png")


def bg(app):
    w, h = 840, 500
    accent = app["accent"]
    img = gradient(w, h)
    d = ImageDraw.Draw(img)
    # soft rings
    for i in range(6):
        r = 160 + i * 70
        col = tuple(int(c * 0.35 + b * 0.65) for c, b in zip(accent, BG_BOTTOM))
        d.arc([w * 0.78 - r, h * 0.95 - r, w * 0.78 + r, h * 0.95 + r], 180, 300, fill=col, width=2)
    cast_glyph(d, 210, 240, 180, accent)
    title_font = font(64 if len(app["title"]) < 10 else 50)
    d.text((360, 190), app["title"], font=title_font, fill=WHITE)
    d.text((364, 272), app["subtitle"], font=font(26), fill=(170, 180, 196))
    save(img, app, "livearea/contents/bg.png")


def startup(app):
    w, h = 280, 158
    img = gradient(w, h)
    d = ImageDraw.Draw(img)
    cast_glyph(d, 70, h / 2, 70, app["accent"])
    d.text((120, h / 2 - 22), "Start", font=font(34), fill=WHITE)
    save(img, app, "livearea/contents/startup.png")


if __name__ == "__main__":
    for app in (RECEIVER, MIRROR):
        icon(app)
        bg(app)
        startup(app)

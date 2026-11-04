#!/usr/bin/env python3
"""App icons for every lgp app from one own motif (no game artwork, no marks of others).

The motif is defined once below (a ring cut by a lightning bolt on a blue
gradient, plus the letters "LGP" for wide images) and written as

  platform/icon/lgp-icon.svg                 vector form (rounded square)
  platform/icon/lgp-icon-128.png             SDL window icon (macOS Dock, Linux)
  platform/icon/lgp-icon-180.png             browser page (apple-touch-icon)
  platform/ios/Assets.xcassets/              iOS/iPadOS: one 1024 px icon
  platform/tvos/Assets.xcassets/             tvOS: layered icon (back/middle/front),
                                             App Store icon, Top Shelf images
  platform/macos/lgp.icns                    macOS app bundle
  platform/windows/lgp.ico                   Windows exe (lgp.rc)
  platform/android/res/drawable/*.xml        adaptive icon layers and TV banner
  platform/android/res/mipmap-anydpi-v26/    adaptive icon

  gen_icons.py            write everything (needs numpy and Pillow)
  gen_icons.py --check    verify the files in the repository: text files
                          (SVG, Android XML, Contents.json) byte for byte,
                          images by type, size and opacity (plain Python)

The generated files are committed, so builds need neither numpy nor Pillow.
"""
import argparse
import json
import math
import os
import struct
import sys
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

# --- Motif -----------------------------------------------------------------
# Colours (sRGB).
BG_TOP = (0x24, 0x40, 0x7E)
BG_BOTTOM = (0x0D, 0x16, 0x30)
RING = (0x2E, 0xC4, 0xB6)
BOLT = (0xFF, 0xC8, 0x3D)
WORD = (0xF4, 0xF1, 0xE8)

# Mark in a unit box (x right, y down): a ring and a bolt crossing it. The
# ring is cut where the bolt passes (GAP around the bolt shows the background).
RING_CENTER = (0.5, 0.5)
RING_RADIUS = 0.30
RING_WIDTH = 0.075
BOLT_POINTS = [(0.62, 0.10), (0.33, 0.55), (0.49, 0.55), (0.38, 0.90),
               (0.69, 0.44), (0.53, 0.44), (0.72, 0.10)]
GAP = 0.028

# Letters "LGP" in units of the letter height (x right, y down).
STROKE = 0.22
LETTER_SPACE = 0.22


def _letters():
    """Shapes of "LGP" with height 1: ("rect", x0, y0, x1, y1) or
    ("sector", cx, cy, r_in, r_out, a0, a1) (degrees, clockwise from +x)."""
    w = STROKE
    shapes = []
    x = 0.0
    # L
    shapes += [("rect", x, 0.0, x + w, 1.0), ("rect", x, 1.0 - w, x + 0.62, 1.0)]
    x += 0.62 + LETTER_SPACE
    # G: an open ring (gap at the upper right) and its crossbar.
    shapes += [("sector", x + 0.5, 0.5, 0.5 - w, 0.5, 0.0, 310.0),
               ("rect", x + 0.52, 0.5, x + 1.0 - w + 0.02, 0.5 + w)]
    x += 1.0 + LETTER_SPACE
    # P: stem, two bars and the right half of a ring.
    hb, xb = 0.62, 0.38
    shapes += [("rect", x, 0.0, x + w, 1.0), ("rect", x, 0.0, x + xb, w),
               ("rect", x, hb - w, x + xb, hb),
               ("sector", x + xb, hb / 2, hb / 2 - w, hb / 2, -90.0, 90.0)]
    x += xb + hb / 2
    return shapes, x


LETTERS, WORD_WIDTH = _letters()


class Layout:
    """Where the mark (box at mx, my, size ms) and the letters (at wx, wy,
    height wh; wh 0 = none) sit on a W x H image, and the background:
    bg "full" (opaque), "round" (rounded square, transparent corners, inset
    by margin) or None."""

    def __init__(self, w, h, mx, my, ms, wx=0.0, wy=0.0, wh=0.0, bg="full", margin=0.0):
        self.w, self.h, self.mx, self.my, self.ms = w, h, mx, my, ms
        self.wx, self.wy, self.wh, self.bg, self.margin = wx, wy, wh, bg, margin

    def bg_rect(self):
        m = self.margin
        return (m, m, self.w - m, self.h - m)

    def gradient_span(self):
        x0, y0, x1, y1 = self.bg_rect()
        return y0, y1


def square(size, bg="full", margin_frac=0.0, mark_frac=0.80):
    m = size * margin_frac
    ms = (size - 2 * m) * mark_frac
    return Layout(size, size, (size - ms) / 2, (size - ms) / 2, ms, bg=bg, margin=m)


def wide(w, h, mark_frac, word_frac, space_frac, bg="full"):
    ms, wh, sp = h * mark_frac, h * word_frac, h * space_frac
    total = ms + sp + WORD_WIDTH * wh
    x = (w - total) / 2
    return Layout(w, h, x, (h - ms) / 2, ms, x + ms + sp, (h - wh) / 2, wh, bg=bg)


def mark_only(w, h, mark_frac, bg="full"):
    ms = h * mark_frac
    return Layout(w, h, (w - ms) / 2, (h - ms) / 2, ms, bg=bg)


ROUND_RADIUS = 0.225  # corner radius of the rounded square, of its side


def corner_radius(lay):
    x0, y0, x1, y1 = lay.bg_rect()
    return ROUND_RADIUS * (x1 - x0)


def _sector_points(cx, cy, r, a0, a1):
    p0 = (cx + r * math.cos(math.radians(a0)), cy + r * math.sin(math.radians(a0)))
    p1 = (cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1)))
    return p0, p1


# --- Vector output (SVG, Android VectorDrawable) ----------------------------

def _f(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def _hex(c):
    return "#%02X%02X%02X" % c


def _mark_paths(lay):
    """Path data (absolute, y down) of ring and bolt for a layout."""
    s, ox, oy = lay.ms, lay.mx, lay.my
    cx, cy = ox + RING_CENTER[0] * s, oy + RING_CENTER[1] * s
    ro, ri = (RING_RADIUS + RING_WIDTH / 2) * s, (RING_RADIUS - RING_WIDTH / 2) * s
    ring = (f"M{_f(cx)},{_f(cy - ro)}a{_f(ro)},{_f(ro)} 0 1 1 0,{_f(2 * ro)}"
            f"a{_f(ro)},{_f(ro)} 0 1 1 0,{_f(-2 * ro)}z"
            f"M{_f(cx)},{_f(cy - ri)}a{_f(ri)},{_f(ri)} 0 1 0 0,{_f(2 * ri)}"
            f"a{_f(ri)},{_f(ri)} 0 1 0 0,{_f(-2 * ri)}z")
    pts = [(ox + x * s, oy + y * s) for x, y in BOLT_POINTS]
    bolt = "M" + "L".join(f"{_f(x)},{_f(y)}" for x, y in pts) + "z"
    return ring, bolt, GAP * s


def _word_path(lay):
    h, ox, oy = lay.wh, lay.wx, lay.wy
    parts = []
    for sh in LETTERS:
        if sh[0] == "rect":
            _, x0, y0, x1, y1 = sh
            parts.append(f"M{_f(ox + x0 * h)},{_f(oy + y0 * h)}H{_f(ox + x1 * h)}"
                         f"V{_f(oy + y1 * h)}H{_f(ox + x0 * h)}z")
        else:
            _, cx, cy, ri, ro, a0, a1 = sh
            cx, cy, ri, ro = ox + cx * h, oy + cy * h, ri * h, ro * h
            large = 1 if a1 - a0 > 180 else 0
            (o0, o1), (i0, i1) = _sector_points(cx, cy, ro, a0, a1), _sector_points(cx, cy, ri, a0, a1)
            parts.append(f"M{_f(o0[0])},{_f(o0[1])}A{_f(ro)},{_f(ro)} 0 {large} 1 {_f(o1[0])},{_f(o1[1])}"
                         f"L{_f(i1[0])},{_f(i1[1])}A{_f(ri)},{_f(ri)} 0 {large} 0 {_f(i0[0])},{_f(i0[1])}z")
    return "".join(parts)


def svg(lay):
    ring, bolt, gap = _mark_paths(lay)
    y0, y1 = lay.gradient_span()
    x0, by0, x1, by1 = lay.bg_rect()
    r = corner_radius(lay) if lay.bg == "round" else 0
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{lay.w}" height="{lay.h}" viewBox="0 0 {lay.w} {lay.h}">',
           "<!-- lgp app icon: generated by tools/icon/gen_icons.py, edit the motif there. -->",
           "<defs>",
           f'<linearGradient id="bg" gradientUnits="userSpaceOnUse" x1="0" y1="{_f(y0)}" x2="0" y2="{_f(y1)}">'
           f'<stop offset="0" stop-color="{_hex(BG_TOP)}"/><stop offset="1" stop-color="{_hex(BG_BOTTOM)}"/>'
           "</linearGradient>",
           "</defs>",
           f'<rect x="{_f(x0)}" y="{_f(by0)}" width="{_f(x1 - x0)}" height="{_f(by1 - by0)}" rx="{_f(r)}" fill="url(#bg)"/>',
           f'<path fill="{_hex(RING)}" fill-rule="evenodd" d="{ring}"/>',
           f'<path fill="url(#bg)" stroke="url(#bg)" stroke-width="{_f(2 * gap)}" stroke-linejoin="round" d="{bolt}"/>',
           f'<path fill="{_hex(BOLT)}" d="{bolt}"/>']
    if lay.wh:
        out.append(f'<path fill="{_hex(WORD)}" d="{_word_path(lay)}"/>')
    out.append("</svg>")
    return "\n".join(out) + "\n"


ANDROID_HEAD = ('<?xml version="1.0" encoding="utf-8"?>\n'
                "<!-- Generated by tools/icon/gen_icons.py (lgp app icon); edit the motif there. -->\n")


def _vd_gradient(lay, attr):
    y0, y1 = lay.gradient_span()
    return (f'        <aapt:attr name="{attr}">\n'
            f'            <gradient android:type="linear" android:startX="0" android:startY="{_f(y0)}"\n'
            f'                android:endX="0" android:endY="{_f(y1)}"\n'
            f'                android:startColor="#FF{_hex(BG_TOP)[1:]}" android:endColor="#FF{_hex(BG_BOTTOM)[1:]}" />\n'
            f"        </aapt:attr>\n")


def vector_drawable(lay, dp_w, dp_h, background=True, mark=True):
    ring, bolt, gap = _mark_paths(lay)
    out = [ANDROID_HEAD,
           '<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
           '    xmlns:aapt="http://schemas.android.com/aapt"\n'
           f'    android:width="{dp_w}dp"\n    android:height="{dp_h}dp"\n'
           f'    android:viewportWidth="{lay.w}"\n    android:viewportHeight="{lay.h}">\n']
    if background:
        out.append(f'    <path android:pathData="M0,0h{lay.w}v{lay.h}h-{lay.w}z">\n'
                   + _vd_gradient(lay, "android:fillColor") + "    </path>\n")
    if mark:
        out.append(f'    <path android:fillColor="#FF{_hex(RING)[1:]}" android:fillType="evenOdd"\n'
                   f'        android:pathData="{ring}" />\n')
        if background:
            out.append(f'    <path android:strokeWidth="{_f(2 * gap)}" android:strokeLineJoin="round"\n'
                       f'        android:pathData="{bolt}">\n'
                       + _vd_gradient(lay, "android:fillColor") + _vd_gradient(lay, "android:strokeColor")
                       + "    </path>\n")
        out.append(f'    <path android:fillColor="#FF{_hex(BOLT)[1:]}"\n        android:pathData="{bolt}" />\n')
    if lay.wh:
        out.append(f'    <path android:fillColor="#FF{_hex(WORD)[1:]}"\n'
                   f'        android:pathData="{_word_path(lay)}" />\n')
    out.append("</vector>\n")
    return "".join(out)


def adaptive_icon():
    return (ANDROID_HEAD +
            '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
            '    <background android:drawable="@drawable/ic_launcher_background" />\n'
            '    <foreground android:drawable="@drawable/ic_launcher_foreground" />\n'
            "</adaptive-icon>\n")


# Android adaptive icon: 108 dp layers, the launcher shows at least the
# centre circle of 66 dp. The bolt's far corner is 0.456 of the mark box from
# its centre, so a 70 dp box keeps it inside (32 dp).
ANDROID_FG = Layout(108, 108, 19, 19, 70, bg=None)
ANDROID_BG = Layout(108, 108, 19, 19, 70, bg="full")
# Android TV banner: 320 x 180 dp.
ANDROID_BANNER = wide(320, 180, 0.70, 0.30, 0.08)


# --- Raster output ----------------------------------------------------------

def _np():
    import numpy as np  # noqa: PLC0415
    return np


class Canvas:
    """Premultiplied RGBA in float, anti-aliased from signed distances in pixels."""

    def __init__(self, w, h):
        np = _np()
        self.w, self.h = w, h
        self.px = np.zeros((h, w, 4))

    def _grid(self, box):
        np = _np()
        x0, y0, x1, y1 = box
        x0, y0 = max(int(math.floor(x0)) - 2, 0), max(int(math.floor(y0)) - 2, 0)
        x1, y1 = min(int(math.ceil(x1)) + 2, self.w), min(int(math.ceil(y1)) + 2, self.h)
        if x0 >= x1 or y0 >= y1:
            return None
        ys, xs = np.mgrid[y0:y1, x0:x1].astype(np.float64)
        return (slice(y0, y1), slice(x0, x1)), xs + 0.5, ys + 0.5

    def fill(self, shape, paint):
        np = _np()
        g = self._grid(shape.bbox())
        if g is None:
            return
        sl, xs, ys = g
        a = np.clip(0.5 - shape.sdf(xs, ys), 0.0, 1.0)[..., None]
        col = paint(xs, ys)
        src = np.concatenate([col * a, a], axis=-1)
        self.px[sl] = src + self.px[sl] * (1.0 - a)

    def erase(self, shape):
        np = _np()
        g = self._grid(shape.bbox())
        if g is None:
            return
        sl, xs, ys = g
        a = np.clip(0.5 - shape.sdf(xs, ys), 0.0, 1.0)[..., None]
        self.px[sl] = self.px[sl] * (1.0 - a)

    def image(self, opaque):
        from PIL import Image  # noqa: PLC0415
        np = _np()
        rgba = self.px.copy()
        alpha = rgba[..., 3:4]
        rgb = np.where(alpha > 0, rgba[..., :3] / np.maximum(alpha, 1e-12), 0.0)
        if opaque:
            arr = np.round(np.clip(rgb, 0, 1) * 255).astype(np.uint8)
            return Image.fromarray(arr, "RGB")
        arr = np.round(np.clip(np.concatenate([rgb, alpha], axis=-1), 0, 1) * 255).astype(np.uint8)
        return Image.fromarray(arr, "RGBA")


def solid(c):
    def paint(xs, ys):
        np = _np()
        return np.broadcast_to(np.array(c, dtype=np.float64) / 255.0, xs.shape + (3,))
    return paint


def vgradient(y0, y1):
    def paint(xs, ys):
        np = _np()
        t = np.clip((ys - y0) / (y1 - y0), 0.0, 1.0)[..., None]
        c0 = np.array(BG_TOP, dtype=np.float64) / 255.0
        c1 = np.array(BG_BOTTOM, dtype=np.float64) / 255.0
        return c0 + (c1 - c0) * t
    return paint


class RoundRect:
    def __init__(self, x0, y0, x1, y1, r):
        self.b, self.r = (x0, y0, x1, y1), r

    def bbox(self):
        return self.b

    def sdf(self, xs, ys):
        np = _np()
        x0, y0, x1, y1 = self.b
        cx, cy, hx, hy = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2 - self.r, (y1 - y0) / 2 - self.r
        qx, qy = np.abs(xs - cx) - hx, np.abs(ys - cy) - hy
        out = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0))
        return out + np.minimum(np.maximum(qx, qy), 0) - self.r


class Ring:
    def __init__(self, cx, cy, r, w):
        self.cx, self.cy, self.r, self.hw = cx, cy, r, w / 2

    def bbox(self):
        e = self.r + self.hw
        return (self.cx - e, self.cy - e, self.cx + e, self.cy + e)

    def sdf(self, xs, ys):
        np = _np()
        return np.abs(np.hypot(xs - self.cx, ys - self.cy) - self.r) - self.hw


class Sector:
    """Part of a ring between angles a0..a1 (degrees, clockwise from +x)."""

    def __init__(self, cx, cy, ri, ro, a0, a1):
        self.cx, self.cy, self.ri, self.ro, self.a0, self.a1 = cx, cy, ri, ro, a0, a1

    def bbox(self):
        return (self.cx - self.ro, self.cy - self.ro, self.cx + self.ro, self.cy + self.ro)

    def sdf(self, xs, ys):
        np = _np()
        dx, dy = xs - self.cx, ys - self.cy
        rc, hw = (self.ri + self.ro) / 2, (self.ro - self.ri) / 2
        d_ring = np.abs(np.hypot(dx, dy) - rc) - hw
        ang = (np.degrees(np.arctan2(dy, dx)) - self.a0) % 360.0
        inside = ang <= (self.a1 - self.a0)
        d_end = None
        for a in (self.a0, self.a1):
            ux, uy = math.cos(math.radians(a)), math.sin(math.radians(a))
            t = np.clip(dx * ux + dy * uy, self.ri, self.ro)
            d = np.hypot(dx - t * ux, dy - t * uy)
            d_end = d if d_end is None else np.minimum(d_end, d)
        return np.where(inside, d_ring, d_end)


class Polygon:
    def __init__(self, pts, grow=0.0):
        self.pts, self.grow = pts, grow

    def bbox(self):
        xs = [p[0] for p in self.pts]
        ys = [p[1] for p in self.pts]
        g = self.grow
        return (min(xs) - g, min(ys) - g, max(xs) + g, max(ys) + g)

    def sdf(self, xs, ys):
        np = _np()
        v = self.pts
        d = (xs - v[0][0]) ** 2 + (ys - v[0][1]) ** 2
        s = np.ones_like(xs)
        n = len(v)
        for i in range(n):
            j = i - 1
            ex, ey = v[j][0] - v[i][0], v[j][1] - v[i][1]
            wx, wy = xs - v[i][0], ys - v[i][1]
            t = np.clip((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0, 1.0)
            bx, by = wx - ex * t, wy - ey * t
            d = np.minimum(d, bx * bx + by * by)
            c1, c2, c3 = ys >= v[i][1], ys < v[j][1], ex * wy > ey * wx
            flip = (c1 & c2 & c3) | (~c1 & ~c2 & ~c3)
            s = np.where(flip, -s, s)
        return s * np.sqrt(d) - self.grow


def _mark_shapes(lay):
    s, ox, oy = lay.ms, lay.mx, lay.my
    ring = Ring(ox + RING_CENTER[0] * s, oy + RING_CENTER[1] * s, RING_RADIUS * s, RING_WIDTH * s)
    pts = [(ox + x * s, oy + y * s) for x, y in BOLT_POINTS]
    return ring, Polygon(pts), Polygon(pts, GAP * s)


def _word_shapes(lay):
    h, ox, oy = lay.wh, lay.wx, lay.wy
    out = []
    for sh in LETTERS:
        if sh[0] == "rect":
            _, x0, y0, x1, y1 = sh
            out.append(RoundRect(ox + x0 * h, oy + y0 * h, ox + x1 * h, oy + y1 * h, 0.0))
        else:
            _, cx, cy, ri, ro, a0, a1 = sh
            out.append(Sector(ox + cx * h, oy + cy * h, ri * h, ro * h, a0, a1))
    return out


def render(lay, layers=("bg", "ring", "bolt", "word")):
    """Draw the chosen layers; the ring is cut around the bolt either with the
    background (bg drawn) or transparent (ring alone, tvOS middle layer)."""
    c = Canvas(lay.w, lay.h)
    y0, y1 = lay.gradient_span()
    bg_paint = vgradient(y0, y1)
    ring, bolt, gap = _mark_shapes(lay)
    if "bg" in layers and lay.bg:
        x0, by0, x1, by1 = lay.bg_rect()
        r = corner_radius(lay) if lay.bg == "round" else 0.0
        c.fill(RoundRect(x0, by0, x1, by1, r), bg_paint)
    if "ring" in layers:
        c.fill(ring, solid(RING))
        if "bg" in layers and lay.bg:
            c.fill(gap, bg_paint)
        else:
            c.erase(gap)
    if "bolt" in layers:
        c.fill(bolt, solid(BOLT))
    if "word" in layers and lay.wh:
        for sh in _word_shapes(lay):
            c.fill(sh, solid(WORD))
    return c


# --- Outputs ----------------------------------------------------------------

XC_INFO = {"author": "xcode", "version": 1}


def _json(obj):
    return json.dumps(obj, indent=2, sort_keys=False) + "\n"


TV_STACKS = {
    # name: (role size, [(scale, w, h)], idiom)
    "AppIcon.imagestack": ("400x240", [("1x", 400, 240), ("2x", 800, 480)], "tv"),
    "AppIconAppStore.imagestack": ("1280x768", [("1x", 1280, 768)], "tv-marketing"),
}
TV_LAYERS = [("Front", ("bolt",)), ("Middle", ("ring",)), ("Back", ("bg",))]
TV_SHELF = {
    "TopShelf.imageset": ("top-shelf-image", "1920x720", [("1x", 1920, 720), ("2x", 3840, 1440)]),
    "TopShelfWide.imageset": ("top-shelf-image-wide", "2320x720", [("1x", 2320, 720), ("2x", 4640, 1440)]),
}
TV_BRAND = "platform/tvos/Assets.xcassets/AppIcon.brandassets"
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
ICNS_SIZES = [32, 64, 128, 256, 512, 1024]


def outputs():
    """Every generated file: path -> ("text", content) or
    ("png", layout, layers, opaque) or ("ico"|"icns", sizes)."""
    out = {}
    out["platform/icon/lgp-icon.svg"] = ("text", svg(square(1024, "round", 0.03)))
    out["platform/icon/lgp-icon-128.png"] = ("png", square(128, "round", 0.03), None, False)
    out["platform/icon/lgp-icon-180.png"] = ("png", square(180), None, True)
    # iOS/iPadOS: one 1024 px icon (Xcode 14+ makes the other sizes), opaque.
    ios = "platform/ios/Assets.xcassets"
    out[f"{ios}/Contents.json"] = ("text", _json({"info": XC_INFO}))
    out[f"{ios}/AppIcon.appiconset/Contents.json"] = ("text", _json({
        "images": [{"filename": "AppIcon-1024.png", "idiom": "universal", "platform": "ios",
                    "size": "1024x1024"}],
        "info": XC_INFO}))
    out[f"{ios}/AppIcon.appiconset/AppIcon-1024.png"] = ("png", square(1024), None, True)
    # tvOS: brand assets with two layered icons (parallax: back opaque, middle
    # and front transparent) and the Top Shelf images.
    tv = "platform/tvos/Assets.xcassets"
    out[f"{tv}/Contents.json"] = ("text", _json({"info": XC_INFO}))
    assets = []
    for stack, (size, scales, idiom) in TV_STACKS.items():
        assets.append({"filename": stack, "idiom": "tv", "role": "primary-app-icon", "size": size})
        base = f"{TV_BRAND}/{stack}"
        out[f"{base}/Contents.json"] = ("text", _json({
            "info": XC_INFO, "layers": [{"filename": f"{n}.imagestacklayer"} for n, _ in TV_LAYERS]}))
        for name, layers in TV_LAYERS:
            lbase = f"{base}/{name}.imagestacklayer"
            out[f"{lbase}/Contents.json"] = ("text", _json({"info": XC_INFO}))
            images = []
            for scale, w, h in scales:
                fn = f"{name.lower()}-{w}x{h}.png"
                images.append({"filename": fn, "idiom": idiom, "scale": scale})
                out[f"{lbase}/Content.imageset/{fn}"] = ("png", mark_only(w, h, 0.86), layers, name == "Back")
            out[f"{lbase}/Content.imageset/Contents.json"] = ("text", _json({"images": images, "info": XC_INFO}))
    for shelf, (role, size, scales) in TV_SHELF.items():
        assets.append({"filename": shelf, "idiom": "tv", "role": role, "size": size})
        images = []
        for scale, w, h in scales:
            fn = f"top-shelf-{w}x{h}.png"
            images.append({"filename": fn, "idiom": "tv", "scale": scale})
            out[f"{TV_BRAND}/{shelf}/{fn}"] = ("png", wide(w, h, 0.62, 0.30, 0.10), None, True)
        out[f"{TV_BRAND}/{shelf}/Contents.json"] = ("text", _json({"images": images, "info": XC_INFO}))
    out[f"{TV_BRAND}/Contents.json"] = ("text", _json({"assets": assets, "info": XC_INFO}))
    # macOS: Apple's grid, the body 824 of 1024 px.
    out["platform/macos/lgp.icns"] = ("icns", ICNS_SIZES)
    out["platform/windows/lgp.ico"] = ("ico", ICO_SIZES)
    # Android: adaptive icon (vector layers) and the TV banner.
    res = "platform/android/res"
    out[f"{res}/drawable/ic_launcher_background.xml"] = ("text", vector_drawable(ANDROID_BG, 108, 108, mark=False))
    out[f"{res}/drawable/ic_launcher_foreground.xml"] = ("text", vector_drawable(ANDROID_FG, 108, 108,
                                                                                background=False))
    out[f"{res}/mipmap-anydpi-v26/ic_launcher.xml"] = ("text", adaptive_icon())
    out[f"{res}/mipmap-anydpi-v26/ic_launcher_round.xml"] = ("text", adaptive_icon())
    out[f"{res}/drawable/banner.xml"] = ("text", vector_drawable(ANDROID_BANNER, 320, 180))
    return out


def macos_layout(size):
    return square(size, "round", 100 / 1024)


def windows_layout(size):
    return square(size, "round", 0.03 if size >= 32 else 0.0)


def write_all():
    from PIL import Image  # noqa: PLC0415
    for rel, spec in outputs().items():
        path = os.path.join(ROOT, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        kind = spec[0]
        if kind == "text":
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(spec[1])
        elif kind == "png":
            _, lay, layers, opaque = spec
            img = render(lay, layers or ("bg", "ring", "bolt", "word")).image(opaque)
            img.save(path, optimize=True)
        elif kind == "ico":
            imgs = [render(windows_layout(s)).image(False) for s in spec[1]]
            imgs[-1].save(path, format="ICO", sizes=[(s, s) for s in spec[1]], append_images=imgs[:-1])
        elif kind == "icns":
            imgs = [render(macos_layout(s)).image(False) for s in spec[1]]
            imgs[-1].save(path, format="ICNS", append_images=imgs[:-1])
        print(f"wrote {rel} ({os.path.getsize(path)} bytes)")
    del Image


# --- Check (plain Python) ---------------------------------------------------

MAX_BYTES = 2 * 1024 * 1024  # the game-data guard's size limit


def png_info(data):
    """(width, height, colour type) of a PNG; raises on anything else."""
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("not a PNG")
    w, h, depth, ctype = struct.unpack(">IIBB", data[16:26])
    if depth != 8:
        raise ValueError(f"bit depth {depth}")
    return w, h, ctype


def png_has_alpha_pixels(data):
    """True when an RGBA PNG has any pixel that is not fully opaque."""
    w, h, ctype = png_info(data)
    if ctype != 6:
        return False
    pos, idat = 8, b""
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        if typ == b"IDAT":
            idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride, bpp = w * 4, 4
    prev = bytearray(stride)
    i = 0
    for _ in range(h):
        ft = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ft == 1:
                line[x] = (line[x] + a) & 255
            elif ft == 2:
                line[x] = (line[x] + b) & 255
            elif ft == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        if any(v != 255 for v in line[3::4]):
            return True
        prev = line
    return False


def ico_sizes(data):
    reserved, typ, n = struct.unpack("<HHH", data[:6])
    if reserved != 0 or typ != 1:
        raise ValueError("not an ICO")
    sizes = []
    for k in range(n):
        w, h = data[6 + 16 * k], data[7 + 16 * k]
        sizes.append((w or 256, h or 256))
    return sorted(sizes)


def icns_types(data):
    if data[:4] != b"icns" or struct.unpack(">I", data[4:8])[0] != len(data):
        raise ValueError("not an ICNS")
    types, pos = [], 8
    while pos < len(data):
        typ, n = struct.unpack(">4sI", data[pos:pos + 8])
        types.append(typ.decode("ascii"))
        pos += n
    return types


# ICNS elements (PNG payloads) Pillow writes from those sizes: 128..1024 at
# 1x (ic07..ic10) and 16..256 at 2x (ic11..ic14); macOS scales for 16 at 1x.
ICNS_TYPES = ["ic07", "ic08", "ic09", "ic10", "ic11", "ic12", "ic13", "ic14"]


def check():
    problems = []
    known = outputs()
    for rel, spec in known.items():
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            problems.append(f"{rel}: missing")
            continue
        with open(path, "rb") as f:
            data = f.read()
        if len(data) > MAX_BYTES:
            problems.append(f"{rel}: {len(data)} bytes, over the guard's 2 MiB")
        kind = spec[0]
        try:
            if kind == "text":
                if data.decode("utf-8") != spec[1]:
                    problems.append(f"{rel}: differs from gen_icons.py (run it again)")
            elif kind == "png":
                _, lay, _, opaque = spec
                w, h, ctype = png_info(data)
                if (w, h) != (lay.w, lay.h):
                    problems.append(f"{rel}: {w}x{h}, expected {lay.w}x{lay.h}")
                if opaque and (ctype != 2 or png_has_alpha_pixels(data)):
                    problems.append(f"{rel}: must be opaque RGB (colour type {ctype})")
                if not opaque and ctype != 6:
                    problems.append(f"{rel}: expected RGBA (colour type {ctype})")
            elif kind == "ico":
                got = ico_sizes(data)
                want = [(s, s) for s in spec[1]]
                if got != want:
                    problems.append(f"{rel}: sizes {got}, expected {want}")
            elif kind == "icns":
                got = set(icns_types(data))
                missing = [t for t in ICNS_TYPES if t not in got]
                if missing:
                    problems.append(f"{rel}: no {', '.join(missing)}")
        except (ValueError, struct.error, zlib.error) as e:
            problems.append(f"{rel}: {e}")
    # Every file an asset catalog names must exist (and nothing stray in it).
    for cat in ("platform/ios/Assets.xcassets", "platform/tvos/Assets.xcassets"):
        for dirpath, _, files in os.walk(os.path.join(ROOT, cat)):
            for fn in files:
                rel = os.path.relpath(os.path.join(dirpath, fn), ROOT).replace(os.sep, "/")
                if rel not in known:
                    problems.append(f"{rel}: not made by gen_icons.py")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="verify the committed files instead of writing them")
    a = ap.parse_args()
    if a.check:
        problems = check()
        for p in problems:
            print(p)
        print(f"{len(outputs())} icon files, {len(problems)} problems")
        return 1 if problems else 0
    write_all()
    return 0


if __name__ == "__main__":
    sys.exit(main())

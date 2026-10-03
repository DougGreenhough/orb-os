#!/usr/bin/env python3
"""Build the Plasma theme for The Orb, from nothing but this folder and the firmware source.

    <venv>/bin/python themes/plasma/build.py            # everything
    <venv>/bin/python themes/plasma/build.py --no-fonts # keep the font_*.bin already in out/

Needs Pillow and numpy, a C++ compiler (for render_plasma.cpp, which runs the firmware's own
plasma engine to draw the filaments) and node (npx lv_font_conv, for the typefaces).

Writes the theme folder to out/plasma/ (the slug is `plasma`) and packs Plasma.orb beside
this file. Every random choice is seeded, so the same sources give the same bytes.

What each file is for, and which firmware source decides its shape:
  theme.json            name, author, app roster, and the asset list     (theme_style.cpp)
  *_style.json          one per screen: colours, positions, geometry     (theme_style.cpp)
  extras_style.json     the fork's own screens                           (orb_style.cpp)
  clock_plate.png       the dial, 466x466 (still: no extra frames)       (custom_sprite.cpp)
  clock_hand_*.png      hands, pointing up, pivot given in clock_style   (clock_view.cpp)
  clock_hand_*_<n>.png  more versions of each hand, shown in turn        (custom_sprite.cpp)
  clock_shadow_*.png    same size and pivot as the hand: here, its glow  (clock_view.cpp)
  clock_overlay.png     glass and the hub, over the hands                (custom_sprite.cpp)
  radar_*.png           plate, rings (over the map), blip, card          (radar_sprite.cpp)
  intel/menu/settings/splash plates, extras plate and glass, 466x466
  font_*.bin            lv_font_conv binaries, 4 bpp, uncompressed       (theme_font.cpp)
"""
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from plasma_art import (BLOOM, BLOOM_TIGHT, BLOOM_WIDE, C, CYAN, DEEP, DIM, HOT, MAGENTA, PINK,  # noqa: E402
                        RULE, TEXT, VIOLET, W, Mask, blur, glass_reflection, glow_disc, load_ppm,
                        neon, place, polar, radius_map, resize, rgb, save_plate, save_sprite, smooth,
                        spark, vignette_alpha)

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
OUT = HERE / "out" / "plasma"
TMP = HERE / "build"
FONTS = HERE / "fonts"

TILT = FONTS / "TiltNeon.ttf"
TITI_R = FONTS / "TitilliumWeb-Regular.ttf"
TITI_SB = FONTS / "TitilliumWeb-SemiBold.ttf"
CHAKRA_M = FONTS / "ChakraPetch-Medium.ttf"
CHAKRA_SB = FONTS / "ChakraPetch-SemiBold.ttf"

SEED = 20261003            # for sparks; the discharge has seeds of its own below
FRAMES = 0                 # EXTRA clock frames: none. The dial is still; the hands are the plasma now
HAND_FRAMES = 4            # EXTRA versions of each bolt (clock_hand_hour_1.png ...): the fork's handAnim
HAND_FPS = 9


def log(*a):
    print("[plasma]", *a, flush=True)


# =========================================================================================
# The discharge, from the firmware's own engine
# =========================================================================================
def render_discharge():
    """Compile render_plasma.cpp against the firmware's engine and run it three times."""
    TMP.mkdir(exist_ok=True)
    exe = TMP / "render_plasma"
    srcs = [HERE / "render_plasma.cpp", REPO / "src" / "plasma_engine.cpp", REPO / "src" / "plasma_render.cpp"]
    if not exe.exists() or any(s.stat().st_mtime > exe.stat().st_mtime for s in srcs):
        log("compiling render_plasma")
        subprocess.run(["c++", "-O2", "-std=c++17", "-ffp-contract=off", "-o", str(exe)] + [str(s) for s in srcs],
                       check=True)
    runs = {
        # prefix: seed, power, warm-up s, frames, s between frames
        "still": ("11", "0.30", "14", "14", "0.45"),   # far apart: texture, and the static gas to subtract
        "loop":  ("11", "0.30", "14", "4",  "0.05"),   # the same discharge, four frames a twentieth apart
        "ball":  ("5",  "0.62", "16", "3",  "0.50"),   # brighter and busier, for the splash
    }
    for name, args in runs.items():
        subprocess.run([str(exe), str(TMP / name), *args], check=True, stderr=subprocess.DEVNULL)
    still = [load_ppm(TMP / f"still{i:02d}.ppm") for i in range(14)]
    loop = [load_ppm(TMP / f"loop{i:02d}.ppm") for i in range(4)]
    ball = [load_ppm(TMP / f"ball{i:02d}.ppm") for i in range(3)]
    return still, loop, ball


def filaments_only(frame, base):
    """One engine frame (233 px) with the standing light taken out, at panel size.

    The renderer draws the gas, the glass, its reflection and the electrode into every
    frame. The median of each pixel across frames far apart in time is exactly that standing
    light (a channel is never in the same place twice), so subtracting it leaves the
    channels and their feet on true black. What is left is the channels' own light, which
    is close to white; it is pushed back towards the engine's blue-violet bodies and pink
    ends so it still reads as plasma when dimmed."""
    f = np.clip(frame - base * 1.04 - 0.035, 0, None) * 1.25
    mean = f.mean(axis=-1, keepdims=True)
    f = np.clip(mean + (f - mean) * 1.9, 0, None)                 # more colour
    f = f * np.array([1.00, 0.74, 1.32], np.float32)               # towards violet
    f[..., 1] = np.minimum(f[..., 1], 0.85 * np.maximum(f[..., 0], f[..., 2]))   # never green
    f = np.clip(f, 0, 1) ** 0.85                                   # lift the faint ones
    return resize(f, W)


# =========================================================================================
# Pieces shared by several screens
# =========================================================================================
def glass_pill(x0, y0, x1, y1, color, w=W, h=W, fill=0.07, edge=0.8, lw=1.6, bloom=BLOOM_TIGHT):
    """A glass lozenge lit along its rim: faint tinted fill, neon outline, a sheen on top."""
    rad = (y1 - y0) / 2
    inner = Mask(w, h).rrect(x0, y0, x1, y1, rad).arr()
    line = Mask(w, h).rrect(x0, y0, x1, y1, rad, wd=lw).arr()
    yy = np.mgrid[0:h, 0:w][0].astype(np.float32)
    t = np.clip((yy - y0) / max(1.0, y1 - y0), 0, 1)
    light = (inner * fill * (1.25 - 0.8 * t))[..., None] * rgb(color)
    light += neon(line, color, lw=lw, core=edge, hot=0.45, bloom=bloom, gain=edge)
    # sheen: a thin white arc just inside the top edge
    sheen = Mask(w, h).rrect(x0 + rad * 0.7, y0 + 2.5, x1 - rad * 0.7, y0 + 4.2, 1).arr()
    light += blur(sheen, 1.0)[..., None] * np.array([0.30, 0.28, 0.36], np.float32)
    return light


def scatter_sparks(light, rng, n, rmin, rmax, colors=(HOT, PINK, CYAN), keepout=None):
    for _ in range(n):
        for _try in range(20):
            r = rng.uniform(rmin, rmax)
            a = rng.uniform(0, 360)
            x, y = polar(r, a)
            if keepout is None or not keepout(x, y):
                break
        d = a + rng.uniform(-70, 70)                      # mostly outward, like a thrown spark
        col = colors[int(rng.integers(0, len(colors)))]
        spark(light, x, y, d, rng.uniform(7, 18), color=col, size=rng.uniform(0.7, 1.15))
    return light


def rim(color=VIOLET, r=227.0, lw=1.6, k=0.55, bloom=BLOOM_TIGHT):
    return neon(Mask().ring(C, C, r, lw).arr(), color, lw=lw, core=k, hot=0.3, bloom=bloom, gain=k)


def neon_text(s, x, y, font, size, color, w=W, h=W, spacing=0, bloom=BLOOM_WIDE, hot=0.7, gain=1.0):
    m = Mask(w, h).text(x, y, s, font, size, spacing=spacing).arr()
    return neon(m, color, lw=max(2.0, size * 0.085), hot=hot, bloom=bloom, gain=gain)


# =========================================================================================
# Clock
# =========================================================================================
HAND_W = 92                # wide enough for a bolt to wander and still fade out before the sprite's edge
HAND_PAD = 30
HANDS = {
    #          length  start  width  colour
    # Each hand is an arc of plasma from the electrode: its own colour, its own reach.
    "hour":   (108,    30,    5.2,   MAGENTA),
    "minute": (170,    30,    3.8,   CYAN),
    "second": (201,    30,    1.9,   HOT),
}
DATE_Y = 322


def clock_static():
    light = np.zeros((W, W, 3), np.float32)
    # hour markers: short neon tubes; the quarters longer and cyan, twelve doubled
    mq, mh = Mask(), Mask()
    for i in range(12):
        a = i * 30
        if i == 0:
            for off in (-5.5, 5.5):
                x0, y0 = polar(172, a); x1, y1 = polar(207, a)
                mq.line(x0 + off, y0, x1 + off, y1, 4.2)
        elif i % 3 == 0:
            x0, y0 = polar(172, a); x1, y1 = polar(207, a)
            mq.line(x0, y0, x1, y1, 4.6)
        else:
            x0, y0 = polar(187, a); x1, y1 = polar(207, a)
            mh.line(x0, y0, x1, y1, 3.6)
    light += neon(mq.arr(), CYAN, lw=4.4, hot=0.62, bloom=BLOOM)
    light += neon(mh.arr(), MAGENTA, lw=3.6, hot=0.55, bloom=BLOOM)
    # minute pips
    mp = Mask()
    for i in range(60):
        if i % 5:
            x, y = polar(205.5, i * 6)
            mp.dot(x, y, 1.15)
    light += neon(mp.arr(), VIOLET, lw=2.3, core=0.75, hot=0.35, bloom=((1.4, 0.3), (3.5, 0.12)))
    # the glass's edge, as a thin violet tube
    light += rim(VIOLET, r=225.5, lw=1.6, k=0.62, bloom=BLOOM)
    # date window
    light += glass_pill(C - 70, DATE_Y - 17, C + 70, DATE_Y + 17, VIOLET, fill=0.085, edge=0.62)
    # the electrode the filaments leave from: a small glass sphere, lit from inside
    light += glow_disc((W, W), C, C, [(HOT, 13.0, 0.75), (MAGENTA, 25.0, 0.55), (VIOLET, 44.0, 0.22)])
    light += neon(Mask().ring(C, C, 37.0, 1.3).arr(), PINK, lw=1.3, core=0.55, hot=0.5, bloom=((1.4, 0.3), (4.0, 0.16)))
    # the maker's mark, small, where a watch has one
    light += neon_text("PLASMA", C, 148, TILT, 19, MAGENTA, spacing=4.5, bloom=BLOOM_TIGHT, hot=0.45, gain=0.8) * 0.8
    return light


def bolt(rng, a, b, sway, limit):
    """A jagged path from a to b by midpoint displacement: the sideways kick halves at each
    level, so it reads as one arc with fine crackle on it. `limit` keeps it this far, at
    most, from the straight line (the sprite is only so wide)."""
    pts = [a, b]
    amp = sway
    for _ in range(5):
        out = [pts[0]]
        for p, q in zip(pts, pts[1:]):
            mx, my = (p[0] + q[0]) / 2, (p[1] + q[1]) / 2
            dx, dy = q[0] - p[0], q[1] - p[1]
            d = max(1e-6, float(np.hypot(dx, dy)))
            k = rng.uniform(-1, 1) * amp
            mx, my = mx - dy / d * k, my + dx / d * k
            mx = float(np.clip(mx, a[0] - limit, a[0] + limit))
            out += [(mx, my), q]
        pts = out
        amp *= 0.55
    return pts


def build_clock(still, loop, base):
    log("clock")
    # One still plate. The dial used to carry a loop of the engine's filaments behind the
    # hands; the hands themselves are the discharge now, so the dial is left dark and quiet.
    save_plate(OUT / "clock_plate.png", clock_static())

    pivots = {}
    for name, (length, start, width, color) in HANDS.items():
        tail = max(0, -start)
        h = HAND_PAD + length + tail + HAND_PAD
        px, py = HAND_W / 2, HAND_PAD + length
        # A bolt, not a tube: a jagged path from just outside the electrode to the tip, drawn
        # thick at the root and fine at the end, with a few short forks leaving it and a
        # pink spark where it lands (the engine's filaments end pink too). Each hand is drawn
        # 1 + HAND_FRAMES times from different seeds, same root and same tip: the clock shows
        # one after another (handAnim), so the arc writhes while its two ends keep the time.
        sway = {"hour": 9.0, "minute": 12.0, "second": 13.0}[name]
        # keep the glow off the sprite's own edges, so no box shows when it turns
        xs = np.arange(HAND_W, dtype=np.float32) + 0.5
        edge = np.clip(np.minimum(xs, HAND_W - xs) / 9.0, 0, 1)[None, :, None]
        ys = np.arange(h, dtype=np.float32) + 0.5
        edge = edge * np.clip(np.minimum(ys, h - ys) / 9.0, 0, 1)[:, None, None]
        tip = Mask(HAND_W, h).dot(px, py - length, max(2.2, width * 0.55)).arr()
        for frame in range(HAND_FRAMES + 1):
            hrng = np.random.default_rng(SEED + {"hour": 11, "minute": 23, "second": 37}[name] + 1000 * frame)
            pts = bolt(hrng, (px, py - start), (px, py - length), sway, HAND_W / 2 - 16)
            body, core = Mask(HAND_W, h), Mask(HAND_W, h)
            n = len(pts) - 1
            for i in range(n):
                k = i / n
                wd = width * (1.0 - 0.55 * k)                      # tapers to the tip
                body.line(*pts[i], *pts[i + 1], wd)
                core.line(*pts[i], *pts[i + 1], max(0.7, wd * 0.34))
            # forks: short, thin, leaning outwards and forwards
            forks = Mask(HAND_W, h)
            for f in range({"hour": 2, "minute": 3, "second": 3}[name]):
                i = int(n * (0.25 + 0.6 * (f + hrng.random() * 0.6) / 3.2))
                x0, y0 = pts[min(i, n - 1)]
                side = 1 if (f + frame + (name == "minute")) % 2 else -1
                ang = np.radians(side * hrng.uniform(24, 46))
                ln = hrng.uniform(13, 26) * (0.8 if name == "hour" else 1.0)
                fx, fy = x0 + np.sin(ang) * ln, y0 - np.cos(ang) * ln
                fx = float(np.clip(fx, 14, HAND_W - 14))
                mid = ((x0 + fx) / 2 + hrng.uniform(-3, 3), (y0 + fy) / 2 + hrng.uniform(-2, 2))
                forks.line(x0, y0, *mid, max(0.9, width * 0.30))
                forks.line(*mid, fx, fy, max(0.7, width * 0.22))
            m = np.maximum(body.arr(), forks.arr() * 0.8)
            light = neon(m, color, lw=width, core=0.95, hot=0.45, bloom=BLOOM_TIGHT)
            light += core.arr()[..., None] * np.array([0.95, 0.95, 0.95], np.float32)
            light += neon(tip, PINK, lw=5.0, hot=0.85, bloom=((1.5, 0.6), (4.0, 0.38)))
            # the firmware skips hand pixels under alpha 8; fade to that instead of stopping at it
            save_sprite(OUT / (f"clock_hand_{name}.png" if frame == 0 else f"clock_hand_{name}_{frame}.png"),
                        light * edge, min_alpha=0.034)
        # The wide glow ("shadow" sprite) is one image for all the versions, so it follows the
        # straight line between the two ends: the air the arc is burning through, which does
        # not jump about when the arc does.
        spine = Mask(HAND_W, h)
        for i in range(12):
            k0, k1 = i / 12, (i + 1) / 12
            spine.line(px, py - start - (length - start) * k0, px, py - start - (length - start) * k1,
                       width * (1.0 - 0.55 * k0) * 1.6)
        m = spine.arr()
        # wide and faint: at the old strength a straight glow beside a crooked bolt read as
        # a ghost of the hand
        halo = (blur(m, 9.0) * 0.15 * (9.0 * 2.5066 / max(width * 1.6, 3.0)) + blur(m, 15.0) * 0.09 * (15.0 * 2.5066 / max(width * 1.6, 3.0)))[..., None] * rgb(color)
        halo += (blur(tip, 6.0) * 2.4)[..., None] * rgb(PINK) * 0.22
        save_sprite(OUT / f"clock_shadow_{name}.png", halo * edge, min_alpha=0.034)
        pivots[name] = (int(px), int(py))

    # glass, and the electrode the hands turn on
    hub = glow_disc((W, W), C, C, [(0xFFFFFF, 6.0, 1.3), (HOT, 10.0, 0.9), (MAGENTA, 17.0, 0.5)])
    save_sprite(OUT / "clock_overlay.png", hub + glass_reflection(1.0), shade=vignette_alpha(205, 236, 0.30))
    return pivots


# =========================================================================================
# Flight Tracker
# =========================================================================================
RADAR_R = 218.0
BLIP_W, BLIP_H, BLIP_PX, BLIP_PY = 44, 76, 22, 20
CARD_W, CARD_H = 204, 110


def build_radar():
    log("flight tracker")
    # plate: black, the lit rim with its graduations and compass points, the glass
    plate = np.zeros((W, W, 3), np.float32)
    plate += neon(Mask().ring(C, C, RADAR_R, 2.4).arr(), MAGENTA, lw=2.4, hot=0.55, bloom=BLOOM)
    ticks = Mask()
    for i in range(72):
        a = i * 5
        if i % 18 == 0:
            continue                       # the compass letters sit here
        r0 = RADAR_R - (12 if i % 6 == 0 else 6)
        x0, y0 = polar(r0, a); x1, y1 = polar(RADAR_R - 3.0, a)
        ticks.line(x0, y0, x1, y1, 1.4 if i % 6 == 0 else 1.0)
    plate += neon(ticks.arr(), PINK, lw=1.2, core=0.75, hot=0.4, bloom=((1.3, 0.3), (3.5, 0.12)))
    letters = Mask()
    for ch, a in (("N", 0), ("E", 90), ("S", 180), ("W", 270)):
        x, y = polar(RADAR_R - 17, a)
        letters.text(x, y, ch, CHAKRA_SB, 17)
    plate += neon(letters.arr(), CYAN, lw=2.2, hot=0.65, bloom=((1.3, 0.5), (3.5, 0.3), (8.0, 0.14)))
    plate += glass_reflection(0.85)
    save_plate(OUT / "radar_plate.png", plate)

    # rings, over the map: the two inner circles and the crosshair. The lit rim, its
    # graduations and the compass points are in the plate, outside the keep-out circle.
    light = np.zeros((W, W, 3), np.float32)
    inner = Mask()
    for k in (1, 2):
        inner.ring(C, C, RADAR_R * k / 3, 1.5)
    light += neon(inner.arr(), VIOLET, lw=1.5, core=0.8, hot=0.4, bloom=((1.4, 0.45), (4.0, 0.26), (10.0, 0.12)))
    cross = Mask()
    for a in (0, 90, 180, 270):
        x0, y0 = polar(20, a); x1, y1 = polar(RADAR_R - 30, a)
        cross.line(x0, y0, x1, y1, 1.0)
    light += neon(cross.arr(), VIOLET, lw=1.0, core=0.38, hot=0.2, bloom=((2.0, 0.10),))
    save_sprite(OUT / "radar_rings.png", light)

    # blip: a hot spark with its trail. Drawn nose up; the scope turns it to the aircraft's track.
    m = Mask(BLIP_W, BLIP_H)
    n = 9
    for i in range(n):
        t0, t1 = i / n, (i + 1) / n
        m.line(BLIP_PX, BLIP_PY + 3 + 44 * t0, BLIP_PX, BLIP_PY + 3 + 44 * t1, 3.2 * (1 - 0.75 * t0), v=(1 - t0) ** 1.5 * 0.85)
    trail = neon(m.arr(), MAGENTA, lw=2.6, hot=0.35, bloom=((1.4, 0.5), (3.5, 0.25)))
    head = glow_disc((BLIP_H, BLIP_W), BLIP_PX, BLIP_PY, [(0xFFFFFF, 3.4, 1.4), (HOT, 5.5, 0.9), (MAGENTA, 10.0, 0.55)])
    # four short rays, so it reads as a spark and not a dot
    rays = Mask(BLIP_W, BLIP_H)
    for a in (45, 135, 225, 315):
        x1, y1 = polar(9.5, a, BLIP_PX, BLIP_PY)
        rays.line(BLIP_PX, BLIP_PY, x1, y1, 1.0, v=0.8)
    for a in (0, 90, 270):
        x1, y1 = polar(13.0, a, BLIP_PX, BLIP_PY)
        rays.line(BLIP_PX, BLIP_PY, x1, y1, 1.2)
    blip = trail + head + neon(rays.arr(), HOT, lw=1.2, hot=0.6, bloom=((1.2, 0.3),))
    save_sprite(OUT / "radar_blip.png", blip, min_alpha=0.03)

    # the selection card: smoked glass with a lit rim
    pad = 9
    card = glass_pill(pad, pad, CARD_W - pad, CARD_H - pad, VIOLET, w=CARD_W, h=CARD_H, fill=0.10, edge=0.9, lw=1.6)
    # glass_pill rounds fully; a card wants a gentler corner, so redraw the shapes by hand
    inner = Mask(CARD_W, CARD_H).rrect(pad, pad, CARD_W - pad, CARD_H - pad, 18).arr()
    line = Mask(CARD_W, CARD_H).rrect(pad, pad, CARD_W - pad, CARD_H - pad, 18, wd=1.6).arr()
    yy = np.mgrid[0:CARD_H, 0:CARD_W][0].astype(np.float32)
    t = np.clip((yy - pad) / (CARD_H - 2 * pad), 0, 1)
    card = (inner * 0.11 * (1.3 - 0.9 * t))[..., None] * rgb(VIOLET)
    card += neon(line, VIOLET, lw=1.6, core=0.9, hot=0.5, bloom=BLOOM_TIGHT, gain=0.9)
    sheen = Mask(CARD_W, CARD_H).rrect(pad + 16, pad + 3, CARD_W - pad - 16, pad + 4.6, 1).arr()
    card += blur(sheen, 1.0)[..., None] * np.array([0.32, 0.30, 0.40], np.float32)
    save_sprite(OUT / "radar_card.png", card, shade=inner * 0.86)


# =========================================================================================
# News
# =========================================================================================
def build_news(rng):
    log("news")
    light = np.zeros((W, W, 3), np.float32)
    # the sign
    light += neon_text("NEWS", C, 62, TILT, 38, MAGENTA, spacing=5)
    rule = Mask()
    rule.line(C - 150, 62, C - 78, 62, 1.6).line(C + 78, 62, C + 150, 62, 1.6)
    light += neon(rule.arr(), CYAN, lw=1.6, core=0.9, hot=0.5, bloom=BLOOM)
    # the glass the headlines sit on: a pane with a lit top edge and a faint frame
    x0, y0, x1, y1, rad = 44, 98, W - 44, 384, 30
    pane = Mask().rrect(x0, y0, x1, y1, rad).arr()
    yy = np.mgrid[0:W, 0:W][0].astype(np.float32)
    t = np.clip((yy - y0) / (y1 - y0), 0, 1)
    light += (pane * 0.050 * (1.5 - 1.1 * t))[..., None] * rgb(VIOLET)
    frame = Mask().rrect(x0, y0, x1, y1, rad, wd=1.3).arr()
    light += neon(frame, VIOLET, lw=1.3, core=0.42, hot=0.3, bloom=((1.5, 0.18), (5.0, 0.08)), gain=1.0)
    top = Mask().line(x0 + rad + 6, y0, x1 - rad - 6, y0, 1.8).arr()
    light += neon(top, MAGENTA, lw=1.8, core=0.9, hot=0.5, bloom=BLOOM)
    # a diagonal sheen across the pane
    xx = np.mgrid[0:W, 0:W][1].astype(np.float32)
    band = np.exp(-(((xx - yy * 0.55) - 60) / 34) ** 2) * pane
    light += (band * 0.028)[..., None] * np.array([0.9, 0.88, 1.0], np.float32)
    light += rim(VIOLET, r=227.5, lw=1.4, k=0.42)
    light += glass_reflection(0.75)
    scatter_sparks(light, rng, 7, 196, 220, keepout=lambda x, y: (70 < y < 400 and 40 < x < W - 40) or y > 380 or y < 90)
    save_plate(OUT / "intel_plate.png", light)


# =========================================================================================
# Menu, Settings, Splash, and the fork's own screens
# =========================================================================================
def edge_filaments(frame, base, r0, r1, level):
    f = filaments_only(frame, base)
    r = radius_map()
    return f * (smooth(r0, r1, r) * level)[..., None]


def build_menu(still, base, rng):
    log("menu")
    light = edge_filaments(still[5], base, 122, 190, 0.50)
    light += glow_disc((W, W), C, C, [(MAGENTA, 78.0, 0.075)])     # a breath of light behind the name
    light += rim(VIOLET, r=227.0, lw=1.6, k=0.55, bloom=BLOOM)
    light += glass_reflection(0.9)
    # keep them off the middle and off the two hints above and below it
    scatter_sparks(light, rng, 9, 150, 216, keepout=lambda x, y: (abs(x - C) < 125 and (abs(y - 92) < 26 or abs(y - 374) < 26)) or (abs(x - C) < 200 and abs(y - C) < 44))
    save_plate(OUT / "menu_plate.png", light)


SET_HL_W, SET_HL_H = 316, 56


def build_settings(still, base, rng):
    log("settings")
    light = edge_filaments(still[9], base, 150, 205, 0.26)
    light += rim(VIOLET, r=227.0, lw=1.6, k=0.5, bloom=BLOOM)
    # the wheel's notch: two cyan chevron ticks at the rim, pointing at the selected row
    ticks = Mask()
    for sx in (-1, 1):
        ticks.line(C + sx * 214, C, C + sx * 198, C, 2.0)
    light += neon(ticks.arr(), CYAN, lw=2.0, hot=0.6, bloom=BLOOM)
    light += glass_pill(C - SET_HL_W / 2, C - SET_HL_H / 2, C + SET_HL_W / 2, C + SET_HL_H / 2,
                        MAGENTA, fill=0.085, edge=1.0, lw=2.0, bloom=BLOOM)
    light += glass_reflection(0.8)
    scatter_sparks(light, rng, 6, 168, 214, keepout=lambda x, y: abs(y - C) < 46)
    save_plate(OUT / "settings_plate.png", light)


BALL_CY, BALL_R = 168.0, 148.0


def build_splash(ball, rng):
    log("splash")
    light = np.zeros((W, W, 3), np.float32)
    size = int(round(BALL_R * 2))
    b = resize(ball[1], size)
    # the engine's frame is a disc on black already; soften its last pixel so the glass has an edge
    rr = radius_map(size, size, size / 2, size / 2)
    b = b * (1 - smooth(BALL_R - 1.5, BALL_R, rr))[..., None]
    place(light, b, C, BALL_CY)
    # the light it throws: a wide violet breath round the sphere, and a pool under it
    light += glow_disc((W, W), C, BALL_CY, [(DEEP, BALL_R * 1.25, 0.10)]) * (smooth(BALL_R - 6, BALL_R + 30, radius_map(W, W, C, BALL_CY)))[..., None]
    light += neon_text("PLASMA", C, 356, TILT, 50, MAGENTA, spacing=7)
    rule = Mask()
    rule.line(C - 96, 388, C + 96, 388, 1.5)
    light += neon(rule.arr(), CYAN, lw=1.5, core=0.85, hot=0.5, bloom=BLOOM)
    scatter_sparks(light, rng, 8, 176, 218, keepout=lambda x, y: y > 300 or math.hypot(x - C, y - BALL_CY) < BALL_R + 6)
    save_plate(OUT / "splash.png", light)


def build_extras(rng):
    log("extras")
    # backdrop for Forecast, Music, Photos, Facts, Globe: black, a glass rim, a hint of glow low down
    light = rim(VIOLET, r=227.0, lw=1.6, k=0.5, bloom=BLOOM)
    arc = Mask().arc(C, C, 227.0, 150, 210, 1.8).arr()
    light += neon(arc, MAGENTA, lw=1.8, core=0.8, hot=0.5, bloom=BLOOM_WIDE, gain=0.9)
    arc2 = Mask().arc(C, C, 227.0, -30, 30, 1.8).arr()
    light += neon(arc2, CYAN, lw=1.8, core=0.6, hot=0.5, bloom=BLOOM, gain=0.7)
    save_plate(OUT / "extras_plate.png", light)
    # their glass: only the reflection, so text under it is not washed out
    save_sprite(OUT / "extras_overlay.png", glass_reflection(0.8))


# =========================================================================================
# Typefaces
# =========================================================================================
ASCII = "0x20-0x7E"
FONT_SLOTS = {
    # file                    face        px  ranges
    "font_clock1.bin":       (CHAKRA_SB, 18, ASCII),
    "font_menu_current.bin": (TILT,      46, ASCII),
    "font_menu_prev.bin":    (CHAKRA_M,  17, ASCII),
    "font_menu_next.bin":    (CHAKRA_M,  17, ASCII),
    "font_settings.bin":     (TITI_R,    25, ASCII),
    "font_settings_sel.bin": (TILT,      30, ASCII),
    "font_radar1.bin":       (CHAKRA_SB, 22, ASCII),
    "font_radar2.bin":       (CHAKRA_M,  15, ASCII),
    "font_radar3.bin":       (CHAKRA_M,  15, ASCII),
    "font_radar_loc.bin":    (CHAKRA_M,  14, ASCII),
    "font_intel_title.bin":  (TILT,      28, ASCII),
    "font_intel_text.bin":   (TITI_SB,   22, ASCII),
    "font_intel_source.bin": (CHAKRA_M,  13, ASCII),
    "font_intel_age.bin":    (CHAKRA_M,  14, ASCII),
    "font_intel_brief.bin":  (TITI_R,    20, ASCII),
    # the weather map shows temperatures: these carry the degree sign
    "font_weather1.bin":     (CHAKRA_SB, 24, ASCII + ",0xB0"),
    "font_weather2.bin":     (CHAKRA_M,  16, ASCII + ",0xB0"),
    "font_weather3.bin":     (CHAKRA_M,  14, ASCII + ",0xB0"),
    "font_weather4.bin":     (CHAKRA_M,  13, ASCII + ",0xB0"),
}


def build_fonts():
    log("typefaces")
    done = {}
    for name, (face, px, ranges) in FONT_SLOTS.items():
        key = (face, px, ranges)
        dst = OUT / name
        if key in done:
            shutil.copyfile(done[key], dst)
            continue
        # 4 bpp, uncompressed, no prefilter: what the firmware's own glyph reader expects
        # (clock_view.cpp glyph_alpha4) and what Orb Studio's fonts are.
        subprocess.run(["npx", "--yes", "lv_font_conv@1.5.3", "--font", str(face), "--size", str(px), "--bpp", "4",
                        "--format", "bin", "--no-compress", "--no-prefilter", "-r", ranges, "-o", str(dst)],
                       check=True, cwd=str(TMP))
        done[key] = dst


# =========================================================================================
# Style files
# =========================================================================================
def text_slot(**kw):
    d = {"show": True, "x": 233, "y": 233, "color": TEXT, "opa": 255, "glow": 0, "glowColor": MAGENTA,
         "fmt": "", "upper": False, "curved": False, "curveR": 0, "arcDeg": 0, "align": 1,
         "bg": 0, "bgOpa": 0, "radius": 4}
    d.update(kw)
    return d


def write_json(name, obj):
    data = json.dumps(obj, separators=(",", ":"), ensure_ascii=True)
    assert len(data) < 8000, f"{name} is {len(data)} bytes; the firmware reads at most 8192"
    (OUT / name).write_text(data + "\n")


def build_styles(pivots):
    log("style files")
    hand = lambda n: {"show": True, "pivotX": pivots[n][0], "pivotY": pivots[n][1],
                      "centerX": 233, "centerY": 233, "blend": 2}   # screen: light adds to light
    write_json("clock_style.json", {
        "bg": 0, "plateFollow": 0, "textOverHands": False, "secondSweep": True, "secondRailway": False,
        "handAnim": {"frames": HAND_FRAMES, "fps": HAND_FPS},
        "text1": text_slot(y=DATE_Y, color=0xF0E2FF, glow=3, glowColor=0x9A3DFF, fmt="%a %d %b", upper=True),
        "text2": {"show": False},
        "windOn": False,
        "hands": {
            "order": [3, 4, 0, 1, 2],
            # the "shadow" sprites are each hand's wide glow, so the light does not move off it
            "shadow": {"on": True, "dx": 0, "dy": 0},
            "hour": hand("hour"), "minute": hand("minute"), "second": hand("second"),
            "static1": {"show": False}, "static2": {"show": False},
        },
    })

    rt = lambda **kw: dict(text_slot(**kw), onCard=True)
    write_json("radar_style.json", {
        "order": [3, 4, 0, 1, 2, 5], "ringsPlate": True,
        "sweepEnabled": True, "sweepTypeImage": False,
        "sweepColor": VIOLET, "sweepLeadColor": HOT, "sweepTrailDeg": 46, "sweepOpacity": 62,
        "sweepLength": 212, "sweepSpeed": 40, "sweepTrailWidth": 6, "sweepLeadWidth": 2, "sweepTrailSteps": 60,
        "sweepHubOn": True, "sweepHubColor": HOT, "sweepHubRadius": 6, "sweepHubGlow": 14, "sweepHubGlowColor": MAGENTA,
        "blipEnabled": True, "blipTypeImage": True, "blipRotate": True, "blipKiteShape": True, "blipKiteT": 15,
        "blipSize": 9, "blipFixedColorMode": True, "blipFixedColor": HOT,
        "blipAltGround": 0x6A5A8A, "blipAltLow": MAGENTA, "blipAltMid": PINK, "blipAltHigh": 0xC9A6FF,
        "blipAltCruise": VIOLET, "blipAltJet": CYAN,
        "blipGlow": 0, "blipGlowColor": MAGENTA, "blipImageTint": False,
        "blipPivotX": BLIP_PX, "blipPivotY": BLIP_PY,
        "selEnabled": True, "selStyle": 0, "selColor": CYAN, "selWidth": 2, "selDiameter": 40,
        "selGlow": 9, "selGlowColor": CYAN,
        "offRangeEnabled": True, "offRangeColor": MAGENTA, "offRangeSize": 5,
        "centerEnabled": False, "centerRadius": 5, "centerColor": HOT, "centerInnerRadius": 2, "centerInnerColor": 0xFFFFFF,
        "rtext": [
            rt(y=206, color=CYAN, glow=2, glowColor=0x1E90C8, fmt="{callsign}  {type}", upper=True),
            rt(y=233, color=TEXT, fmt="{alt} ft  {spd} kt  {dist} km"),
            rt(y=258, color=0xB79BE6, fmt="{from} > {to}"),
            {"show": False},
        ],
        "locText": {"show": False},
        "mapRoadsOn": True, "mapRoadColor": 0x6A4FB0, "mapRoadWidth": 1, "mapRoadOpacity": 120,
        "mapAirportsOn": True, "mapAirportColor": 0x8A7AB8,
        "card": {"enabled": True, "typeImage": True, "radius": 112, "w": CARD_W - 18, "h": CARD_H - 18, "corner": 18,
                 "color": 0x0B0616, "opacity": 255, "borderColor": VIOLET, "borderWidth": 1},
        "static1": {"show": False}, "static2": {"show": False},
        "overlayEnabled": False, "overlayColor": 0, "overlayOpacity": 0,
        # one inverted circle: nothing that moves may cross the lit rim
        "zones": [{"x": 233, "y": 233, "r": 208, "w": 0, "h": 0, "rect": False, "invert": True}],
        "rangeKm": 40, "maxAircraft": 8, "minAltFt": 1000, "hideGround": True, "deadZonePx": 16,
    })

    mt = lambda **kw: dict({"show": True, "x": 233, "y": 233, "color": TEXT, "opa": 255, "glow": 0,
                            "glowColor": MAGENTA, "fmt": "{name}", "upper": False, "align": 1,
                            "wrapWidth": 0, "lineGap": 0, "lineStep": 20}, **kw)
    write_json("menu_style.json", {
        "current": mt(color=0xFFE9FA, glow=7, glowColor=MAGENTA, wrapWidth=300, lineStep=52),
        "prev": mt(y=92, color=DIM, opa=230, upper=True),
        "next": mt(y=374, color=DIM, opa=230, upper=True),
    })

    write_json("settings_style.json", {
        "wheelR": 172, "wheelRx": 18, "wheelStepDeg": 24, "wheelCy": 0, "wheelFade": 1.5,
        "selColor": 0xFFE9FA, "selOpa": 255, "itemColor": 0xA98AD8, "itemOpa": 235,
        "glow": 5, "glowColor": MAGENTA, "selGlow": 5, "selGlowColor": MAGENTA,
        "itemGlow": 0, "itemGlowColor": VIOLET,
        # the pill is in the plate, lit; the firmware's flat rectangle stays off
        "hlShow": False, "hlColor": 0x1A0A2E, "hlOpacity": 200, "hlW": SET_HL_W, "hlH": SET_HL_H, "hlRadius": 28,
        "defaultSel": 0,
    })

    write_json("intel_style.json", {
        "bg": 0, "titleColor": MAGENTA, "titleOpa": 255, "textColor": TEXT, "textOpa": 255,
        "sourceColor": 0x8E6FD0, "sourceOpa": 255, "staleColor": 0xFF8A5C,
        "count": 10, "onScreen": 3, "topic": "general", "source": "bbc",
        "curvedBounds": False, "curveRadius": 200,
        # the title is the neon sign in the plate
        "title": "NEWS", "titleShow": False, "titleSize": 28, "titleX": 233, "titleY": 62,
        "textSize": 22, "marginLeft": 62, "marginRight": 62, "marginTop": 110, "marginBottom": 94,
        "pollMinutes": 10, "blockOffsetY": 0, "lineGap": 0, "sourceGap": 2, "sourceSize": 13,
        "textAlign": "left", "sourceAlign": "left", "blockAngle": 0,
        "ageFmt": "updated {t}", "ageShow": True, "ageX": 233, "ageY": 412, "ageColor": 0x8E6FD0, "ageOpa": 255,
        "ageSize": 14, "ageGlow": 0, "ageGlowColor": VIOLET, "ageCurved": True, "ageCurveR": 196, "ageArcDeg": 180,
        "ageBg": 0, "ageBgOpa": 0, "ageRadius": 4,
        "selDim": 150, "selColorOn": True, "selColor": 0xFFFFFF,
        "selBarOn": True, "selBarColor": VIOLET, "selBarOpa": 54, "selBarRadius": 12, "selBarPadX": 10, "selBarPadY": 5,
        "briefColorOn": True, "briefColor": 0xE6DAFA, "briefOpa": 255, "briefGap": 14, "briefSize": 20,
        "briefHideTitle": False,
        "morePlace": False, "moreX": 233, "moreY": 400, "backPlace": False, "backX": 233, "backY": 370,
        "briefMorePlace": False, "briefMoreX": 233, "briefMoreY": 340,
    })

    st = lambda **kw: dict({"x": 233, "y": 400, "size": 12, "color": DIM, "opa": 255, "glow": 0, "glowColor": MAGENTA,
                            "align": 1, "curved": False, "curveR": 0, "arcDeg": 0, "bg": 0, "bgOpa": 0, "radius": 4,
                            "upper": False}, **kw)
    write_json("splash_style.json", {
        "version": st(y=405, size=14, color=0xE6DAFA),
        "network": st(y=300, show=False),
        "credits": st(y=424, size=12, color=0x8E6FD0),
        "theme": st(y=300, show=False),
    })

    wt = lambda **kw: dict(text_slot(**kw), onCard=False)
    write_json("weather_style.json", {
        "bg": 0, "sweepEnabled": True, "sweepTypeImage": False, "sweepColor": VIOLET, "sweepLeadColor": HOT,
        "sweepTrailDeg": 30, "sweepOpacity": 50, "sweepLength": 214, "sweepSpeed": 30,
        "sweepTrailWidth": 4, "sweepLeadWidth": 2, "sweepTrailSteps": 20,
        "ringsEnabled": True, "ringColor": VIOLET, "ringColorOn": True, "ringCount": 3, "ringWidth": 1,
        "ringOpacity": 170, "crosshair": False,
        "roadsEnabled": True, "roadColor": 0x4A3A78, "coastEnabled": True, "coastColor": 0x35A8C8,
        "wtext": [
            wt(y=388, color=TEXT, glow=2, glowColor=MAGENTA, fmt="{temp}{unit}  {cond}"),
            wt(y=412, color=0xB79BE6, fmt="WIND {windDir} {wind} {windUnit}"),
            wt(x=71, color=DIM, fmt="{range} {rangeUnit}", curved=True, curveR=166, arcDeg=270),
            wt(y=14, color=DIM, fmt="updated {updated}", curved=True, curveR=224, arcDeg=0),
        ],
        "credit": {"x": 233, "y": 123, "color": DIM, "opa": 255, "bg": 0, "bgOpa": 0, "radius": 4,
                   "upper": False, "align": 1, "curved": True, "curveR": 214, "arcDeg": 90},
        "zones": [],
    })
    write_json("ticker_style.json", {
        "bg": 0, "symbols": "^GSPC,^DJI,^IXIC,AAPL", "pollSeconds": 300,
        "upColor": CYAN, "downColor": MAGENTA, "flatColor": DIM,
        "nameColor": DIM, "nameSize": 16, "nameY": 186, "nameShow": True,
        "priceSize": 40, "priceY": 222, "priceShow": True, "priceColorOn": True, "priceColor": TEXT,
        "changeSize": 22, "changeY": 286, "changeShow": True, "changePct": True,
        "stripShow": True, "stripPlace": 2, "stripSize": 16, "stripColor": 0xB79BE6, "stripOpa": 235,
        "stripSpeed": 26, "stripY": 392, "stripRadius": 196, "stripAngle": 0, "stripUpDown": True,
    })

    hx = lambda v: "#%06x" % v
    write_json("extras_style.json", {
        "bg": "#000000", "text": hx(TEXT), "dim": hx(DIM), "accent": hx(0xFF4FD8), "accent2": hx(0x4FD8FF),
        "rule": hx(RULE), "dark": True, "mono": False, "glow": 3, "sparks": True, "plateDim": 0,
        "plate": "extras_plate.png", "overlay": "extras_overlay.png",
        "screens": {
            # the Plasma screen IS the ball: nothing behind it, nothing over it
            "plasma": {"plate": "", "overlay": "", "sparks": False},
            # a photograph and a globe are pictures; sparks across them are noise
            "photos": {"sparks": False},
            "globe": {"sparks": False},
        },
    })


# =========================================================================================
# theme.json, and the .orb
# =========================================================================================
def fnv1a(chunks):
    h = 2166136261
    for b in chunks:
        for c in b:
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h or 1


def build_manifest():
    log("theme.json")
    assets = sorted(p.name for p in OUT.iterdir() if p.suffix in (".png", ".bin", ".pcm"))
    # frame order matters to nobody, but a tidy list is easier to read: plates first
    assets.sort(key=lambda n: (not n.endswith(".png"), n))
    assert len(assets) <= 64, f"{len(assets)} assets; the firmware keeps 64 names"
    assert all(len(n) <= 27 for n in assets), "an asset name longer than 27 characters is cut short"
    # assetsHash is the firmware's "has the art changed, must I re-bake" fingerprint. It only
    # has to change when the contents do; FNV-1a over every asset's name and bytes does that.
    h = fnv1a(x for n in assets for x in (n.encode(), (OUT / n).read_bytes()))
    write_json("theme.json", {
        "format": 1,
        "name": "Plasma",
        "author": "Doug Greenhough",
        "license": "CC0 1.0",
        "names": {},
        "assets": assets,
        "assetsHash": h,
        "apps": {"clock": True, "flight": True, "weather": False, "surveillance": False,
                 "headlines": True, "ticker": False},
    })
    (OUT / "_installed").write_text("1\n")


def main():
    fonts = "--no-fonts" not in sys.argv
    OUT.mkdir(parents=True, exist_ok=True)
    for p in OUT.iterdir():
        if p.is_file() and (fonts or not p.name.startswith("font_")):
            p.unlink()
    rng = np.random.default_rng(SEED)
    still, loop, ball = render_discharge()
    base = np.median(np.stack(still), axis=0)    # the standing light: gas, glass, electrode
    pivots = build_clock(still, loop, base)
    build_radar()
    build_news(rng)
    build_menu(still, base, rng)
    build_settings(still, base, rng)
    build_splash(ball, rng)
    build_extras(rng)
    if fonts:
        build_fonts()
    build_styles(pivots)
    build_manifest()
    subprocess.run([sys.executable, str(REPO / "tools" / "pack-orb.py"), str(OUT), str(HERE / "Plasma.orb")], check=True)
    total = sum(p.stat().st_size for p in OUT.iterdir())
    log(f"{len(list(OUT.iterdir()))} files, {total / 1024:.0f} KB in {OUT}")


if __name__ == "__main__":
    main()

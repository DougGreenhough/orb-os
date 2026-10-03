"""Drawing helpers for the Plasma theme: light is added up in floating point, then turned
into the 8-bit RGBA PNGs the Orb's decoders want.

Everything is "light on black". A picture is an (H, W, 3) float array of linear-ish light,
0 = off. Neon is drawn sharp, blurred at several radii and added back (bloom); the result
is clipped, which is what makes a tube's middle burn out to white.

The firmware keeps RGB565 (it masks each channel, it does not round), so plates are
quantised here, with an ordered dither, to exactly the levels the panel will show. Zero
stays zero: an AMOLED pixel that is off is the black this theme is built on.
"""
import math

import numpy as np
from PIL import Image, ImageDraw, ImageFont

W = 466            # the panel
C = 233.0          # its centre


# ---- colour -----------------------------------------------------------------------------
def rgb(hexv):
    return np.array([(hexv >> 16) & 255, (hexv >> 8) & 255, hexv & 255], dtype=np.float32) / 255.0


MAGENTA = 0xFF2FD0
PINK    = 0xFF7AE0
HOT     = 0xFFD9F4     # white-pink, the electrode and the second hand
VIOLET  = 0x8A4DFF
DEEP    = 0x5A2FC8     # violet, further from white
CYAN    = 0x35E0FF
TEXT    = 0xF4EAFF
DIM     = 0x9A7FC0
RULE    = 0x2A1840


# ---- geometry ---------------------------------------------------------------------------
def polar(r, deg, cx=C, cy=C):
    """Clock angle: 0 is twelve o'clock, clockwise."""
    a = math.radians(deg)
    return cx + r * math.sin(a), cy - r * math.cos(a)


def radius_map(w=W, h=W, cx=C, cy=C):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    return np.hypot(x + 0.5 - cx, y + 0.5 - cy)


def smooth(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


# ---- masks: supersampled coverage, 0..1 ---------------------------------------------------
class Mask:
    """An antialiased greyscale stencil, drawn at `ss` times the size and averaged down."""

    def __init__(self, w=W, h=W, ss=4):
        self.w, self.h, self.ss = w, h, ss
        self.im = Image.new("L", (w * ss, h * ss), 0)
        self.d = ImageDraw.Draw(self.im)

    def _v(self, v):
        return int(round(255 * v))

    def dot(self, x, y, r, v=1.0):
        s = self.ss
        self.d.ellipse([(x - r) * s, (y - r) * s, (x + r) * s, (y + r) * s], fill=self._v(v))
        return self

    def line(self, x0, y0, x1, y1, wd, v=1.0):
        s = self.ss
        self.d.line([x0 * s, y0 * s, x1 * s, y1 * s], fill=self._v(v), width=max(1, int(round(wd * s))))
        self.dot(x0, y0, wd / 2, v)
        self.dot(x1, y1, wd / 2, v)
        return self

    def ring(self, cx, cy, r, wd, v=1.0):
        s = self.ss
        o = r + wd / 2
        self.d.ellipse([(cx - o) * s, (cy - o) * s, (cx + o) * s, (cy + o) * s],
                       outline=self._v(v), width=max(1, int(round(wd * s))))
        return self

    def arc(self, cx, cy, r, a0, a1, wd, v=1.0):
        """a0..a1 in clock degrees (0 = twelve, clockwise), round ends."""
        s = self.ss
        o = r + wd / 2
        self.d.arc([(cx - o) * s, (cy - o) * s, (cx + o) * s, (cy + o) * s],
                   a0 - 90, a1 - 90, fill=self._v(v), width=max(1, int(round(wd * s))))
        for a in (a0, a1):
            x, y = polar(r, a, cx, cy)
            self.dot(x, y, wd / 2, v)
        return self

    def rrect(self, x0, y0, x1, y1, rad, wd=None, v=1.0):
        s = self.ss
        box = [x0 * s, y0 * s, x1 * s, y1 * s]
        if wd is None:
            self.d.rounded_rectangle(box, radius=rad * s, fill=self._v(v))
        else:
            self.d.rounded_rectangle(box, radius=rad * s, outline=self._v(v),
                                     width=max(1, int(round(wd * s))))
        return self

    def poly(self, pts, v=1.0):
        s = self.ss
        self.d.polygon([(x * s, y * s) for x, y in pts], fill=self._v(v))
        return self

    def text(self, x, y, s_, font_path, size, anchor="mm", v=1.0, spacing=0):
        s = self.ss
        f = ImageFont.truetype(str(font_path), int(round(size * s)))
        if spacing:
            # letter-spaced: lay the glyphs out one at a time
            widths = [f.getlength(ch) + spacing * s for ch in s_]
            total = sum(widths) - spacing * s
            cx = x * s - total / 2
            for ch, wd in zip(s_, widths):
                self.d.text((cx, y * s), ch, font=f, fill=self._v(v), anchor="l" + anchor[1])
                cx += wd
        else:
            self.d.text((x * s, y * s), s_, font=f, fill=self._v(v), anchor=anchor)
        return self

    def arr(self):
        im = self.im.resize((self.w, self.h), Image.BOX)
        return np.asarray(im, dtype=np.float32) / 255.0


# ---- blur and bloom -------------------------------------------------------------------------
def blur(a, sigma):
    """Gaussian blur of an (H, W) or (H, W, 3) array; what falls off the edge is lost."""
    if sigma <= 0:
        return a
    pad = int(sigma * 3) + 2
    padw = ((pad, pad), (pad, pad)) + ((0, 0),) * (a.ndim - 2)
    ap = np.pad(a, padw, mode="constant")
    h, w = ap.shape[:2]
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.rfftfreq(w)[None, :]
    tf = np.exp(-2 * (math.pi ** 2) * (sigma ** 2) * (fx ** 2 + fy ** 2)).astype(np.float32)
    if a.ndim == 3:
        tf = tf[..., None]
    out = np.fft.irfft2(np.fft.rfft2(ap, axes=(0, 1)) * tf, s=(h, w), axes=(0, 1))
    return np.maximum(out[pad:-pad, pad:-pad], 0).astype(np.float32)


# Bloom recipe: (sigma px, peak brightness a thin line's halo reaches at that radius).
BLOOM = ((1.3, 0.55), (3.5, 0.36), (8.0, 0.20), (18.0, 0.10))
BLOOM_TIGHT = ((1.2, 0.5), (3.0, 0.28), (6.0, 0.12))
BLOOM_WIDE = ((1.3, 0.55), (3.5, 0.38), (9.0, 0.24), (22.0, 0.14), (48.0, 0.07))


def neon(mask, color, lw=3.0, core=1.0, hot=0.6, bloom=BLOOM, gain=1.0):
    """A stencil lit as a neon tube: a hot middle, and its own light bled out round it.

    `lw` is the stencil's stroke width in px; a blurred thin line loses height in
    proportion to sigma/lw, so each bloom pass is scaled back up to the peak the recipe
    names. That keeps a 2 px tube and an 8 px tube glowing alike.
    """
    c = rgb(color) if isinstance(color, int) else np.asarray(color, dtype=np.float32)
    hotc = c + (1 - c) * hot
    light = mask[..., None] * hotc * core
    for sigma, peak in bloom:
        k = max(1.0, sigma * 2.5066 / max(lw, 0.5))
        light = light + (blur(mask, sigma) * (peak * k * gain))[..., None] * c
    return light


def glow_disc(shape, cx, cy, parts):
    """Round light: parts = [(colour, radius, strength), ...] as Gaussians."""
    h, w = shape
    r = radius_map(w, h, cx, cy)
    light = np.zeros((h, w, 3), np.float32)
    for col, rad, k in parts:
        light += (np.exp(-(r / rad) ** 2) * k)[..., None] * rgb(col)
    return light


def spark(light, x, y, deg, length, color=HOT, size=1.0, rng=None):
    """A flying spark: a white point, a short streak behind it, and a little glow.

    `deg` is the way it is travelling (clock degrees); the streak trails the other way.
    """
    h, w = light.shape[:2]
    m = Mask(w, h)
    steps = 6
    for i in range(steps):
        t0, t1 = i / steps, (i + 1) / steps
        x0, y0 = polar(length * t0, deg + 180, x, y)
        x1, y1 = polar(length * t1, deg + 180, x, y)
        m.line(x0, y0, x1, y1, 1.3 * size * (1 - 0.6 * t0), v=(1 - t0) ** 1.6 * 0.8)
    m.dot(x, y, 1.5 * size)
    a = m.arr()
    light += neon(a, color, lw=2.0 * size, hot=0.5, bloom=((1.5, 0.7), (4.0, 0.35), (9.0, 0.14)))
    head = Mask(w, h).dot(x, y, 1.1 * size).arr()
    light += head[..., None] * 1.0
    return light


# ---- glass ------------------------------------------------------------------------------
def glass_reflection(strength=1.0, w=W, cx=C, cy=C, R=233.0):
    """The room in the glass. A dome of glass shows a long soft highlight that follows its
    own edge on the side the light comes from (up and to the left), a shorter, tighter one
    inside that, a hairline on the rim itself, and a faint violet bounce on the far side.
    Returns light (H, W, 3); most of it is zero, and the middle is left alone for reading."""
    y, x = np.mgrid[0:w, 0:w].astype(np.float32)
    x = (x + 0.5 - cx) / R
    y = (y + 0.5 - cy) / R
    r = np.hypot(x, y)
    inside = 1 - smooth(0.985, 1.0, r)
    ang = np.degrees(np.arctan2(x, -y))          # clock degrees, 0 = twelve

    def span(mid, half, power=2.0):
        d = np.abs((ang - mid + 180) % 360 - 180)
        return np.clip(np.cos(np.clip(d / half, 0, 1) * math.pi / 2), 0, 1) ** power

    def band(at, width):
        return np.exp(-((r - at) / width) ** 2)

    # asymmetric: a firm outer edge, a slow fade towards the middle
    main = np.where(r > 0.875, band(0.875, 0.035), band(0.875, 0.11)) * span(-48, 58, 1.6)
    second = band(0.74, 0.022) * span(-62, 20, 1.5)
    rimline = band(0.972, 0.010) * (0.18 + 0.82 * span(-45, 100, 1.2))
    bounce = np.where(r > 0.90, band(0.90, 0.03), band(0.90, 0.07)) * span(132, 42, 1.8)

    white = np.array([0.92, 0.90, 1.0], np.float32)
    violet = rgb(0xA98BFF)
    light = (main * 0.115 + second * 0.07 + rimline * 0.26)[..., None] * white
    light += (bounce * 0.07)[..., None] * violet
    return light * (inside * strength)[..., None]


def vignette_alpha(start=196.0, end=233.0, strength=0.4, w=W):
    return smooth(start, end, radius_map(w, w)) * strength


BLACK_BELOW = 0.016      # light dimmer than this (4/255) is switched off in a plate


# ---- out to PNG -------------------------------------------------------------------------
_BAYER4 = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.float32) / 16.0


def _bayer(h, w):
    return np.tile(_BAYER4, (h // 4 + 1, w // 4 + 1))[:h, :w]


def quant565(light, floor=0.010):
    """Float light -> uint8 RGB already sitting on RGB565 levels, ordered-dithered.
    Anything below `floor` is switched off rather than left as a speckle."""
    a = np.clip(light, 0, 1)
    a = np.where(a < floor, 0.0, a)
    h, w = a.shape[:2]
    t = _bayer(h, w)
    r = np.minimum(np.floor(a[..., 0] * 31 + t), 31).astype(np.uint8)
    g = np.minimum(np.floor(a[..., 1] * 63 + t), 63).astype(np.uint8)
    b = np.minimum(np.floor(a[..., 2] * 31 + t), 31).astype(np.uint8)
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1)


def circle_clip(light, R=233.0):
    """Nothing outside the round panel: it is never seen, and black compresses to nothing."""
    h, w = light.shape[:2]
    keep = (radius_map(w, h, w / 2, h / 2) <= R + 0.5).astype(np.float32)
    return light * keep[..., None]


def save_plate(path, light, clip=True):
    """An opaque full-screen picture (plates, splash): 8-bit RGBA, alpha 255."""
    if clip:
        light = circle_clip(light)
    # Black is off. The far tail of a glow is a few stray dithered pixels across a large
    # area, which costs the panel its black and buys nothing the eye can see, so the last
    # few levels are faded out to nothing rather than kept.
    peak = np.clip(light, 0, None).max(axis=-1)
    light = light * smooth(BLACK_BELOW, BLACK_BELOW * 2.8, peak)[..., None]
    q = quant565(light)
    a = np.full(q.shape[:2] + (1,), 255, np.uint8)
    Image.fromarray(np.concatenate([q, a], axis=-1), "RGBA").save(path, optimize=True, compress_level=9)


def save_sprite(path, light, shade=None, min_alpha=0.0):
    """Light as a see-through layer (hands, glows, glass): alpha is how bright, colour is
    which. Laid over black it gives back the light exactly. `shade` (H, W) is black to lay
    UNDER the light, 0..1, for a vignette or a panel that darkens what is behind it."""
    lt = np.clip(light, 0, None)
    if min_alpha > 0:
        # take the floor off the top of everything, so the edge fades out instead of stepping
        peak = lt.max(axis=-1)
        lt = lt * (np.clip(peak - min_alpha, 0, None) / np.maximum(peak, 1e-6))[..., None] / (1 - min_alpha)
    al = np.clip(lt.max(axis=-1), 0, 1)
    col = np.where(al[..., None] > 1e-4, np.clip(lt / np.maximum(al[..., None], 1e-4), 0, 1), 0)
    if shade is not None:
        sh = np.clip(shade, 0, 1)
        out_a = al + sh * (1 - al)
        col = np.where(out_a[..., None] > 1e-4, col * (al / np.maximum(out_a, 1e-4))[..., None], 0)
        al = out_a
    h, w = al.shape
    a8 = np.minimum(np.floor(al * 255 + _bayer(h, w)), 255)
    a8 = np.where(al < 0.008, 0, a8).astype(np.uint8)
    q = quant565(col, floor=0.0)
    q = np.where(a8[..., None] == 0, 0, q).astype(np.uint8)
    Image.fromarray(np.concatenate([q, a8[..., None]], axis=-1), "RGBA").save(path, optimize=True, compress_level=9)


def load_ppm(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def resize(a, size, method=Image.BICUBIC):
    """Resize an (H, W, 3) float array through 8-bit; good enough for texture."""
    im = Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")
    return np.asarray(im.resize((size, size), method), dtype=np.float32) / 255.0


def place(canvas, patch, cx, cy):
    """Add `patch` into `canvas` with its centre at (cx, cy), clipped."""
    h, w = patch.shape[:2]
    x0, y0 = int(round(cx - w / 2)), int(round(cy - h / 2))
    X0, Y0 = max(0, x0), max(0, y0)
    X1, Y1 = min(canvas.shape[1], x0 + w), min(canvas.shape[0], y0 + h)
    if X1 > X0 and Y1 > Y0:
        canvas[Y0:Y1, X0:X1] += patch[Y0 - y0:Y1 - y0, X0 - x0:X1 - x0]
    return canvas

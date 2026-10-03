#!/usr/bin/env python3
"""Bake the Globe screen's Earth map into flash: src/globe_texture.h + src/globe_texture.c.

Source: the three.js example planet textures that Doug's homepage globe uses (and that
~/Claude/planets vendors as assets/tex/earth.jpg, byte-identical):

    earth_atmos_2048.jpg     2048x1024 colour map
    earth_clouds_1024.png    1024x512  cloud layer (alpha)
    earth_specular_2048.jpg  2048x1024 ocean mask (white = water)

These are from the three.js repository (MIT licence), and are themselves derived from
NASA Visible Earth / Blue Marble imagery (public domain). See ~/Claude/planets/CREDITS.md.

What comes out (all const, so it stays in flash):

    globe_tex[256][512]    RGB565, equirectangular, row 0 = 90N, column 0 = 180W
    globe_ocean[256][64]   1 bit per texel, bit (u & 7) of byte u >> 3: 1 = water
    globe_coast[256][512]  8 bits per texel: how far the nearest coast is, signed

The coast map is for the Globe's drawn styles (a theme's phosphor outline, an engraved
atlas, a neon globe), which need a coastline rather than a photograph. It is a signed
distance field: 128 is the shore, above it is land, below it water, COAST_SCALE steps to a
texel-height (0.703 degrees) of true distance on the sphere (east-west distances are
shortened by cos(latitude), so a line drawn at a fixed distance is as thick in Greenland
as on the equator), clamped at about 7.5 degrees either way. Sampled bilinearly it gives
a smooth shoreline at any zoom, and one table lookup on it gives the line, a glow that
falls away from it, the water-lining of an old chart and the land's fill. It is measured
on the 1024-wide water mask, twice the output's resolution. 128 KB.

Why 512x256: the screen renders the globe at half resolution, 96 px radius, so the
centre of the disc shows ~1.36 render px per degree of longitude. 512 columns is 1.42
per degree: one texel per rendered pixel where the globe is sharpest, and more than
enough towards the limb, where everything is foreshortened. 1024x512 would cost 1 MB
for detail the 233 px frame cannot show. 256 KB + 16 KB here, and 128 KB of coast map.

The texture is pre-treated so that the device can shade it with a single multiply and
still look like the homepage. The homepage (three.js r165) loads the map without a
colour space, so its bytes are used as LINEAR values, lit, then sRGB-encoded on output:
out = srgb(t * k). With a power-law approximation that is srgb(t) * srgb(k), so this
bakes srgb(t) (and blends the clouds in that space, as the page's blending does) and
the device multiplies by a per-pixel shade that already holds srgb(k).

Usage:
    python3 tools/bake_globe_texture.py [texture dir]   (default ~/Claude/homepage/assets/textures)
Needs Pillow and numpy (the coast map takes about half a minute).
"""
import math
import os
import sys

import numpy as np
from PIL import Image

W, H = 512, 256
CLOUD_OPACITY = 0.38          # the homepage's cloud mesh opacity
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_H = os.path.join(HERE, '..', 'src', 'globe_texture.h')
OUT_C = os.path.join(HERE, '..', 'src', 'globe_texture.c')

BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]

COAST_SCALE = 12              # steps per texel-height of distance; 128 = the shore
COAST_SS = 2                  # measured on a mask this many times the output's resolution


def coast_field(spec_full):
    """Signed distance to the shore for every output texel: + on land, - on water."""
    w, h = W * COAST_SS, H * COAST_SS
    water = np.asarray(spec_full.resize((w, h), Image.BOX)) >= 128
    reach = 127.0 / COAST_SCALE * COAST_SS + 1          # as far as a byte can say, in mask px
    lat = (0.5 - (np.arange(h) + 0.5) / h) * math.pi
    cosl = np.maximum(np.cos(lat), 0.12)                 # past 83 degrees a row is nearly a point

    # The mask has every river and pond in it, and each would be a ring of coastline. Keep
    # only water with some body to it: wear the water back by a pixel all round (a degree
    # across survives), then let what is left grow back, inside the original mask, a few
    # steps. Seas, straits and the big lakes come back whole; rivers and specks do not.
    kx = np.minimum(np.rint(1.0 / cosl).astype(int), 12)

    def spread(m, grow):
        out = m.copy()
        up = np.vstack([m[:1], m[:-1]])
        down = np.vstack([m[1:], m[-1:]])
        for dx in range(-int(kx.max()), int(kx.max()) + 1):
            ok = (kx >= abs(dx))[:, None]
            for layer in (m, up, down):
                sh = np.roll(layer, dx, axis=1)
                out = (out | (sh & ok)) if grow else (out & (sh | ~ok))
        return out

    body = spread(water, False)
    for _ in range(4):
        body = spread(body, True) & water
    water = body
    best = np.full((h, w), reach, dtype=np.float32)
    ry = int(math.ceil(reach))
    rows = np.arange(h)
    for dy in range(-ry, ry + 1):
        src = np.clip(rows + dy, 0, h - 1)
        there = water[src]                               # the row dy away (clamped at the poles)
        rem = reach * reach - dy * dy
        if rem <= 0:
            continue
        # how far east or west is still within reach, per row
        maxdx = np.minimum(np.floor(math.sqrt(rem) / cosl).astype(int), w // 2)
        for dx in range(0, int(maxdx.max()) + 1):
            ok = maxdx >= dx
            d = np.sqrt((dx * cosl) ** 2 + dy * dy).astype(np.float32)
            for sgn in ((1, -1) if dx else (1,)):
                other = np.roll(there, -sgn * dx, axis=1) != water
                cand = np.where(other & ok[:, None], d[:, None], reach)
                np.minimum(best, cand, out=best)
    # to the nearest pixel of the other kind is half a pixel past the shore itself
    dist = np.maximum(best - 0.5, 0) / COAST_SS
    signed = np.where(water, -dist, dist)
    out = signed.reshape(H, COAST_SS, W, COAST_SS).mean(axis=(1, 3))
    return np.clip(np.rint(128 + out * COAST_SCALE), 0, 255).astype(np.uint8)


def srgb(x):
    x = max(0.0, min(1.0, x))
    return 12.92 * x if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/Claude/homepage/assets/textures')
    atmos = Image.open(os.path.join(src, 'earth_atmos_2048.jpg')).convert('RGB')
    clouds = Image.open(os.path.join(src, 'earth_clouds_1024.png')).convert('RGBA')
    spec = Image.open(os.path.join(src, 'earth_specular_2048.jpg')).convert('L')

    coast = coast_field(spec)

    atmos = atmos.resize((W, H), Image.LANCZOS)
    clouds = clouds.resize((W, H), Image.LANCZOS)
    spec = spec.resize((W, H), Image.BOX)

    enc = [srgb(i / 255.0) for i in range(256)]
    A = atmos.load()
    C = clouds.load()
    S = spec.load()

    tex = []
    ocean = bytearray(W * H // 8)
    for v in range(H):
        for u in range(W):
            r, g, b = A[u, v]
            cr, cg, cb, ca = C[u, v]
            a = CLOUD_OPACITY * ca / 255.0
            rgb = [(1 - a) * enc[r] + a * enc[cr],
                   (1 - a) * enc[g] + a * enc[cg],
                   (1 - a) * enc[b] + a * enc[cb]]
            # ordered dither into 5/6/5 bits: the oceans are wide flat gradients
            d = (BAYER4[v & 3][u & 3] + 0.5) / 16.0
            R = min(31, int(rgb[0] * 31 + d))
            G = min(63, int(rgb[1] * 63 + d))
            B = min(31, int(rgb[2] * 31 + d))
            tex.append((R << 11) | (G << 5) | B)
            if S[u, v] >= 128:
                ocean[(v * W + u) >> 3] |= 1 << (u & 7)

    with open(OUT_H, 'w') as f:
        f.write('#pragma once\n')
        f.write('// Generated by tools/bake_globe_texture.py. Do not edit.\n')
        f.write('// Earth map for the Globe screen: three.js example textures (MIT), from NASA\n')
        f.write('// Visible Earth / Blue Marble (public domain). See the script for the treatment.\n')
        f.write('#include <stdint.h>\n\n')
        f.write('#define GLOBE_TEX_W %d\n#define GLOBE_TEX_H %d\n\n' % (W, H))
        f.write('#ifdef __cplusplus\nextern "C" {\n#endif\n')
        f.write('extern const uint16_t globe_tex[GLOBE_TEX_W * GLOBE_TEX_H];     // RGB565, row 0 = 90N, col 0 = 180W\n')
        f.write('extern const uint8_t  globe_ocean[GLOBE_TEX_W * GLOBE_TEX_H / 8]; // 1 = water; bit (u & 7) of byte (v*W+u) >> 3\n')
        f.write('// Signed distance to the shore: 128 = the shore, more = land, less = water,\n')
        f.write('// GLOBE_COAST_SCALE steps per texel-height (180/GLOBE_TEX_H degrees) of true distance.\n')
        f.write('#define GLOBE_COAST_SCALE %d\n' % COAST_SCALE)
        f.write('extern const uint8_t  globe_coast[GLOBE_TEX_W * GLOBE_TEX_H];\n')
        f.write('#ifdef __cplusplus\n}\n#endif\n')

    with open(OUT_C, 'w') as f:
        f.write('// Generated by tools/bake_globe_texture.py. Do not edit.\n')
        f.write('// Source: three.js examples/textures/planets earth_atmos_2048.jpg, earth_clouds_1024.png,\n')
        f.write('// earth_specular_2048.jpg (MIT licence), derived from NASA Visible Earth / Blue Marble\n')
        f.write('// (public domain). %dx%d RGB565, sRGB-treated, clouds blended at %.2f.\n' % (W, H, CLOUD_OPACITY))
        f.write('#include "globe_texture.h"\n\n')
        f.write('const uint16_t globe_tex[GLOBE_TEX_W * GLOBE_TEX_H] = {\n')
        for i in range(0, len(tex), 16):
            f.write(','.join('0x%04x' % t for t in tex[i:i + 16]) + ',\n')
        f.write('};\n\n')
        f.write('const uint8_t globe_ocean[GLOBE_TEX_W * GLOBE_TEX_H / 8] = {\n')
        for i in range(0, len(ocean), 32):
            f.write(','.join('0x%02x' % b for b in ocean[i:i + 32]) + ',\n')
        f.write('};\n\n')
        f.write('const uint8_t globe_coast[GLOBE_TEX_W * GLOBE_TEX_H] = {\n')
        flat = coast.reshape(-1)
        for i in range(0, len(flat), 32):
            f.write(','.join('0x%02x' % b for b in flat[i:i + 32]) + ',\n')
        f.write('};\n')
    print('wrote %s and %s: %d KB texture + %d KB ocean mask + %d KB coast map' %
          (OUT_H, OUT_C, len(tex) * 2 // 1024, len(ocean) // 1024, coast.size // 1024))


if __name__ == '__main__':
    main()

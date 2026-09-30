// The Plasma screen's renderer. See plasma_render.h for the shape of it.
//
// The looks are the web page's, pass for pass: four strokes per channel (a broad haze, a
// wide glow, a body and a warm thread down the middle), colour running cool along the
// body and warm over the last stretch before the glass, a flickering brush of fibres and
// a glowing tail and spot where each foot lands, a bright point where each trunk leaves
// the electrode, the electrode itself as an opaque lit ball with a ring of glow round it,
// the gas as a dark haze of its own colour, and the glass as a brighter rim and a window
// reflected in the top left. Widths, alphas and colours are the page's numbers; the page's
// line-width unit is WF render pixels here.
#include "plasma_render.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
static void *big_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void  big_free(void *p) { heap_caps_free(p); }
#else
static void *big_alloc(size_t n) { return malloc(n); }
static void  big_free(void *p) { free(p); }
#endif

namespace plasma {
namespace {

// ---- the camera: the page's, with the glass's silhouette just inside the panel's edge --
constexpr float D = 3.0f;
constexpr float LIMB = 2.8284271f;           // sqrt(D*D - 1)
constexpr float C = 116.5f;                  // centre of the render grid
constexpr float RL = 115.5f;                 // the glass's silhouette, render px
constexpr float K = RL * LIMB;
constexpr float RD = 116.5f;                 // the panel's own edge, render px
constexpr float WF = 0.42f;                  // one web line-width unit, in render px
constexpr float IREF = 0.09f;

// ---- how it reads as light ---------------------------------------------------------
// The page lays a blurred copy of the finished frame back over it twice (at 0.52 and
// 0.45), which roughly doubles everything broad and adds a halo to everything fine. Here
// that is split three ways: strokes are drawn a little brighter (GAIN), a share of their
// light also goes to the bloom grid (BLOOM), and the gas, glass and electrode, which are
// broad and smooth, are simply drawn as bright as the bloom would have made them (BROAD).
constexpr float GAIN = 1.35f;
constexpr float BLOOM = 0.7f;
constexpr float BROAD = 1.9f;
// The 12.5-unit glow round every channel: drawn as a stroke (true), or handed to the bloom
// grid (false), which roughly halves the pixels the rasteriser touches. The first knob to
// turn if frames run long; the difference is a slightly softer halo close to each line.
constexpr bool  GLOW_SHARP = false;
constexpr int   NL = 40;                     // entries in a stroke's profile table

// Colour along a channel: the body runs cool, the end warm (page: BODY_SHIFT, END_SHIFT).
constexpr float BODY_SHIFT = -52.0f, END_SHIFT = 34.0f, WARM_FROM = 0.62f;
constexpr float SPREAD = 20.0f;              // how far strands' hues are spread (fixed mode)

struct RGB { float r, g, b; };

RGB hsl(float h, float s, float l) {
    h = fmodf(h, 360.0f); if (h < 0) h += 360.0f;
    s *= 0.01f; l *= 0.01f;
    const float c = (1 - fabsf(2 * l - 1)) * s;
    const float hp = h / 60.0f;
    const float x = c * (1 - fabsf(fmodf(hp, 2.0f) - 1));
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; } else if (hp < 2) { r = x; g = c; } else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; } else if (hp < 5) { r = x; b = c; } else { r = c; b = x; }
    const float m = l - c / 2;
    return { r + m, g + m, b + m };
}

float warmth(float u) {
    float t = (u - WARM_FROM) / (1 - WARM_FROM);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}
RGB shade(float base, float u, float sat, float li) {
    const float t = warmth(u);
    return hsl(base + BODY_SHIFT + (END_SHIFT - BODY_SHIFT) * t, sat * (0.72f + 0.26f * t), li);
}

inline void proj(float x, float y, float z, float &sx, float &sy) {
    const float q = K / (D - z);
    sx = C + x * q;
    sy = C - y * q;
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- a stroke's profile: light added at each distance from its centre line ----------
// Indexed by distance squared, so a pixel costs no square root. Values are 12.4 fixed.
struct Pass { float w, a; RGB c; };
struct Lut {
    float rm2, scale;
    uint16_t v[NL][3];
    float e[3];                // light per unit length, for the bloom
};

// Coverage of a pixel by a stroke of width w at distance d from its centre: full inside,
// a one-pixel ramp at the edge, and never more than the width itself for hairlines.
inline float cov(float w, float d) {
    float c = w * 0.5f + 0.5f - d;
    if (c > 1) c = 1;
    if (c > w) c = w;
    return c > 0 ? c : 0;
}

void build_lut(Lut &L, const Pass *p, int n) {
    float rm = 0;
    L.e[0] = L.e[1] = L.e[2] = 0;
    for (int k = 0; k < n; ++k) {
        if (p[k].w * 0.5f + 0.5f > rm) rm = p[k].w * 0.5f + 0.5f;
        const float s = p[k].a * p[k].w * 255.0f;
        L.e[0] += s * p[k].c.r; L.e[1] += s * p[k].c.g; L.e[2] += s * p[k].c.b;
    }
    L.rm2 = rm * rm;
    L.scale = NL / L.rm2;
    for (int i = 0; i < NL; ++i) {
        const float d = sqrtf((i + 0.5f) / L.scale);
        float r = 0, g = 0, b = 0;
        for (int k = 0; k < n; ++k) {
            const float c = cov(p[k].w, d) * p[k].a * 255.0f * GAIN * 16.0f;
            r += c * p[k].c.r; g += c * p[k].c.g; b += c * p[k].c.b;
        }
        L.v[i][0] = (uint16_t)fminf(65535.0f, r);
        L.v[i][1] = (uint16_t)fminf(65535.0f, g);
        L.v[i][2] = (uint16_t)fminf(65535.0f, b);
    }
}

inline void add_px(uint8_t *p, const uint16_t *v, unsigned dith) {
    unsigned r = p[0] + ((v[0] + dith) >> 4); p[0] = (uint8_t)(r > 255 ? 255 : r);
    unsigned g = p[1] + ((v[1] + dith) >> 4); p[1] = (uint8_t)(g > 255 ? 255 : g);
    unsigned b = p[2] + ((v[2] + dith) >> 4); p[2] = (uint8_t)(b > 255 ? 255 : b);
}

// 4x4 ordered thresholds, 0..15: faint light below one step still lands on some pixels.
const uint8_t BAYER16[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

// Light into the bloom grid: `e` per render px along the stroke, spread bilinearly.
void glow_line(Render &R, float x0, float y0, float x1, float y1, const float *e, float gain) {
    const float ex = x1 - x0, ey = y1 - y0;
    const float len = sqrtf(ex * ex + ey * ey);
    const int n = len > 2.0f ? (int)(len * 0.5f) + 1 : 1;
    const float w = gain * (len > 0.25f ? len : 0.25f) / n / (LCELL * LCELL);
    for (int s = 0; s < n; ++s) {
        const float t = (s + 0.5f) / n;
        const float gx = (x0 + ex * t) / LCELL - 0.5f, gy = (y0 + ey * t) / LCELL - 0.5f;
        int ix = (int)floorf(gx), iy = (int)floorf(gy);
        const float fx = gx - ix, fy = gy - iy;
        for (int q = 0; q < 4; ++q) {
            const int cx = ix + (q & 1), cy = iy + (q >> 1);
            if (cx < 0 || cy < 0 || cx >= LG || cy >= LG) continue;
            const float ww = w * ((q & 1) ? fx : 1 - fx) * ((q >> 1) ? fy : 1 - fy);
            float *g = &R.glow[(cy * LG + cx) * 3];
            g[0] += ww * e[0]; g[1] += ww * e[1]; g[2] += ww * e[2];
        }
    }
}
void glow_point(Render &R, float x, float y, const float *e) {
    const float gx = x / LCELL - 0.5f, gy = y / LCELL - 0.5f;
    int ix = (int)floorf(gx), iy = (int)floorf(gy);
    const float fx = gx - ix, fy = gy - iy;
    for (int q = 0; q < 4; ++q) {
        const int cx = ix + (q & 1), cy = iy + (q >> 1);
        if (cx < 0 || cy < 0 || cx >= LG || cy >= LG) continue;
        const float ww = ((q & 1) ? fx : 1 - fx) * ((q >> 1) ? fy : 1 - fy) / (LCELL * LCELL);
        float *g = &R.glow[(cy * LG + cx) * 3];
        g[0] += ww * e[0]; g[1] += ww * e[1]; g[2] += ww * e[2];
    }
}

// One soft capsule into the accumulator, a scanline at a time. Caps only where a stroke
// really ends: at a joint the two segments meet at the perpendicular, so nothing doubles.
void seg(Render &R, float x0, float y0, float x1, float y1, const Lut &L, bool cap0, bool cap1) {
    R.segs++;
    const float ex = x1 - x0, ey = y1 - y0;
    const float len2 = ex * ex + ey * ey;
    const float il2 = len2 > 1e-6f ? 1.0f / len2 : 0.0f;
    const float rm2 = L.rm2, rm = sqrtf(rm2);
    int ya = (int)floorf(fminf(y0, y1) - rm), yb = (int)ceilf(fmaxf(y0, y1) + rm);
    if (ya < 0) ya = 0;
    if (yb > RW - 1) yb = RW - 1;
    const float iey = fabsf(ey) > 1e-4f ? 1.0f / ey : 0.0f;
    const float iex = fabsf(ex) > 1e-4f ? 1.0f / ex : 0.0f;
    const float bandK = iey != 0 ? rm * sqrtf(len2) * fabsf(iey) : 0.0f;
    for (int y = ya; y <= yb; ++y) {
        const float py = y + 0.5f;
        float ta = 0, tb = 1;
        if (iey != 0) {
            ta = clampf((py - rm - y0) * iey, 0, 1);
            tb = clampf((py + rm - y0) * iey, 0, 1);
        }
        float xa = x0 + ex * ta, xb = x0 + ex * tb;
        if (xa > xb) { const float t = xa; xa = xb; xb = t; }
        xa -= rm; xb += rm;
        if (bandK > 0) {
            // and within rm of the line itself: a steep stroke's row is only this wide
            const float xc = x0 + ex * (py - y0) * iey;
            if (xc - bandK > xa) xa = xc - bandK;
            if (xc + bandK < xb) xb = xc + bandK;
        }
        if (!cap0 && !cap1 && iex != 0) {
            // and between the two perpendiculars that end it (joints are cut there)
            const float dyy = (py - y0) * ey;
            float u0 = x0 - dyy * iex, u1 = x0 + (len2 - dyy) * iex;
            if (u0 > u1) { const float t = u0; u0 = u1; u1 = t; }
            if (u0 - 0.5f > xa) xa = u0 - 0.5f;
            if (u1 + 0.5f < xb) xb = u1 + 0.5f;
        }
        int xs = (int)floorf(xa), xe = (int)ceilf(xb);
        if (xs < R.chordL[y]) xs = R.chordL[y];
        if (xe > R.chordR[y]) xe = R.chordR[y];
        if (xs > xe) continue;
        uint8_t *p = R.acc + (y * RW + xs) * 3;
        const float dy = py - y0;
        float dx = xs + 0.5f - x0;
        const uint8_t *bay = &BAYER16[(y & 3) * 4];
        for (int x = xs; x <= xe; ++x, dx += 1.0f, p += 3) {
            float t = (dx * ex + dy * ey) * il2;
            if (t < 0) { if (!cap0) continue; t = 0; }
            else if (t >= 1) { if (!cap1) continue; t = 1; }
            const float qx = dx - t * ex, qy = dy - t * ey;
            const float d2 = qx * qx + qy * qy;
            if (d2 >= rm2) continue;
            add_px(p, L.v[(int)(d2 * L.scale)], bay[x & 3]);
        }
    }
}

// A round or foreshortened spot, lying on the glass: `lean` squashes it along the radius
// (the page's onGlass), 1 for a plain disc. The table is over r^2/R^2.
struct Spot { uint16_t v[32][3]; float e[3]; };
void spot(Render &R, float x, float y, float rad, float lean, const Spot &S) {
    const float rx = x - C, ry = y - C;
    const float rl = sqrtf(rx * rx + ry * ry);
    const float cr = rl > 1e-3f ? rx / rl : 1.0f, sr = rl > 1e-3f ? ry / rl : 0.0f;
    const float il = 1.0f / lean, iR2 = 1.0f / (rad * rad);
    int ya = (int)floorf(y - rad), yb = (int)ceilf(y + rad);
    if (ya < 0) ya = 0;
    if (yb > RW - 1) yb = RW - 1;
    for (int yy = ya; yy <= yb; ++yy) {
        int xs = (int)floorf(x - rad), xe = (int)ceilf(x + rad);
        if (xs < R.chordL[yy]) xs = R.chordL[yy];
        if (xe > R.chordR[yy]) xe = R.chordR[yy];
        const float dy = yy + 0.5f - y;
        uint8_t *p = R.acc + (yy * RW + xs) * 3;
        const uint8_t *bay = &BAYER16[(yy & 3) * 4];
        for (int xx = xs; xx <= xe; ++xx, p += 3) {
            const float dx = xx + 0.5f - x;
            const float u = (dx * cr + dy * sr) * il, v = -dx * sr + dy * cr;
            const float rr = (u * u + v * v) * iR2;
            if (rr >= 1.0f) continue;
            add_px(p, S.v[(int)(rr * 32.0f)], bay[xx & 3]);
        }
    }
    // its light into the bloom: the table's mean over the disc's area (uniform in r^2)
    const float area = 3.14159f * rad * rad * lean;
    float e[3] = { S.e[0] * area * BLOOM, S.e[1] * area * BLOOM, S.e[2] * area * BLOOM };
    glow_point(R, x, y, e);
}

// Build a spot's table from colour stops (position 0..1 along the radius, premultiplied).
void build_spot(Spot &S, const float *pos, const RGB *col, const float *alpha, int n, float gain) {
    S.e[0] = S.e[1] = S.e[2] = 0;
    for (int i = 0; i < 32; ++i) {
        const float t = sqrtf((i + 0.5f) / 32.0f);
        int k = 0;
        while (k < n - 2 && t > pos[k + 1]) ++k;
        const float f = clampf((t - pos[k]) / (pos[k + 1] - pos[k]), 0, 1);
        const float a0 = alpha[k], a1 = alpha[k + 1];
        const float r = col[k].r * a0 + (col[k + 1].r * a1 - col[k].r * a0) * f;
        const float g = col[k].g * a0 + (col[k + 1].g * a1 - col[k].g * a0) * f;
        const float b = col[k].b * a0 + (col[k + 1].b * a1 - col[k].b * a0) * f;
        const float s = 255.0f * gain;
        S.v[i][0] = (uint16_t)fminf(65535.0f, r * s * 16.0f);
        S.v[i][1] = (uint16_t)fminf(65535.0f, g * s * 16.0f);
        S.v[i][2] = (uint16_t)fminf(65535.0f, b * s * 16.0f);
        S.e[0] += r * s / 32.0f; S.e[1] += g * s / 32.0f; S.e[2] += b * s / 32.0f;
    }
}

// ---- the pieces of one channel ----------------------------------------------------------

inline int level(float I) { return (int)lroundf(log2f(I / IREF + 0.35f) * 2.0f); }

// Screen points of a channel, subdivided once (Catmull-Rom) so its bends are curves.
struct Poly { float x[NN * 2], y[NN * 2]; };

void channel_points(const Engine &e, const Fil &f, float *sx, float *sy) {
    for (int j = 0; j < NN; ++j) {
        const float r = e.radii[j];
        proj(f.d[j * 3] * r, f.d[j * 3 + 1] * r, f.d[j * 3 + 2] * r, sx[j], sy[j]);
    }
}

void channel(Render &R, const Engine &e, const Fil &f) {
    float sx[NN], sy[NN];
    channel_points(e, f, sx, sy);
    const float base = R.hue + SPREAD * f.hue;
    const float dz = 0.42f + 0.58f * (f.d[(NN - 1) * 3 + 2] + 1) * 0.5f;
    const int start = f.par >= 0 ? f.k : 0;
    int a = start;
    while (a < NN - 1) {
        const int lvl = level(f.segI[a + 1]);
        int b = a + 1;
        while (b < NN - 1 && level(f.segI[b + 1]) == lvl) ++b;
        float I = 0;
        for (int j = a + 1; j <= b; ++j) I += f.segI[j];
        const float c = I / (b - a) / IREF;
        const float al = f.life * dz * fminf(2.2f, powf(c, 0.6f));
        if (al >= 0.005f) {
            const float wf = WF * dz * fminf(2.4f, 0.55f + 0.45f * sqrtf(c));
            Lut L;
            float haze[3];
            bool built = false;
            for (int j = a; j < b; ++j) {
                const float u = (j + 0.5f) / (NN - 1);
                if (!built || u > WARM_FROM - 0.5f / (NN - 1)) {
                    // the four passes: haze (bloom only), glow, body, core
                    const Pass p[3] = {
                        { 5.2f * wf, fminf(1.0f, 0.200f * al), shade(base, u, 100, 72) },
                        { 2.0f * wf, fminf(1.0f, 0.520f * al), shade(base, u, 94, 90) },
                        { 12.5f * wf, fminf(1.0f, 0.075f * al), shade(base, u, 100, 60) },
                    };
                    build_lut(L, p, GLOW_SHARP ? 3 : 2);
                    const RGB hc = shade(base, u, 100, 50);
                    const float hs = 25.0f * wf * fminf(1.0f, 0.03f * al) * 255.0f;
                    haze[0] = hc.r * hs + L.e[0] * BLOOM; haze[1] = hc.g * hs + L.e[1] * BLOOM; haze[2] = hc.b * hs + L.e[2] * BLOOM;
                    if (!GLOW_SHARP) {
                        // the glow pass goes to the bloom grid whole, at full strength
                        const float gs = p[2].w * p[2].a * 255.0f * GAIN;
                        haze[0] += p[2].c.r * gs; haze[1] += p[2].c.g * gs; haze[2] += p[2].c.b * gs;
                    }
                    built = true;
                }
                // Catmull-Rom midpoint, from the neighbours either side
                const int j0 = j > 0 ? j - 1 : 0, j3 = j + 2 < NN ? j + 2 : NN - 1;
                const float mx = (-sx[j0] + 9 * sx[j] + 9 * sx[j + 1] - sx[j3]) * (1.0f / 16);
                const float my = (-sy[j0] + 9 * sy[j] + 9 * sy[j + 1] - sy[j3]) * (1.0f / 16);
                const bool first = j == start, last = j == NN - 2;
                seg(R, sx[j], sy[j], mx, my, L, first, false);
                seg(R, mx, my, sx[j + 1], sy[j + 1], L, false, last);
                glow_line(R, sx[j], sy[j], sx[j + 1], sy[j + 1], haze, 1.0f);
            }
        }
        a = b;
    }
}

void reroll(Render &R, float *fb) {
    auto rn = [&]() { uint32_t x = R.rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; R.rng = x; return (float)(x >> 8) * (1.0f / 16777216.0f); };
    fb[0] = rn();                                                    // where along the last stretch it leaves
    fb[1] = rn() < 0.15f ? (rn() - 0.5f) * 6.2831853f : (rn() + rn() - 1) * 2.2f;   // which way over the glass
    fb[2] = rn();                                                    // how far
    fb[3] = rn() * 2 - 1;                                            // two kinks
    fb[4] = rn() * 2 - 1;
    fb[5] = rn();                                                    // faint or less faint
    fb[6] = rn();                                                    // forks, below 0.3
}

// Where a channel meets the glass it stops being a line: a flickering brush of fibres,
// a glowing tail along the glass the foot has just left, and a soft warm spot.
void brush(Render &R, const Engine &e, int fi) {
    const Fil &f = e.fil[fi];
    const int F = (NN - 1) * 3, B = (NN - 3) * 3;
    const float rb = e.radii[NN - 3];
    const float c = f.I / IREF;
    const float dz = 0.42f + 0.58f * (f.d[F + 2] + 1) * 0.5f;
    const float al = f.life * dz * fminf(2.0f, powf(c, 0.6f));
    if (al < 0.01f) return;
    const float base = R.hue + SPREAD * f.hue;
    const float fx = f.d[F], fy = f.d[F + 1], fz = f.d[F + 2];
    const float wf = WF * dz;

    // the way the channel arrives, laid flat on the glass at the foot
    float ax = fx - f.d[B], ay = fy - f.d[B + 1], az = fz - f.d[B + 2];
    const float kd = ax * fx + ay * fy + az * fz;
    ax -= kd * fx; ay -= kd * fy; az -= kd * fz;
    float L = sqrtf(ax * ax + ay * ay + az * az);
    if (L < 1e-5f) {
        // any direction on the glass will do
        ax = fabsf(fz) > 0.9f ? 1.0f : 0.0f; ay = 0; az = fabsf(fz) > 0.9f ? 0.0f : 1.0f;
        const float k2 = ax * fx + az * fz;
        ax -= k2 * fx; ay -= k2 * fy; az -= k2 * fz;
        L = sqrtf(ax * ax + ay * ay + az * az);
    }
    ax /= L; ay /= L; az /= L;
    const float bx = fy * az - fz * ay, by = fz * ax - fx * az, bz = fx * ay - fy * ax;

    const float pxPerRad = K / (D - fz);
    const float size = WF * (0.8f + 0.25f * fminf(1.5f, sqrtf(c))) / pxPerRad;

    // fibres: mostly haze, the lines only just there
    const float ral = fminf(1.3f, al);
    const RGB c70 = shade(base, 1, 96, 70), c58 = shade(base, 1, 96, 58);
    Lut dim, lit;
    {
        const Pass pd[2] = { { 3.5f * wf, fminf(1.0f, 0.05f * ral), c70 }, { 1.2f * wf, fminf(1.0f, 0.06f * ral), c70 } };
        const Pass pl[2] = { { 3.5f * wf, fminf(1.0f, 0.09f * ral), c70 }, { 1.2f * wf, fminf(1.0f, 0.12f * ral), c70 } };
        build_lut(dim, pd, 2);
        build_lut(lit, pl, 2);
    }
    float hzD[3], hzL[3];
    {
        const float sd = 11.0f * wf * fminf(1.0f, 0.055f * ral) * 255.0f, sl = 11.0f * wf * fminf(1.0f, 0.09f * ral) * 255.0f;
        hzD[0] = c58.r * sd + dim.e[0] * BLOOM; hzD[1] = c58.g * sd + dim.e[1] * BLOOM; hzD[2] = c58.b * sd + dim.e[2] * BLOOM;
        hzL[0] = c58.r * sl + lit.e[0] * BLOOM; hzL[1] = c58.g * sl + lit.e[1] * BLOOM; hzL[2] = c58.b * sl + lit.e[2] * BLOOM;
    }
    float *fbAll = &R.fib[fi * FIBMAX * FIB];
    int nf = (int)lroundf(4 + 2 * fminf(3.0f, c));
    if (nf > FIBMAX) nf = FIBMAX;
    const float bdx = f.d[B], bdy = f.d[B + 1], bdz = f.d[B + 2];
    for (int i = 0; i < nf; ++i) {
        float *fb = &fbAll[i * FIB];
        uint32_t x = R.rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; R.rng = x;
        if ((x >> 8) < (uint32_t)(0.4f * 16777216.0f) || fb[2] == 0) reroll(R, fb);
        const float ang = (16 + 30 * fb[2]) * size;
        const float ct = cosf(fb[1]), st = sinf(fb[1]);
        const float tx = ax * ct + bx * st, ty = ay * ct + by * st, tz = az * ct + bz * st;   // along the glass
        const float px = fy * tz - fz * ty, py = fz * tx - fx * tz, pz = fx * ty - fy * tx;   // across it
        const float a = fb[0];
        float sx = bdx + (fx - bdx) * a, sy = bdy + (fy - bdy) * a, sz = bdz + (fz - bdz) * a;
        const float sl = 1.0f / (sqrtf(sx * sx + sy * sy + sz * sz) + 1e-9f);
        sx *= sl; sy *= sl; sz *= sl;
        const float r0 = rb + (1 - rb) * (0.35f + 0.65f * a);
        const bool isLit = fb[5] > 0.65f;
        const Lut &LU = isLit ? lit : dim;
        const float *hz = isLit ? hzL : hzD;
        float qx, qy;
        proj(sx * r0, sy * r0, sz * r0, qx, qy);
        static const float U06[4] = { 0.0f, 0.51728f, 0.78394f, 1.0f };   // (q/3)^0.6
        for (int q = 1; q <= 3; ++q) {
            const float u = q / 3.0f;
            const float kink = (q == 1 ? fb[3] : q == 2 ? fb[4] : 0) * ang * 0.4f;
            const float X = sx + (fx - sx) * u + tx * ang * u + px * kink;
            const float Y = sy + (fy - sy) * u + ty * ang * u + py * kink;
            const float Z = sz + (fz - sz) * u + tz * ang * u + pz * kink;
            const float r = (r0 + (1 - r0) * U06[q]) / (sqrtf(X * X + Y * Y + Z * Z) + 1e-9f);
            float nx, ny;
            proj(X * r, Y * r, Z * r, nx, ny);
            seg(R, qx, qy, nx, ny, LU, q == 1, q == 3);
            glow_line(R, qx, qy, nx, ny, hz, 1.0f);
            if (q == 2 && fb[6] < 0.3f) {
                // a fork off the second kink, bent away to one side
                const float side = fb[6] < 0.15f ? 0.7f : -0.7f;
                const float c2 = cosf(fb[1] + side), s2 = sinf(fb[1] + side);
                float Ex = fx + (ax * c2 + bx * s2) * ang * 0.8f, Ey = fy + (ay * c2 + by * s2) * ang * 0.8f, Ez = fz + (az * c2 + bz * s2) * ang * 0.8f;
                const float M = 1.0f / (sqrtf(Ex * Ex + Ey * Ey + Ez * Ez) + 1e-9f);
                float gx, gy;
                proj(Ex * M, Ey * M, Ez * M, gx, gy);
                seg(R, nx, ny, gx, gy, LU, false, true);
                glow_line(R, nx, ny, gx, gy, hz, 1.0f);
            }
            qx = nx; qy = ny;
        }
    }

    // the tail: stops at a length on the screen, however far the foot went
    if (f.trailN > 2) {
        float tx[TRAILN], ty[TRAILN];
        const float maxPx = 34.0f * WF;
        float run = 0;
        int n = 0;
        for (int t = 0; t < f.trailN; ++t) {
            proj(f.trail[t * 3], f.trail[t * 3 + 1], f.trail[t * 3 + 2], tx[t], ty[t]);
            if (t > 0) {
                run += sqrtf((tx[t] - tx[t - 1]) * (tx[t] - tx[t - 1]) + (ty[t] - ty[t - 1]) * (ty[t] - ty[t - 1]));
                if (run > maxPx) break;
            }
            n = t + 1;
        }
        if (n > 2 && run > 0.75f) {
            const RGB c62 = shade(base, 1, 96, 62);
            const int half = n >> 1;
            for (int part = 0; part < 2; ++part) {
                const int t0 = part ? half : 0, t1 = part ? n - 1 : half;
                const Pass p = { (part ? 10.0f : 18.0f) * wf, fminf(1.0f, (part ? 0.07f : 0.13f) * al), c62 };
                Lut TL;
                build_lut(TL, &p, 1);
                float e3[3] = { TL.e[0] * BLOOM, TL.e[1] * BLOOM, TL.e[2] * BLOOM };
                for (int t = t0; t < t1; ++t) {
                    seg(R, tx[t], ty[t], tx[t + 1], ty[t + 1], TL, t == t0, t == t1 - 1);
                    glow_line(R, tx[t], ty[t], tx[t + 1], ty[t + 1], e3, 1.0f);
                }
            }
        }
    }

    // the spot where it lands, foreshortened as the glass is
    float gx, gy;
    proj(fx, fy, fz, gx, gy);
    const float rad = (12 + 9 * fminf(2.0f, sqrtf(c))) * wf;
    const float lean = fmaxf(0.18f, fabsf(D * fz - 1) / sqrtf(fx * fx + fy * fy + (D - fz) * (D - fz)));
    const float boost = fminf(1.6f, 1.0f / sqrtf(lean));
    const float pos[3] = { 0, 0.35f, 1 };
    const RGB col[3] = { shade(base, 1, 90, 82), shade(base, 1, 96, 62), shade(base, 1, 96, 55) };
    const float alpha[3] = { 1, 0.8f, 0 };
    Spot S;
    build_spot(S, pos, col, alpha, 3, GAIN * fminf(1.0f, 0.7f * al * boost));
    spot(R, gx, gy, rad, lean, S);
}

// Where each trunk leaves the electrode: a bright point on its surface, front side only.
void roots(Render &R, const Engine &e) {
    const float r0 = e.r0;
    for (int fi = 0; fi < MAXF; ++fi) {
        const Fil &f = e.fil[fi];
        if (!f.used || f.par >= 0 || f.life <= 0) continue;
        const float c = f.segI[1] / IREF;
        if (c < 0.05f) continue;
        if (D * f.d[2] - r0 < -0.15f) continue;
        float x, y;
        proj(f.d[0] * r0, f.d[1] * r0, f.d[2] * r0, x, y);
        const float base = R.hue + SPREAD * f.hue;
        const float rad = (4 + 5 * fminf(2.0f, sqrtf(c))) * WF;
        const float pos[3] = { 0, 0.4f, 1 };
        const RGB col[3] = { shade(base, 0, 60, 94), shade(base, 0, 100, 72), shade(base, 0, 100, 60) };
        const float alpha[3] = { 1, 0.55f, 0 };
        Spot S;
        build_spot(S, pos, col, alpha, 3, GAIN * fminf(1.0f, f.life * (0.4f + 0.3f * sqrtf(c))));
        spot(R, x, y, rad, 1.0f, S);
    }
}

// The electrode: opaque, so it is stamped over what is behind it, then its glow is added.
void electrode(Render &R) {
    const int S = R.elecS;
    for (int sy = 0; sy < S; ++sy) {
        const int y = R.elecY + sy;
        if (y < 0 || y >= RW) continue;
        uint8_t *p = R.acc + (y * RW + R.elecX) * 3;
        const uint8_t *s = R.elec + sy * S * 4;
        const uint8_t *g = R.elecGlow + sy * S * 3;
        for (int sx = 0; sx < S; ++sx, p += 3, s += 4, g += 3) {
            const unsigned cv = s[3];
            if (cv) {
                p[0] = (uint8_t)((p[0] * (255 - cv) + s[0] * cv) / 255);
                p[1] = (uint8_t)((p[1] * (255 - cv) + s[1] * cv) / 255);
                p[2] = (uint8_t)((p[2] * (255 - cv) + s[2] * cv) / 255);
            }
            unsigned r = p[0] + g[0], gg = p[1] + g[1], b = p[2] + g[2];
            p[0] = (uint8_t)(r > 255 ? 255 : r); p[1] = (uint8_t)(gg > 255 ? 255 : gg); p[2] = (uint8_t)(b > 255 ? 255 : b);
        }
    }
}

// The Power readout: a thin arc of light on the inside of the glass along the bottom, lit
// from the left as far as the level. Drawn as light rather than as an LVGL arc because it
// is repainted every frame anyway, and here it costs a few dozen short strokes.
void ring(Render &R) {
    if (R.ring < 0) return;
    const float rr = RL - 3.5f;
    const float a0 = 135.0f * 3.14159265f / 180.0f, span = 90.0f * 3.14159265f / 180.0f;
    const RGB on = shade(R.hue, 1, 100, 72), off = hsl(R.hue, 20, 70);
    const Pass pOn[2] = { { 3.0f, 0.35f, on }, { 1.3f, 0.9f, shade(R.hue, 1, 90, 88) } };
    const Pass pOff = { 1.2f, 0.22f, off };
    Lut LOn, LOff;
    build_lut(LOn, pOn, 2);
    build_lut(LOff, &pOff, 1);
    const int N = 36;
    const int lit = (int)lroundf(R.ring * N);
    float px = C + rr * cosf(a0), py = C + rr * sinf(a0);
    for (int i = 1; i <= N; ++i) {
        const float a = a0 - span * i / N;
        const float nx = C + rr * cosf(a), ny = C + rr * sinf(a);
        seg(R, px, py, nx, ny, i <= lit ? LOn : LOff, i == 1 || i == lit + 1, i == N || i == lit);
        px = nx; py = ny;
    }
}

void blur_glow(Render &R) {
    // [1 4 6 4 1] / 16, across then down: a Gaussian of about one cell
    static const float W5[5] = { 1 / 16.f, 4 / 16.f, 6 / 16.f, 4 / 16.f, 1 / 16.f };
    float *a = R.glow, *b = R.tmp;
    for (int y = 0; y < LG; ++y)
        for (int x = 0; x < LG; ++x) {
            float s0 = 0, s1 = 0, s2 = 0;
            for (int k = -2; k <= 2; ++k) {
                int xx = x + k; xx = xx < 0 ? 0 : xx >= LG ? LG - 1 : xx;
                const float *p = &a[(y * LG + xx) * 3];
                s0 += W5[k + 2] * p[0]; s1 += W5[k + 2] * p[1]; s2 += W5[k + 2] * p[2];
            }
            float *o = &b[(y * LG + x) * 3];
            o[0] = s0; o[1] = s1; o[2] = s2;
        }
    for (int y = 0; y < LG; ++y)
        for (int x = 0; x < LG; ++x) {
            float s0 = 0, s1 = 0, s2 = 0;
            for (int k = -2; k <= 2; ++k) {
                int yy = y + k; yy = yy < 0 ? 0 : yy >= LG ? LG - 1 : yy;
                const float *p = &b[(yy * LG + x) * 3];
                s0 += W5[k + 2] * p[0]; s1 += W5[k + 2] * p[1]; s2 += W5[k + 2] * p[2];
            }
            float *o = &a[(y * LG + x) * 3];
            o[0] = s0; o[1] = s1; o[2] = s2;
        }
}

// The one full-frame pass. Accumulated light + the gas and glass + bloom + reflection,
// clamped, dithered to RGB565; the accumulator is cleared behind it.
void finish(Render &R) {
    static int16_t vrow[LG * 3];
    // Bloom cell coordinates for each render column: (x + 0.5) / 4 - 0.5, in eighths.
    for (int y = 0; y < RW; ++y) {
        const int xl = R.chordL[y], xr = R.chordR[y];
        if (xl > xr) continue;
        {
            const int ys = 2 * y - 3;
            int iy0 = ys >> 3; const int fy = ys & 7;
            int iy1 = iy0 + 1;
            if (iy0 < 0) iy0 = 0;
            if (iy1 > LG - 1) iy1 = LG - 1;
            const float *g0 = &R.glow[iy0 * LG * 3], *g1 = &R.glow[iy1 * LG * 3];
            const float w0 = (8 - fy) * 2.0f, w1 = fy * 2.0f;   // x16 fixed, over the 8ths
            for (int i = 0; i < LG * 3; ++i) {
                const float v = g0[i] * w0 + g1[i] * w1;
                vrow[i] = (int16_t)(v > 32000.0f ? 32000.0f : v);
            }
        }
        const int dy = y - 116;
        const int dy2 = dy * dy;
        uint8_t *p = R.acc + (y * RW + xl) * 3;
        uint16_t *o = R.out + y * RW + xl;
        const bool inRefl = y >= R.reflY && y < R.reflY + R.reflH;
        const uint8_t *rrow = inRefl ? R.refl + (y - R.reflY) * R.reflW : nullptr;
        const uint8_t *bay = &BAYER16[(y & 3) * 4];
        for (int x = xl; x <= xr; ++x, p += 3, ++o) {
            const int dx = x - 116;
            const int idx = ((dx * dx + dy2) * 4945) >> 16;          // r^2 / RD^2 * 1024
            const uint8_t *bs = &R.base[(idx > 1024 ? 1024 : idx) * 3];
            const int xs = 2 * x - 3;
            int ix0 = xs >> 3; const int fx = xs & 7;
            int ix1 = ix0 + 1;
            if (ix0 < 0) ix0 = 0;
            if (ix1 > LG - 1) ix1 = LG - 1;
            const int16_t *v0 = &vrow[ix0 * 3], *v1 = &vrow[ix1 * 3];
            int rf = 0;
            if (rrow) {
                const int rx = x - R.reflX;
                if (rx >= 0 && rx < R.reflW) rf = rrow[rx];
            }
            const int d = bay[x & 3];
            int r = p[0] + bs[0] + rf + ((v0[0] * (8 - fx) + v1[0] * fx) >> 7);
            int g = p[1] + bs[1] + rf + ((v0[1] * (8 - fx) + v1[1] * fx) >> 7);
            int b = p[2] + bs[2] + rf + ((v0[2] * (8 - fx) + v1[2] * fx) >> 7);
            p[0] = p[1] = p[2] = 0;
            r = (r + (d >> 1)) >> 3; g = (g + (d >> 2)) >> 2; b = (b + (d >> 1)) >> 3;
            if (r > 31) r = 31;
            if (g > 63) g = 63;
            if (b > 31) b = 31;
            *o = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

} // namespace

bool render_alloc(Render &R, float hue, float r0) {
    memset(&R, 0, sizeof(R));
    R.hue = hue;
    R.rng = 0x2545F491u;
    R.ring = -1;
    R.acc  = (uint8_t *)big_alloc((size_t)RW * RW * 3);
    R.out  = (uint16_t *)big_alloc((size_t)RW * RW * 2);
    R.glow = (float *)big_alloc((size_t)LG * LG * 3 * sizeof(float));
    R.tmp  = (float *)big_alloc((size_t)LG * LG * 3 * sizeof(float));
    R.base = (uint8_t *)big_alloc(1025 * 3);
    R.fib  = (float *)big_alloc((size_t)MAXF * FIBMAX * FIB * sizeof(float));

    // the glass's reflection: an ellipse, blurred by being a gradient
    const float rcx = C - 0.36f * RL, rcy = C - 0.44f * RL, rrad = 0.33f * RL;
    R.reflX = (int)floorf(rcx - rrad); R.reflY = (int)floorf(rcy - rrad);
    R.reflW = R.reflH = (int)ceilf(2 * rrad) + 2;
    R.refl = (uint8_t *)big_alloc((size_t)R.reflW * R.reflH);

    // the electrode, at its true projected size
    const float re = K * r0 / sqrtf(D * D - r0 * r0);
    const float out = re * 2.3f;
    R.elecS = (int)ceilf(out) * 2 + 2;
    R.elecX = (int)floorf(C - R.elecS / 2.0f);
    R.elecY = R.elecX;
    R.elec = (uint8_t *)big_alloc((size_t)R.elecS * R.elecS * 4);
    R.elecGlow = (uint8_t *)big_alloc((size_t)R.elecS * R.elecS * 3);

    if (!R.acc || !R.out || !R.glow || !R.tmp || !R.base || !R.fib || !R.refl || !R.elec || !R.elecGlow) return false;

    memset(R.acc, 0, (size_t)RW * RW * 3);
    memset(R.out, 0, (size_t)RW * RW * 2);
    memset(R.glow, 0, (size_t)LG * LG * 3 * sizeof(float));
    memset(R.fib, 0, (size_t)MAXF * FIBMAX * FIB * sizeof(float));

    for (int y = 0; y < RW; ++y) {
        const float dy = (float)(y - 116);
        const float h2 = RD * RD - dy * dy;
        const int hw = h2 > 0 ? (int)floorf(sqrtf(h2)) : -1;
        R.chordL[y] = (int16_t)(hw >= 0 ? 116 - hw : 1);
        R.chordR[y] = (int16_t)(hw >= 0 ? 116 + hw : 0);
    }

    // The gas and the glass, by radius: a dark haze of the plasma's own colour, the
    // plasma piling up against the inside of the shell, and a fine bright rim.
    const RGB g0 = hsl(hue, 70, 7), g1 = hsl(hue - 12, 60, 4);
    const RGB s1 = hsl(hue, 100, 58), s2 = hsl(hue, 100, 74), rim = hsl(hue, 60, 90);
    for (int i = 0; i <= 1024; ++i) {
        const float r = sqrtf(i / 1024.0f) * RD;
        float cr, cg, cb;
        if (r > RL + 0.5f) { cr = 5 / 255.f; cg = 4 / 255.f; cb = 10 / 255.f; }
        else {
            const float t = clampf(r / RL, 0, 1);
            cr = g0.r + (g1.r - g0.r) * t; cg = g0.g + (g1.g - g0.g) * t; cb = g0.b + (g1.b - g0.b) * t;
            // glass: 0 at 0.6, 0.06 of s1 at 0.82, 0.20 of s2 at the rim (premultiplied)
            float ar = 0, ag = 0, ab = 0;
            if (t > 0.6f && t <= 0.82f) {
                const float f = (t - 0.6f) / 0.22f;
                ar = s1.r * 0.06f * f; ag = s1.g * 0.06f * f; ab = s1.b * 0.06f * f;
            } else if (t > 0.82f) {
                const float f = (t - 0.82f) / 0.18f;
                ar = s1.r * 0.06f + (s2.r * 0.2f - s1.r * 0.06f) * f;
                ag = s1.g * 0.06f + (s2.g * 0.2f - s1.g * 0.06f) * f;
                ab = s1.b * 0.06f + (s2.b * 0.2f - s1.b * 0.06f) * f;
            }
            const float rc = clampf(1.0f - fabsf(r - (RL - 0.5f)), 0, 1) * 0.16f;
            cr += ar + rim.r * rc; cg += ag + rim.g * rc; cb += ab + rim.b * rc;
            if (r > RL - 0.5f) {       // antialias the silhouette into the room
                const float k = clampf(RL + 0.5f - r, 0, 1);
                cr = cr * k + (1 - k) * 5 / 255.f; cg = cg * k + (1 - k) * 4 / 255.f; cb = cb * k + (1 - k) * 10 / 255.f;
            }
        }
        const float bk = r > RL + 0.5f ? 1.0f : BROAD;
        R.base[i * 3]     = (uint8_t)clampf(cr * 255.0f * bk + 0.5f, 0, 255);
        R.base[i * 3 + 1] = (uint8_t)clampf(cg * 255.0f * bk + 0.5f, 0, 255);
        R.base[i * 3 + 2] = (uint8_t)clampf(cb * 255.0f * bk + 0.5f, 0, 255);
    }

    // reflection: translate, rotate(-0.5), scale(1, 0.5), radial gradient to 0.33 R
    {
        const float cs = cosf(0.5f), sn = sinf(0.5f);
        for (int y = 0; y < R.reflH; ++y)
            for (int x = 0; x < R.reflW; ++x) {
                const float dx = R.reflX + x + 0.5f - rcx, dy = R.reflY + y + 0.5f - rcy;
                const float lx = dx * cs - dy * sn, ly = (dx * sn + dy * cs) / 0.5f;
                const float t = sqrtf(lx * lx + ly * ly) / rrad;
                R.refl[y * R.reflW + x] = (uint8_t)(t < 1 ? 0.15f * (1 - t) * 255.0f + 0.5f : 0);
            }
    }

    // electrode sprite: the page's two-circle gradient for the ball, the ring and the rim
    {
        const float he = hue + END_SHIFT * 0.8f;
        const RGB e0 = hsl(he, 70, 46), e1 = hsl(he, 60, 24), e2 = hsl(he, 75, 34);
        const RGB q0 = hsl(he, 100, 80), q1 = hsl(he, 100, 68), q2 = hsl(he, 100, 56), rm = hsl(he, 100, 78);
        const float p0x = -0.3f * re, p0y = -0.35f * re, rho0 = 0.05f * re, dR = re - rho0;
        const float pp = p0x * p0x + p0y * p0y;
        const int S = R.elecS;
        for (int sy = 0; sy < S; ++sy)
            for (int sx = 0; sx < S; ++sx) {
                const float X = R.elecX + sx + 0.5f - C, Y = R.elecY + sy + 0.5f - C;
                const float r = sqrtf(X * X + Y * Y);
                uint8_t *o = &R.elec[(sy * S + sx) * 4];
                uint8_t *g = &R.elecGlow[(sy * S + sx) * 3];
                const float cv = clampf(re + 0.5f - r, 0, 1);
                float cr = 0, cg = 0, cb = 0;
                if (cv > 0) {
                    // |P - (1-t) p0| = rho0 + t dR, for the larger root t
                    const float Ax = X - p0x, Ay = Y - p0y;
                    const float qa = pp - dR * dR, qb = 2 * (Ax * p0x + Ay * p0y - rho0 * dR), qc = Ax * Ax + Ay * Ay - rho0 * rho0;
                    float t;
                    if (fabsf(qa) < 1e-6f) t = -qc / qb;
                    else {
                        const float disc = fmaxf(0.0f, qb * qb - 4 * qa * qc);
                        const float t1 = (-qb + sqrtf(disc)) / (2 * qa), t2 = (-qb - sqrtf(disc)) / (2 * qa);
                        t = fmaxf(t1, t2);
                        if (rho0 + t * dR < 0) t = fminf(t1, t2);
                    }
                    t = clampf(t, 0, 1);
                    RGB c;
                    if (t < 0.65f) { const float f = t / 0.65f; c = { e0.r + (e1.r - e0.r) * f, e0.g + (e1.g - e0.g) * f, e0.b + (e1.b - e0.b) * f }; }
                    else { const float f = (t - 0.65f) / 0.35f; c = { e1.r + (e2.r - e1.r) * f, e1.g + (e2.g - e1.g) * f, e1.b + (e2.b - e1.b) * f }; }
                    cr = c.r; cg = c.g; cb = c.b;
                }
                o[0] = (uint8_t)clampf(cr * 255 * BROAD, 0, 255); o[1] = (uint8_t)clampf(cg * 255 * BROAD, 0, 255); o[2] = (uint8_t)clampf(cb * 255 * BROAD, 0, 255);
                o[3] = (uint8_t)(cv * 255.0f + 0.5f);
                // added light: the ring outside the ball, and the rim across its face
                float ar = 0, ag = 0, ab = 0;
                if (r >= re * 0.92f && r < out) {
                    const float t = (r - re * 0.92f) / (out - re * 0.92f);
                    float a; RGB c;
                    if (t < 0.12f) { const float f = t / 0.12f; a = 0.55f + (0.3f - 0.55f) * f; c = { q0.r + (q1.r - q0.r) * f, q0.g + (q1.g - q0.g) * f, q0.b + (q1.b - q0.b) * f }; }
                    else if (t < 0.45f) { const float f = (t - 0.12f) / 0.33f; a = 0.3f + (0.1f - 0.3f) * f; c = { q1.r + (q2.r - q1.r) * f, q1.g + (q2.g - q1.g) * f, q1.b + (q2.b - q1.b) * f }; }
                    else { const float f = (t - 0.45f) / 0.55f; a = 0.1f * (1 - f); c = q2; }
                    ar += c.r * a; ag += c.g * a; ab += c.b * a;
                }
                if (r >= re * 0.55f && r < re) {
                    const float a = 0.35f * (r - re * 0.55f) / (re * 0.45f) * cv;
                    ar += rm.r * a; ag += rm.g * a; ab += rm.b * a;
                }
                g[0] = (uint8_t)clampf(ar * 255 * GAIN, 0, 255); g[1] = (uint8_t)clampf(ag * 255 * GAIN, 0, 255); g[2] = (uint8_t)clampf(ab * 255 * GAIN, 0, 255);
                R.elecE[0] += g[0] * BLOOM; R.elecE[1] += g[1] * BLOOM; R.elecE[2] += g[2] * BLOOM;
            }
    }
    return true;
}

void render_free(Render &R) {
    big_free(R.acc); big_free(R.out); big_free(R.glow); big_free(R.tmp); big_free(R.base);
    big_free(R.fib); big_free(R.refl); big_free(R.elec); big_free(R.elecGlow);
    memset(&R, 0, sizeof(R));
}

void render_frame(Render &R, const Engine &e) {
    R.segs = 0;
    memset(R.glow, 0, (size_t)LG * LG * 3 * sizeof(float));
    // the electrode is opaque: channels landing on the far side go first, then the
    // electrode over them, then the near side
    for (int pass = 0; pass < 2; ++pass) {
        for (int fi = 0; fi < MAXF; ++fi) {
            const Fil &f = e.fil[fi];
            if (!f.used || f.life <= 0) continue;
            const bool back = f.d[(NN - 1) * 3 + 2] < 0;
            if (back != (pass == 0)) continue;
            channel(R, e, f);
            brush(R, e, fi);
        }
        if (pass == 0) {
            electrode(R);
            glow_point(R, C, C, R.elecE);   // the electrode's own glow, into the bloom
        }
    }
    roots(R, e);
    ring(R);
    blur_glow(R);
    finish(R);
}

} // namespace plasma

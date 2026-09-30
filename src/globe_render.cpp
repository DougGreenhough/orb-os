// The Globe renderer. See globe_render.h.
#include "globe_render.h"
#include "globe_texture.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
static void *ps_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void  ps_free(void *p) { heap_caps_free(p); }
#else
static void *ps_alloc(size_t n) { return malloc(n); }
static void  ps_free(void *p) { free(p); }
#endif

namespace globe {
namespace {

constexpr float PI_F = 3.14159265f;
constexpr float CEN = RW / 2.0f;         // 116.5: the frame's centre, in render px

// The homepage's lights. Ambient 0.45, a directional light of 1.15 from (1.2, 0.6, 5),
// Phong specular 0x334455 with shininess 10 over the water. three.js r165 divides both by
// pi (BRDF_Lambert) and sRGB-encodes the result; see the bake script for why the encode
// can be split between the texture and this table.
constexpr float AMBIENT = 0.45f, DIRECT = 1.15f;
constexpr float LX = 1.2f, LY = 0.6f, LZ = 5.0f;
constexpr float SHINE = 10.0f;
constexpr float SPEC_F0 = 0.333f;        // 0x55, the blue of 0x334455; R and G follow below
// A little brighter than the page: the page's globe sits on a dark-blue sky and is big;
// here it is small and on black, and the panel is an AMOLED that crushes the low end.
constexpr float GAIN = 1.12f;

// The atmosphere: a haze that thickens over the last 18% of the radius and a glow just
// outside it, both brighter on the lit (upper-right) side.
constexpr uint8_t HZ_R = 96, HZ_G = 160, HZ_B = 255;
constexpr float HAZE_FROM = 0.80f, HAZE_MAX = 0.55f;
constexpr float GLOW_NEAR = 3.2f, GLOW_FAR = 11.0f;   // falloffs, render px
constexpr float GLOW_AT_LIMB = 0.75f + 0.22f;          // the glow's strength where it starts
constexpr float EDGE_PX = 1.3f;

// 4x4 ordered dither, added before each channel is cut to 5 or 6 bits. Without it the
// glint and the haze, both smooth gradients, come out as rings.
const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

float srgb(float x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    return x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1 / 2.4f) - 0.055f;
}

inline uint8_t u8(float v) { return (uint8_t)(v <= 0 ? 0 : v >= 255 ? 255 : v + 0.5f); }

inline uint16_t pack(int r, int g, int b) {
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// How lit the limb is in a screen direction: 0 on the terminator side, 1 towards the light.
float limb_light(float dx, float dy) {
    const float lxy = sqrtf(LX * LX + LY * LY);
    const float d = sqrtf(dx * dx + dy * dy);
    if (d <= 0) return 1;
    const float c = (dx * LX + dy * LY) / (d * lxy);
    return 0.35f + 0.65f * (c > 0 ? c : 0) + 0.15f * c;
}

// The sky: black, a glow round the limb, and a sprinkle of faint stars. Painted once;
// draw() only ever writes inside the disc.
void paint_sky(Render &r) {
    uint32_t rng = 0x9E3779B9u;
    auto rnd = [&rng]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    for (int y = 0; y < RW; ++y) {
        for (int x = 0; x < RW; ++x) {
            const float dx = x + 0.5f - CEN, dy = CEN - (y + 0.5f);
            const float d = sqrtf(dx * dx + dy * dy);
            uint16_t v = 0;
            if (d >= R) {
                const float e = d - R;
                const float g = (expf(-e / GLOW_NEAR) * 0.75f + expf(-e / GLOW_FAR) * 0.22f)
                                * limb_light(dx, dy);
                const int d = BAYER[y & 3][x & 3];
                v = pack(u8(HZ_R * g) + (d >> 1), u8(HZ_G * g) + (d >> 2), u8(HZ_B * g) + (d >> 1));
            }
            r.out[y * RW + x] = v;
        }
    }
    // Stars only where the glow has gone: a ring between the atmosphere and the bezel.
    for (int i = 0; i < 70; ++i) {
        const float a = (rnd() % 3600) * (2 * PI_F / 3600);
        const float d = R + 14 + (rnd() % 1000) * (CEN - R - 16) / 1000.0f;
        const int x = (int)(CEN + d * cosf(a)), y = (int)(CEN - d * sinf(a));
        if (x < 0 || y < 0 || x >= RW || y >= RW) continue;
        const int b = 40 + (int)(rnd() % 90);
        r.out[y * RW + x] = pack(b, b, b + 18);
    }
}

} // namespace

bool render_alloc(Render &r) {
    memset(&r, 0, sizeof(r));
    r.out = (uint16_t *)ps_alloc(RW * RW * 2);
    r.lon = (uint16_t *)ps_alloc(BOX * BOX * 2);
    r.row = (uint16_t *)ps_alloc(BOX * BOX * 2);
    r.shade = (uint8_t *)ps_alloc(BOX * BOX);
    r.spec = (uint8_t *)ps_alloc(BOX * BOX);
    r.haze = (uint8_t *)ps_alloc(BOX * BOX);
    r.spanL = (int16_t *)ps_alloc(BOX * 2 * sizeof(int16_t));
    if (!r.out || !r.lon || !r.row || !r.shade || !r.spec || !r.haze || !r.spanL) return false;
    r.spanR = r.spanL + BOX;

    const float ln = sqrtf(LX * LX + LY * LY + LZ * LZ);
    const float lx = LX / ln, ly = LY / ln, lz = LZ / ln;
    // Blinn half-vector between the light and the viewer (0, 0, 1)
    float hx = lx, hy = ly, hz = lz + 1;
    const float hn = sqrtf(hx * hx + hy * hy + hz * hz);
    hx /= hn; hy /= hn; hz /= hn;
    // A representative dark ocean (the map's raw bytes, used as linear by the page), to
    // turn the glint's linear light into an encoded amount to add.
    const float oce = 45 / 255.0f;

    for (int j = 0; j < BOX; ++j) {
        r.spanL[j] = BOX; r.spanR[j] = -1;
        for (int i = 0; i < BOX; ++i) {
            const int k = j * BOX + i;
            const float px = (X0 + i + 0.5f - CEN) / R, py = (CEN - (X0 + j + 0.5f)) / R;
            const float rr = px * px + py * py;
            r.shade[k] = r.spec[k] = r.haze[k] = 0;
            if (rr >= 1) continue;
            if (i < r.spanL[j]) r.spanL[j] = (int16_t)i;
            r.spanR[j] = (int16_t)i;
            const float pz = sqrtf(1 - rr);
            float nl = px * lx + py * ly + pz * lz;
            if (nl < 0) nl = 0;
            const float kd = (AMBIENT + DIRECT * nl) / PI_F;
            float nh = px * hx + py * hy + pz * hz;
            if (nh < 0) nh = 0;
            // BRDF_BlinnPhong: F * G(0.25) * D, D = (shininess/2 + 1)/pi * nh^shininess
            const float sl = DIRECT * nl * SPEC_F0 * 0.25f * (SHINE * 0.5f + 1) / PI_F * powf(nh, SHINE);
            const float spec = srgb(oce * kd + sl) - srgb(oce * kd);
            // the haze: thin in the middle, thick at the rim
            const float rad = sqrtf(rr);
            float h = 0;
            if (rad > HAZE_FROM) { const float t = (rad - HAZE_FROM) / (1 - HAZE_FROM); h = t * t * HAZE_MAX; }
            const float lit = limb_light(px, py);
            // Anti-aliasing the limb: over the last EDGE_PX the map gives way to the glow's
            // own colour, so the disc melts into the atmosphere rather than stepping.
            float edge = (1 - rad) * R / EDGE_PX;
            if (edge > 1) edge = 1;
            h += (1 - edge) * (GLOW_AT_LIMB - h);
            r.shade[k] = u8(srgb(kd) * GAIN * (1 - h) * edge * 255);
            r.spec[k] = u8(spec * edge * 255);
            r.haze[k] = u8(h * lit * 255);
        }
    }
    paint_sky(r);
    r.tablesOk = false;
    return true;
}

void render_free(Render &r) {
    ps_free(r.out); ps_free(r.lon); ps_free(r.row);
    ps_free(r.shade); ps_free(r.spec); ps_free(r.haze); ps_free(r.spanL);
    memset(&r, 0, sizeof(r));
}

void build_tables(Render &r, float tiltDeg) {
    const float a = tiltDeg * PI_F / 180;
    const float ca = cosf(a), sa = sinf(a);
    for (int j = 0; j < BOX; ++j) {
        for (int i = r.spanL[j]; i <= r.spanR[j]; ++i) {
            const int k = j * BOX + i;
            const float px = (X0 + i + 0.5f - CEN) / R, py = (CEN - (X0 + j + 0.5f)) / R;
            float rr = px * px + py * py;
            if (rr > 1) rr = 1;
            const float pz = sqrtf(1 - rr);
            // undo the tilt (rotation.x = tilt)
            const float y1 = py * ca + pz * sa;
            const float z1 = -py * sa + pz * ca;
            const float lat = asinf(y1 > 1 ? 1 : y1 < -1 ? -1 : y1);
            // texel centres sit at +0.5, so the bilinear sample coordinate is shifted by half
            float v = (0.5f - lat / PI_F) * GLOBE_TEX_H - 0.5f;
            if (v < 0) v = 0;
            if (v > GLOBE_TEX_H - 1) v = GLOBE_TEX_H - 1;
            r.row[k] = (uint16_t)(v * 256.0f);
            // longitude plus the Y turn; the turn is added per frame
            const float b = atan2f(-z1, px);
            r.lon[k] = (uint16_t)(int32_t)lroundf(b * (65536.0f / (2 * PI_F)));
        }
    }
    r.tilt = tiltDeg;
    r.tablesOk = true;
}

void draw(Render &r, float lonDeg) {
    if (!r.tablesOk) return;
    // texture column = (lon + 180) / 360, lon = base + face + 90 (see globe_render.h)
    float f = fmodf(lonDeg + 270.0f, 360.0f);
    if (f < 0) f += 360;
    // minus half a column: texel centres (bilinear, as for the rows)
    const uint16_t off = (uint16_t)((int32_t)lroundf(f * (65536.0f / 360.0f)) - 64);
    constexpr int TW = GLOBE_TEX_W, TH = GLOBE_TEX_H;
    constexpr int USHIFT = 16 - 9;        // 65536 -> 512 columns
    static_assert(GLOBE_TEX_W == 512, "USHIFT assumes a 512-wide map");
    for (int j = 0; j < BOX; ++j) {
        const int L = r.spanL[j], Rr = r.spanR[j];
        if (L > Rr) continue;
        uint16_t *o = r.out + (X0 + j) * RW + X0;
        const int kb = j * BOX;
        const uint8_t *bay = BAYER[(X0 + j) & 3];
        for (int i = L; i <= Rr; ++i) {
            const int k = kb + i;
            const uint16_t lon = (uint16_t)(r.lon[k] + off);
            const int u0 = lon >> USHIFT, u1 = (u0 + 1) & (TW - 1);
            const uint32_t fu = (lon >> (USHIFT - 5)) & 31;
            const int v0 = r.row[k] >> 8, v1 = v0 + 1 < TH ? v0 + 1 : TH - 1;
            const uint32_t fv = (r.row[k] >> 3) & 31;
            // bilinear in RGB565, the channels spread apart in one 32-bit word (5-bit weights)
            const uint16_t *t0 = globe_tex + v0 * TW, *t1 = globe_tex + v1 * TW;
            auto sp = [](uint16_t c) { return ((uint32_t)c | ((uint32_t)c << 16)) & 0x07E0F81Fu; };
            const uint32_t a = (sp(t0[u0]) * (32 - fu) + sp(t0[u1]) * fu) >> 5 & 0x07E0F81Fu;
            const uint32_t b = (sp(t1[u0]) * (32 - fu) + sp(t1[u1]) * fu) >> 5 & 0x07E0F81Fu;
            const uint32_t m = (a * (32 - fv) + b * fv) >> 5 & 0x07E0F81Fu;
            const uint16_t t = (uint16_t)(m | (m >> 16));
            const int s = r.shade[k];
            int R8 = ((t >> 11) << 3) | (t >> 13);
            int G8 = (((t >> 5) & 63) << 2) | ((t >> 9) & 3);
            int B8 = ((t & 31) << 3) | ((t >> 2) & 7);
            R8 = (R8 * s) >> 8; G8 = (G8 * s) >> 8; B8 = (B8 * s) >> 8;
            if (const int h = r.haze[k]) {
                R8 += (h * HZ_R) >> 8; G8 += (h * HZ_G) >> 8; B8 += (h * HZ_B) >> 8;
            }
            const int ti = (fv < 16 ? v0 : v1) * TW + (fu < 16 ? u0 : u1);   // the nearest texel
            if ((globe_ocean[ti >> 3] >> (ti & 7)) & 1) {
                const int sp8 = r.spec[k];   // 0x334455: R and G at 3/5 and 4/5 of B
                R8 += (sp8 * 154) >> 8; G8 += (sp8 * 205) >> 8; B8 += sp8;
            }
            const int d = bay[(X0 + i) & 3];
            o[i] = pack(R8 + (d >> 1), G8 + (d >> 2), B8 + (d >> 1));
        }
    }
}

void project(float latDeg, float lonDeg, float tiltDeg, float faceLonDeg, float *x, float *y, float *z) {
    const float d = PI_F / 180;
    const float phi = latDeg * d;
    const float lt = (lonDeg - faceLonDeg - 90) * d;   // lambda + theta, theta = -face - 90
    const float cp = cosf(phi);
    const float x1 = cp * cosf(lt), y1 = sinf(phi), z1 = -cp * sinf(lt);
    const float a = tiltDeg * d, ca = cosf(a), sa = sinf(a);
    *x = x1;
    *y = y1 * ca - z1 * sa;
    *z = y1 * sa + z1 * ca;
}

} // namespace globe

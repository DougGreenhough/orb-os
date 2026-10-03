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
// outside it, both brighter on the lit (upper-right) side. Its colour is the style's.
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

inline float clampf01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
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
    const int HZ_R = r.st->hz[0], HZ_G = r.st->hz[1], HZ_B = r.st->hz[2];
    uint32_t rng = 0x9E3779B9u;
    auto rnd = [&rng]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    for (int y = 0; y < RW; ++y) {
        for (int x = 0; x < RW; ++x) {
            const float dx = x + 0.5f - CEN, dy = CEN - (y + 0.5f);
            const float d = sqrtf(dx * dx + dy * dy);
            uint16_t v = 0;
            if (d >= R && !r.st->sky) {
                v = pack(HZ_R, HZ_G, HZ_B);      // not drawn; only averaged with at the limb
            } else if (d >= R) {
                const float e = d - R;
                const float g = (expf(-e / GLOW_NEAR) * 0.75f + expf(-e / GLOW_FAR) * 0.22f)
                                * limb_light(dx, dy) * r.st->glow;
                const int d = BAYER[y & 3][x & 3];
                v = pack(u8(HZ_R * g) + (d >> 1), u8(HZ_G * g) + (d >> 2), u8(HZ_B * g) + (d >> 1));
            }
            r.out[y * RW + x] = v;
        }
    }
    if (!r.st->stars) return;
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

struct V3 { float r, g, b; };
V3 rgb(uint32_t c) { return { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f }; }
V3 mix(V3 a, V3 b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
V3 add(V3 a, V3 b, float k) { return { a.r + b.r * k, a.g + b.g * k, a.b + b.b * k }; }
float smooth(float t) { t = clampf01(t); return t * t * (3 - 2 * t); }
void put(uint8_t *o, V3 c) { o[0] = u8(c.r * 255); o[1] = u8(c.g * 255); o[2] = u8(c.b * 255); }
// A line of half-width w centred at d0, with a soft edge of `soft`: its coverage at d.
float line(float d, float d0, float w, float soft) { return clampf01((w + soft - fabsf(d - d0)) / soft); }

} // namespace

// The drawn globes. Distances are in texel-heights of the coast map (0.7 degrees; a little
// over one render px at the middle of the disc), negative over water.
void make_style(Style &s, Kind kind, const Colours &c) {
    s = Style();
    if (kind == PLAIN) return;
    const V3 bg = rgb(c.bg), text = rgb(c.text), dim = rgb(c.dim), acc = rgb(c.accent), acc2 = rgb(c.accent2), rule = rgb(c.rule);
    const V3 black = { 0, 0, 0 }, white = { 1, 1, 1 };
    const float gk = 0.4f + 0.2f * c.glow;           // how much things may glow: 0.4 .. 1.0
    s.stars = false;
    auto hz = [&s](V3 v) { put(s.hz, v); };
    auto grat = [&s](V3 v, float a, float w) { put(s.grat, v); s.gratA = u8(a * 255); s.gratW = w; };
    if (kind == PHOTO) {
        // The photograph, with the theme's accent for an atmosphere.
        hz(mix(acc, white, 0.12f));
        s.glow = 0.75f + 0.08f * c.glow;
        return;
    }
    s.chart = true;
    for (int i = 0; i < 256; ++i) {
        const float d = (i - 128) / (float)GLOBE_COAST_SCALE;
        const float landK = smooth(d / 0.5f + 0.5f);     // 0 water .. 1 land, across the shore
        V3 v = black;
        switch (kind) {
        case PHOSPHOR: {
            // One colour: a dim fill for land, the shore as a bright trace with the tube's bloom.
            v = mix(bg, mix(bg, acc2, 0.30f), landK);
            v = add(v, acc2, 0.38f * gk * expf(-fabsf(d) / 1.3f));
            v = mix(v, text, line(d, 0, 0.22f, 0.55f));
            break;
        }
        case ATLAS: {
            // Ink on paper: a wash on the land, deepest at the shore, an inked coastline,
            // and the sea shaded in towards it with one line of an engraver's water-lining.
            const V3 open = mix(bg, white, 0.30f);
            const V3 sea = mix(open, mix(bg, dim, 0.42f), 0.9f * expf(-fabsf(d) / 1.3f));
            const V3 land = mix(mix(bg, acc2, 0.26f), mix(bg, acc2, 0.50f), expf(-fabsf(d) / 1.6f));
            v = mix(sea, land, landK);
            v = mix(v, text, 0.30f * line(d, -2.6f, 0.05f, 0.45f));
            v = mix(v, text, line(d, 0, 0.12f, 0.50f));
            break;
        }
        case ANTIQUE: {
            // Sepia land, its shore gilded, on a verdigris sea that is lightest in the shallows.
            const V3 deep = mix(black, acc2, 0.26f), shoal = mix(black, acc2, 0.60f);
            const V3 sea = mix(deep, shoal, expf(-fabsf(d) / 2.0f));
            const V3 land = mix(mix(bg, acc, 0.40f), mix(bg, acc, 0.62f), expf(-fabsf(d) / 2.5f));
            v = mix(sea, land, landK);
            v = mix(v, mix(acc, text, 0.35f), 0.9f * line(d, 0, 0.12f, 0.50f));
            break;
        }
        default: {   // NEON
            // Dark glass; the shore a hot line that glows one colour out to sea and the
            // other inland.
            v = mix(mix(black, rule, 0.30f), mix(black, acc, 0.13f), landK);
            v = add(v, acc2, 0.75f * gk * expf(-fabsf(d) / 1.5f) * (1 - landK));
            v = add(v, acc, 0.70f * gk * expf(-fabsf(d) / 2.2f) * landK);
            v = mix(v, mix(acc2, white, 0.7f), line(d, 0, 0.22f, 0.55f));
            break;
        }
        }
        put(s.ramp[i], v);
    }
    switch (kind) {
    case PHOSPHOR:
        hz(acc2); grat(acc2, 0.42f, 0.10f);
        s.ringW = 1.2f; s.glow = 0.55f * gk; s.haze = 0.35f;
        break;
    case ATLAS:
        hz(text); grat(mix(text, dim, 0.5f), 0.55f, 0.0f);
        s.ringW = 1.3f; s.sky = false; s.haze = 0.30f;
        break;
    case ANTIQUE:
        hz(acc); grat(text, 0.26f, 0.05f);
        s.ringW = 3.6f; s.sky = false; s.haze = 0; s.shading = 0.7f; s.gloss = 0.2f; s.relief = 0.55f;
        break;
    default:
        hz(mix(acc2, white, 0.15f)); grat(acc2, 0.24f, 0.05f);
        s.ringW = 1.0f; s.glow = 1.05f * gk; s.haze = 0.9f; s.gloss = 0.22f;
        break;
    }
}

bool render_alloc(Render &r, const Style &st) {
    memset(&r, 0, sizeof(r));
    r.st = (Style *)ps_alloc(sizeof(Style));
    if (!r.st) return false;
    *r.st = st;
    if (st.chart) {
        r.latLine = (uint8_t *)ps_alloc(BOX * BOX);
        r.lonQ = (uint8_t *)ps_alloc(BOX * BOX);
        if (!r.latLine || !r.lonQ) return false;
    }
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
            if (rad > HAZE_FROM) { const float t = (rad - HAZE_FROM) / (1 - HAZE_FROM); h = t * t * HAZE_MAX * st.haze; }
            const float lit = limb_light(px, py);
            if (st.chart) {
                // A drawn globe. The shade table holds how much of the drawing shows and
                // the haze table how much of the limb's colour is laid over it: the haze,
                // then the ring, which is opaque. Flat unless the style asks for a lit ball.
                const float litN = (AMBIENT + DIRECT * nl) / (AMBIENT + DIRECT);
                float s = 1 - st.shading * (1 - sqrtf(litN));
                float over = h * (st.sky ? lit : 1.0f);          // of the limb colour, 0..1
                const float e = (1 - rad) * R;                    // render px in from the limb
                if (st.ringW > 0) {
                    const float ra = clampf01(st.ringW + 0.5f - e);
                    float f = 1.0f;
                    if (st.ringW >= 3) {                          // a band of metal, lit from the upper right
                        const float t = clampf01(1 - e / st.ringW);
                        const float hl = (t - 0.4f) / 0.16f;
                        f = (0.34f + 0.5f * powf(4 * t * (1 - t), 0.7f) + 0.5f * expf(-hl * hl)) * (0.55f + 0.45f * lit);
                        if (f > 1) f = 1;
                    }
                    s *= 1 - ra;
                    over = over * (1 - ra) + ra * f;
                }
                if (st.sky) {                                     // and melt into the glow outside
                    float edge = e / EDGE_PX;
                    if (edge > 1) edge = 1;
                    s *= edge;
                    over += (1 - edge) * (GLOW_AT_LIMB * lit * st.glow - over);
                }
                r.shade[k] = u8(s * (1 - h) * 255);
                r.haze[k] = u8(over * 255);
                // the glass: a window's reflection, upper left
                const float gx = (px + 0.36f) * 0.878f + (py - 0.44f) * 0.479f, gy = (-(px + 0.36f) * 0.479f + (py - 0.44f) * 0.878f) / 0.5f;
                const float gt = sqrtf(gx * gx + gy * gy) / 0.33f;
                r.spec[k] = u8(gt < 1 ? st.gloss * 0.5f * (1 - gt) * 255 : 0);
                continue;
            }
            // Anti-aliasing the limb: over the last EDGE_PX the map gives way to the glow's
            // own colour, so the disc melts into the atmosphere rather than stepping.
            float edge = (1 - rad) * R / EDGE_PX;
            if (edge > 1) edge = 1;
            h += (1 - edge) * (GLOW_AT_LIMB - h);   // (the sky's own strength is applied below)
            r.shade[k] = u8(srgb(kd) * GAIN * (1 - h) * edge * 255);
            r.spec[k] = u8(spec * edge * 255);
            r.haze[k] = u8(h * lit * (st.sky ? st.glow > 1 ? 1.0f : 0.5f + 0.5f * st.glow : 1.0f) * 255);
        }
    }
    paint_sky(r);
    r.tablesOk = false;
    return true;
}

void render_free(Render &r) {
    ps_free(r.out); ps_free(r.lon); ps_free(r.row);
    ps_free(r.shade); ps_free(r.spec); ps_free(r.haze); ps_free(r.spanL);
    ps_free(r.latLine); ps_free(r.lonQ); ps_free(r.st);
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
            if (!r.st->chart) continue;
            // The graticule. A line is drawn by how many render px a pixel is from it, so
            // each pixel needs the size of a degree there: the gradients of latitude and
            // longitude across the screen, from the same projection.
            const float pzs = pz > 0.02f ? pz : 0.02f;
            const float cl = sqrtf(1 - (y1 > 1 ? 1 : y1 * y1));
            const float gyx = -(px / pzs) * sa, gyy = ca - (py / pzs) * sa;            // d(y1)/d(px, py)
            const float glat = sqrtf(gyx * gyx + gyy * gyy) / (cl > 0.02f ? cl : 0.02f) / R;   // rad per render px
            const float STEP = PI_F / 6;                                               // 30 degrees
            const float dl = fabsf(lat - roundf(lat / STEP) * STEP);
            float cov = 0;
            if (fabsf(lat) < 1.4f) {
                cov = clampf01(r.st->gratW + 0.5f - dl / glat);
                const float gap = STEP / glat;                                         // px between lines
                if (gap < 4) cov *= gap / 4;                                           // crowding at the limb
            }
            r.latLine[k] = u8(cov * 255);
            const float den = px * px + z1 * z1 + 1e-6f;
            const float gbx = (px * px * ca / pzs + z1) / den, gby = px * (sa + py * ca / pzs) / den;
            // in units of a twelfth of a turn = 65536, per render px
            const float glon = sqrtf(gbx * gbx + gby * gby) / R * (65536.0f * 12 / (2 * PI_F));
            const float q = 262144.0f / glon;
            r.lonQ[k] = q < 16 ? 0 : q > 255 ? 255 : (uint8_t)q;   // 0: the meridians have run together
        }
    }
    r.tilt = tiltDeg;
    r.tablesOk = true;
}

// A drawn globe's frame: see the note on styles in globe_render.h.
static void draw_chart(Render &r, uint16_t off) {
    constexpr int TW = GLOBE_TEX_W, TH = GLOBE_TEX_H;
    constexpr int USHIFT = 16 - 9;
    const Style &st = *r.st;
    const int HZ_R = st.hz[0], HZ_G = st.hz[1], HZ_B = st.hz[2];
    const int GR = st.grat[0], GG = st.grat[1], GB = st.grat[2], GA = st.gratA;
    const int W16 = (int)((st.gratW + 0.5f) * 16);
    uint8_t reliefLut[64];
    const uint8_t *relief = st.relief > 0 ? reliefLut : nullptr;
    for (int i = 0; i < 64; ++i) {
        // 128 = unchanged; the map's mid-grey land (about 26 of 63) stays as it is
        const float m = 1 + st.relief * ((i - 26) / 26.0f);
        reliefLut[i] = u8((m < 0.45f ? 0.45f : m > 1.6f ? 1.6f : m) * 128);
    }
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
            const uint8_t *t0 = globe_coast + v0 * TW, *t1 = globe_coast + v1 * TW;
            const uint32_t a = t0[u0] * (32 - fu) + t0[u1] * fu;
            const uint32_t b = t1[u0] * (32 - fu) + t1[u1] * fu;
            const uint32_t cd = (a * (32 - fv) + b * fv) >> 10;
            const uint8_t *c = st.ramp[cd];
            int R8 = c[0], G8 = c[1], B8 = c[2];
            if (relief && cd > 134) {
                // the land takes its light and dark from the photograph: its green, nearest texel
                const int m = relief[(globe_tex[(fv < 16 ? v0 : v1) * TW + (fu < 16 ? u0 : u1)] >> 5) & 63];
                R8 = (R8 * m) >> 7; G8 = (G8 * m) >> 7; B8 = (B8 * m) >> 7;
            }
            // the graticule: the nearer of this pixel's parallel and meridian
            int ga = r.latLine[k];
            const uint16_t g = (uint16_t)((uint16_t)(lon + 64) * 12);
            const int q = r.lonQ[k];
            const int pd = (int)(((uint32_t)(g < 32768 ? g : 65536 - g) * q) >> 14);   // sixteenths of a px
            int la = q ? (W16 - pd) * 16 : 110;
            if (la > 255) la = 255;
            if (la > ga) ga = la;
            if (ga > 0) {
                ga = (ga * GA) >> 8;
                R8 += ((GR - R8) * ga) >> 8; G8 += ((GG - G8) * ga) >> 8; B8 += ((GB - B8) * ga) >> 8;
            }
            const int s = r.shade[k], h = r.haze[k], sp8 = r.spec[k];
            R8 = ((R8 * s) >> 8) + ((h * HZ_R) >> 8) + sp8;
            G8 = ((G8 * s) >> 8) + ((h * HZ_G) >> 8) + sp8;
            B8 = ((B8 * s) >> 8) + ((h * HZ_B) >> 8) + sp8;
            const int d = bay[(X0 + i) & 3];
            o[i] = pack(R8 + (d >> 1), G8 + (d >> 2), B8 + (d >> 1));
        }
    }
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
    if (r.st->chart) { draw_chart(r, off); return; }
    const int HZ_R = r.st->hz[0], HZ_G = r.st->hz[1], HZ_B = r.st->hz[2];
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

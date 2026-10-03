// Plasma: a plasma ball filling the round glass.
//
// Ported from Doug's plasma project (~/Claude/plasma). The discharge is modelled rather
// than animated: plasma_engine.cpp is its engine, and plasma_render.cpp draws it. This file
// is the LVGL side: one full-screen object whose draw event scales the renderer's
// half-resolution frame up into LVGL's draw buffer, a timer that steps and draws while the
// screen is showing, and the knob.
//
// Drawing. Nothing here goes through LVGL's canvas. A canvas would need a 424 KB
// full-resolution buffer in PSRAM that LVGL then reads back, every frame; instead the draw
// event writes straight into the band of LVGL's own (internal RAM) draw buffer it is asked
// for, doubling the 233 x 233 frame with a 2x bilinear step as it goes. The object reports
// that it covers its area, so LVGL does not paint the screen's background under it first.
//
// Refresh. Every frame changes nearly the whole disc, so there is no small dirty box to
// find. What can be saved is the corners: the frame is invalidated as twelve horizontal
// bands, each only as wide as the circle is at that height, which pushes about 15% fewer
// pixels over QSPI than the square would. LVGL does not merge them (their union is never
// smaller than their sum).
//
// Knob. A turn sets Power (the drive voltage; how many filaments there are follows from
// it), straight away, with no press to arm it: the firmware's grammar, where every turn
// goes to the app and the rock is what opens the switcher. The readout, an arc of light on
// the inside of the glass along the bottom with the number under it, shows while turning
// and fades out two seconds after the last detent. A press just shows it, so the level can
// be read without changing it. Power is kept in NVS ("capsuleradar"/"plasma_pwr"), written
// once the readout has faded rather than on every detent; the simulator keeps it in /tmp.
//
// Themes (orb_style). With no theme, and under a theme whose own colours are the ball's
// (magenta and cyan: the fork's Plasma theme), the ball is the page's and fills the glass,
// as it always did. Any other theme gives the discharge its two colours (bodies in
// accent2, ends in accent; one hue for a one-colour instrument) and gets a smaller ball in
// a rim, so the theme's backdrop shows round it: the ball keeps its own dark interior,
// which is what lets light be added at all on a pale plate. There the draw event leaves
// LVGL's buffer alone outside the ball and blends the last pixel of its edge, and only
// claims to cover areas that lie wholly inside it.
//
// Memory. Everything is taken on enter and given back on exit, all of it in PSRAM:
// engine 70 KB, accumulator 163 KB, frame 109 KB, bloom 2 x 43 KB, electrode and
// reflection sprites ~60 KB, brush state 13 KB: about 500 KB, in nine blocks, none over 163 KB.
// A theme's backdrop is orb_style's while this screen shows: up to 424 KB for a plate and
// 636 KB for a glass layer (Cold War has both: ~1.5 MB in all), freed on exit.
#include "plasma_view.h"
#include "plasma_engine.h"
#include "plasma_render.h"
#include "orb_style.h"
#include "display.h"      // orb_screen_covered()
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(ESP_PLATFORM)
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
static void *ps_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void  ps_free(void *p) { heap_caps_free(p); }
static uint32_t now_us() { return (uint32_t)esp_timer_get_time(); }
#define PLOG(...) Serial.printf(__VA_ARGS__)
#else
#include <chrono>
static void *ps_alloc(size_t n) { return malloc(n); }
static void  ps_free(void *p) { free(p); }
static uint32_t now_us() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
#define PLOG(...) printf(__VA_ARGS__)
#endif

namespace {

constexpr int SCREEN = 466;
constexpr float BALL = 100.0f;            // a themed ball's radius, render px (200 on the panel)
constexpr float RESTLESS = 0.5f;          // the page's default
constexpr float R0 = 0.17f;               // electrode radius, a fraction of the glass
constexpr float POWER_TOP = 0.85f;        // engine power at 100%: keeps MAXF headroom to fork
constexpr int   POWER_STEP = 5;           // % per detent
// Frame pacing. Start by asking for 30, then, every log period, ask for what the device
// can actually hold: twice the measured work (the flush over QSPI costs about as much again,
// as the clock's sweep found), between 33 and 100 ms. The Flight Tracker learned that a
// rate the renderer cannot meet arrives as uneven frames, which reads worse than a lower
// even one; the discharge advances by real elapsed time either way.
constexpr uint32_t FRAME_MS = 33;
constexpr uint32_t FRAME_MS_MAX = 100;
constexpr uint32_t SHOW_MS = 2000;        // the readout stays this long after the last detent...
constexpr uint32_t FADE_MS = 400;         // ...then fades over this
constexpr uint32_t LOG_MS = 5000;
constexpr int BANDS = 12;
constexpr int BANDS_THEMED = 8;           // each split in three: see invalidate_disc()

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_obj = nullptr;
lv_obj_t *s_label = nullptr;
lv_timer_t *s_timer = nullptr;

plasma::Engine *s_eng = nullptr;
plasma::Render s_ren = {};
bool s_ready = false;
orb_style::Backdrop *s_backdrop = nullptr;   // the theme's plate, glass and sparks, while showing
bool s_cut = false;                       // a themed ball: draw only the ball, over the backdrop
float s_ballPx = SCREEN / 2.0f;           // its radius on the panel

int s_power = 55;                         // %, loaded in init(), saved when the readout fades
bool s_showing = false;
bool s_dirty = false;                     // Power changed since it was last saved
uint32_t s_activityMs = 0;
uint32_t s_lastMs = 0;

// measurement
uint32_t s_logAt = 0, s_frames = 0, s_simUs = 0, s_rasterUs = 0, s_drawUs = 0, s_segs = 0;

int16_t s_chordL[SCREEN], s_chordR[SCREEN];   // the panel's circle, per row

// ---- drawing -------------------------------------------------------------------------

inline uint16_t avg2(uint16_t a, uint16_t b) { return (uint16_t)((a & b) + (((a ^ b) & 0xF7DE) >> 1)); }

// Double the 233 px frame into whatever part of LVGL's buffer it asks for. Output pixel
// 2i is source i; 2i+1 is the mean of i and i+1; odd rows likewise between rows. The mean
// of RGB565 values is taken in place, which drops one bit per channel, below what the
// dithering already put there.
inline uint16_t up2(const uint16_t *r0, const uint16_t *r1, int X, bool odd) {
    constexpr int RW = plasma::RW;
    const int sx = X >> 1, sx1 = sx + 1 < RW ? sx + 1 : RW - 1;
    if (!(X & 1)) return odd ? avg2(r0[sx], r1[sx]) : r0[sx];
    return odd ? avg2(avg2(r0[sx], r0[sx1]), avg2(r1[sx], r1[sx1])) : avg2(r0[sx], r0[sx1]);
}

// a of 32 parts of fg over bg, in RGB565
inline uint16_t mix565(uint16_t bg, uint16_t fg, uint32_t a) {
    const uint32_t b = ((uint32_t)bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu;
    const uint32_t f = ((uint32_t)fg | ((uint32_t)fg << 16)) & 0x07E0F81Fu;
    const uint32_t m = ((b * (32 - a) + f * a) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(m | (m >> 16));
}

// The themed ball: only the ball is drawn. Outside it LVGL's buffer already holds the
// theme's backdrop and is left alone; the ring of pixels the silhouette passes through is
// blended by how much of each the ball covers, at the panel's resolution.
void draw_cut(lv_draw_ctx_t *dc, lv_obj_t *obj, const lv_area_t &a) {
    if (!s_ready) return;
    const int stride = lv_area_get_width(dc->buf_area);
    lv_color_t *buf = (lv_color_t *)dc->buf;
    const int ox = obj->coords.x1, oy = obj->coords.y1;
    const uint16_t *src = s_ren.out;
    constexpr int RW = plasma::RW;
    const float mid = SCREEN / 2.0f, rb = s_ballPx;
    for (int y = a.y1; y <= a.y2; ++y) {
        const int Y = y - oy;
        if (Y < 0 || Y >= SCREEN) continue;
        const float dy = Y + 0.5f - mid;
        const float ho2 = (rb + 1) * (rb + 1) - dy * dy;
        if (ho2 <= 0) continue;
        const float ho = sqrtf(ho2);
        const float hi2 = (rb - 1) * (rb - 1) - dy * dy;
        const float hi = hi2 > 0 ? sqrtf(hi2) : -1.0f;
        int xl = (int)floorf(mid - ho) + ox, xr = (int)ceilf(mid + ho) - 1 + ox;      // touched at all
        const int il = (int)ceilf(mid - hi - 0.5f) + ox, ir = (int)floorf(mid + hi - 0.5f) + ox;   // wholly inside
        if (xl < a.x1) xl = a.x1;
        if (xr > a.x2) xr = a.x2;
        const int sy = Y >> 1, sy1 = sy + 1 < RW ? sy + 1 : RW - 1;
        const uint16_t *r0 = src + sy * RW, *r1 = src + sy1 * RW;
        const bool odd = Y & 1;
        lv_color_t *o = buf + (size_t)(y - dc->buf_area->y1) * stride + (xl - dc->buf_area->x1);
        for (int x = xl; x <= xr; ++x, ++o) {
            const int X = x - ox;
            if (hi >= 0 && x >= il && x <= ir) { o->full = up2(r0, r1, X, odd); continue; }
            const float dx = X + 0.5f - mid;
            const float cov = rb + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov <= 0) continue;
            const uint16_t v = up2(r0, r1, X, odd);
            o->full = cov >= 1 ? v : mix565(o->full, v, (uint32_t)(cov * 32.0f));
        }
    }
}

void draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    lv_obj_t *obj = lv_event_get_target(e);
    const uint32_t t0 = now_us();
    lv_area_t a;
    if (!_lv_area_intersect(&a, dc->clip_area, &obj->coords)) return;
    if (s_cut) { draw_cut(dc, obj, a); s_drawUs += now_us() - t0; return; }
    const int stride = lv_area_get_width(dc->buf_area);
    lv_color_t *buf = (lv_color_t *)dc->buf;
    const int ox = obj->coords.x1, oy = obj->coords.y1;
    const uint16_t *src = s_ready ? s_ren.out : nullptr;
    constexpr int RW = plasma::RW;
    for (int y = a.y1; y <= a.y2; ++y) {
        lv_color_t *row = buf + (size_t)(y - dc->buf_area->y1) * stride + (a.x1 - dc->buf_area->x1);
        const int Y = y - oy;
        int xl = 1, xr = 0;                       // nothing to draw from: all black
        if (src && Y >= 0 && Y < SCREEN) { xl = s_chordL[Y] + ox; xr = s_chordR[Y] + ox; }
        const int sy = (Y < 0 ? 0 : Y >= SCREEN ? SCREEN - 1 : Y) >> 1, sy1 = sy + 1 < RW ? sy + 1 : RW - 1;
        const uint16_t *r0 = src ? src + sy * RW : nullptr;
        const uint16_t *r1 = src ? src + sy1 * RW : nullptr;
        const bool odd = Y & 1;
        lv_color_t *o = row;
        for (int x = a.x1; x <= a.x2; ++x, ++o) {
            if (x < xl || x > xr) { o->full = 0; continue; }
            const int X = x - ox;
            const int sx = X >> 1, sx1 = sx + 1 < RW ? sx + 1 : RW - 1;
            uint16_t v;
            if (!(X & 1)) v = odd ? avg2(r0[sx], r1[sx]) : r0[sx];
            else v = odd ? avg2(avg2(r0[sx], r0[sx1]), avg2(r1[sx], r1[sx1])) : avg2(r0[sx], r0[sx1]);
            o->full = v;
        }
    }
    s_drawUs += now_us() - t0;
}

void cover_cb(lv_event_t *e) {
    lv_cover_check_info_t *info = (lv_cover_check_info_t *)lv_event_get_param(e);
    lv_obj_t *obj = lv_event_get_target(e);
    if (info->res == LV_COVER_RES_MASKED) return;
    if (s_cut) {
        // only what lies wholly inside the ball is covered; the backdrop is needed elsewhere
        bool in = s_ready;
        const float lim = (s_ballPx - 2) * (s_ballPx - 2), mid = SCREEN / 2.0f;
        const float xs[2] = { info->area->x1 - obj->coords.x1 - mid, info->area->x2 - obj->coords.x1 + 1 - mid };
        const float ys[2] = { info->area->y1 - obj->coords.y1 - mid, info->area->y2 - obj->coords.y1 + 1 - mid };
        for (float x : xs) for (float y : ys) if (x * x + y * y > lim) in = false;
        info->res = in ? LV_COVER_RES_COVER : LV_COVER_RES_NOT_COVER;
        return;
    }
    info->res = _lv_area_is_in(info->area, &obj->coords, 0) ? LV_COVER_RES_COVER : LV_COVER_RES_NOT_COVER;
}

// A themed ball's bands stop at the ball, and each is cut in three: the middle piece lies
// wholly inside the ball, so LVGL (which asks cover_cb per piece) starts drawing there at
// this object and does not paint the backdrop under it; the two ends are where the
// silhouette runs, and get the backdrop first.
void invalidate_cut() {
    const lv_area_t &c = s_obj->coords;
    const int top = (int)floorf(SCREEN / 2.0f - s_ballPx - 1), bot = (int)ceilf(SCREEN / 2.0f + s_ballPx);
    const int h = (bot - top + 1) / BANDS_THEMED + 1;
    auto half = [](float r, int y) {
        const float dy = y + 0.5f - SCREEN / 2.0f, h2 = r * r - dy * dy;
        return h2 > 0 ? sqrtf(h2) : 0.0f;
    };
    for (int y0 = top; y0 <= bot; y0 += h) {
        const int y1 = y0 + h - 1 < bot ? y0 + h - 1 : bot;
        const int ym = (y0 <= SCREEN / 2 && y1 >= SCREEN / 2) ? SCREEN / 2 : (y1 < SCREEN / 2 ? y1 : y0);
        const int yf = abs(y0 - SCREEN / 2) > abs(y1 + 1 - SCREEN / 2) ? y0 - 1 : y1 + 1;   // the far edge
        const int hw = (int)ceilf(half(s_ballPx + 1, ym)) + 1;
        const int in = (int)floorf(half(s_ballPx - 3, yf));
        const int l = SCREEN / 2 - hw, r = SCREEN / 2 + hw - 1;
        auto inv = [&](int xa, int xb) {
            lv_area_t a = { (lv_coord_t)(c.x1 + xa), (lv_coord_t)(c.y1 + y0), (lv_coord_t)(c.x1 + xb), (lv_coord_t)(c.y1 + y1) };
            lv_obj_invalidate_area(s_obj, &a);
        };
        if (in < 40) { inv(l, r); continue; }
        inv(l, SCREEN / 2 - in - 1);
        inv(SCREEN / 2 - in, SCREEN / 2 + in - 1);
        inv(SCREEN / 2 + in, r);
    }
}

void invalidate_disc() {
    if (s_cut) { invalidate_cut(); return; }
    const lv_area_t &c = s_obj->coords;
    const int h = SCREEN / BANDS + 1;
    for (int y0 = 0; y0 < SCREEN; y0 += h) {
        const int y1 = y0 + h - 1 < SCREEN - 1 ? y0 + h - 1 : SCREEN - 1;
        // the band is as wide as the circle at its row nearest the middle
        const int ym = (y0 <= SCREEN / 2 && y1 >= SCREEN / 2) ? SCREEN / 2 : (y1 < SCREEN / 2 ? y1 : y0);
        lv_area_t a = { (lv_coord_t)(c.x1 + s_chordL[ym]), (lv_coord_t)(c.y1 + y0),
                        (lv_coord_t)(c.x1 + s_chordR[ym]), (lv_coord_t)(c.y1 + y1) };
        lv_obj_invalidate_area(s_obj, &a);
    }
}

// ---- the knob's readout ----------------------------------------------------------------

// ---- Power, kept across reboots ---------------------------------------------------------

#if !defined(ESP_PLATFORM)
const char *SIM_POWER_FILE = "/tmp/orb_sim_plasma_power";
#endif

int load_power() {
    int v = -1;
#if defined(ESP_PLATFORM)
    Preferences p;
    if (p.begin("capsuleradar", true)) { v = p.getInt("plasma_pwr", -1); p.end(); }
#else
    if (FILE *f = fopen(SIM_POWER_FILE, "r")) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
#endif
    return (v >= 0 && v <= 100) ? v : 55;
}

void save_power() {
    if (!s_dirty) return;
    s_dirty = false;
#if defined(ESP_PLATFORM)
    Preferences p;
    if (p.begin("capsuleradar", false)) { p.putInt("plasma_pwr", s_power); p.end(); }
#else
    if (FILE *f = fopen(SIM_POWER_FILE, "w")) { fprintf(f, "%d\n", s_power); fclose(f); }
#endif
}

// ---- the knob's readout ----------------------------------------------------------------

void show_readout() {
    if (!s_label) return;
    char b[24];
    snprintf(b, sizeof b, "POWER  %d", s_power);
    lv_label_set_text(s_label, b);
    lv_obj_set_style_opa(s_label, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_label, LV_OBJ_FLAG_HIDDEN);
    s_ren.ring = s_power / 100.0f;
    s_ren.ringA = 1;
    s_showing = true;
    s_activityMs = lv_tick_get();
}

void hide_readout() {
    if (s_label) lv_obj_add_flag(s_label, LV_OBJ_FLAG_HIDDEN);
    s_ren.ring = -1;
    s_showing = false;
    save_power();
}

// Called every frame: hold, then fade, then hide.
void fade_readout(uint32_t ms) {
    if (!s_showing) return;
    const uint32_t age = ms - s_activityMs;
    if (age < SHOW_MS) return;
    if (age >= SHOW_MS + FADE_MS) { hide_readout(); return; }
    const float k = 1.0f - (float)(age - SHOW_MS) / FADE_MS;
    s_ren.ringA = k;
    if (s_label) lv_obj_set_style_opa(s_label, (lv_opa_t)(k * 255), 0);
}

// ---- the frame -------------------------------------------------------------------------

void tick_cb(lv_timer_t *) {
    if (!s_ready || lv_scr_act() != s_scr || orb_screen_covered()) return;
    const uint32_t ms = lv_tick_get();
    float dt = (ms - s_lastMs) / 1000.0f;
    s_lastMs = ms;
    if (dt > 0.05f) dt = 0.05f;           // a hitch is not a lurch
    if (dt <= 0) return;

    fade_readout(ms);

    const uint32_t t0 = now_us();
    plasma::step(*s_eng, dt, s_power / 100.0f * POWER_TOP, RESTLESS);
    const uint32_t t1 = now_us();
    plasma::render_frame(s_ren, *s_eng);
    const uint32_t t2 = now_us();
    invalidate_disc();

    s_simUs += t1 - t0;
    s_rasterUs += t2 - t1;
    s_segs += s_ren.segs;
    s_frames++;
    if (ms - s_logAt >= LOG_MS) {
        const float secs = (ms - s_logAt) / 1000.0f;
        const float n = s_frames ? (float)s_frames : 1.0f;
        const float work = (s_simUs + s_rasterUs + s_drawUs) / n / 1000.0f;
        uint32_t period = (uint32_t)(work * 2.0f + 0.5f);
        if (period < FRAME_MS) period = FRAME_MS;
        if (period > FRAME_MS_MAX) period = FRAME_MS_MAX;
        if (s_timer) lv_timer_set_period(s_timer, period);
        PLOG("[plasma] %.1f fps, %d segs, %d channels, power %d%% | sim %.1f ms, render %.1f ms, draw %.1f ms per frame -> asking every %u ms\n",
             s_frames / secs, (int)(s_segs / n), plasma::live_count(*s_eng), s_power,
             s_simUs / n / 1000.0f, s_rasterUs / n / 1000.0f, s_drawUs / n / 1000.0f, (unsigned)period);
        s_logAt = ms;
        s_frames = s_simUs = s_rasterUs = s_drawUs = s_segs = 0;
    }
}

// ---- the theme ---------------------------------------------------------------------------

void hsl_of(uint32_t rgb, float *h, float *s, float *l) {
    const float r = ((rgb >> 16) & 255) / 255.0f, g = ((rgb >> 8) & 255) / 255.0f, b = (rgb & 255) / 255.0f;
    const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *l = (mx + mn) / 2;
    *s = d <= 0 ? 0 : d / (1 - fabsf(2 * *l - 1) + 1e-6f);
    float hh = 0;
    if (d > 0) {
        if (mx == r) hh = fmodf((g - b) / d, 6.0f);
        else if (mx == g) hh = (b - r) / d + 2;
        else hh = (r - g) / d + 4;
    }
    hh *= 60;
    *h = hh < 0 ? hh + 360 : hh;
}

// The ball for a look. See the note on themes at the top.
plasma::Palette palette_for(const orb_style::Look &l) {
    plasma::Palette p;                       // the page's own
    const orb_style::Look plain;
    const bool isPlain = l.text == plain.text && l.accent == plain.accent && l.accent2 == plain.accent2
                         && l.bg == plain.bg && !l.plate[0] && !l.overlay[0] && !l.mono && l.dark;
    float ha, sa, la, hb, sb, lb;
    hsl_of(l.accent, &ha, &sa, &la);
    hsl_of(l.accent2, &hb, &sb, &lb);
    // a theme in the ball's own colours, pink and blue: it is already dressed
    const bool own = l.dark && !l.mono && ha >= 285 && ha <= 345 && sa > 0.5f && hb >= 170 && hb <= 260 && sb > 0.5f;
    if (isPlain || own) return p;
    p.native = false;
    p.endHue = ha; p.bodyHue = hb;
    p.endSat = 0.55f + 0.45f * sa; p.bodySat = 0.55f + 0.45f * sb;
    p.spread = 10;
    if (l.mono) {
        // one colour: both ends of a channel in the hue between the theme's two
        float d = hb - ha;
        if (d > 180) d -= 360;
        if (d < -180) d += 360;
        p.endHue = p.bodyHue = fmodf(ha + d / 2 + 360, 360.0f);
        p.endSat = p.bodySat = 0.85f;
        p.spread = 3;
    }
    p.ball = BALL;
    // The rim. Ink on paper, and themes with a warm metal for an accent, get a band of
    // that metal; the rest a drawn line.
    const bool brass = ha >= 18 && ha <= 62 && sa > 0.3f;
    uint32_t rim = l.dim;
    if (!l.dark) { p.rimW = 4.5f; rim = l.accent2; }
    else if (brass && !l.mono) { p.rimW = 4.5f; rim = l.accent; }
    else { p.rimW = 1.4f; rim = l.mono ? l.accent2 : l.dim; }
    p.rim[0] = ((rim >> 16) & 255) / 255.0f; p.rim[1] = ((rim >> 8) & 255) / 255.0f; p.rim[2] = (rim & 255) / 255.0f;
    p.bloom = 0.7f + 0.1f * l.glow;
    p.gas = l.dark ? 1.0f : 0.5f;            // set into a pale page, the glass reads as dark
    return p;
}

// The readout is read against the ball's dark interior whatever the theme's ground is, so
// on an ink-on-paper theme it takes the paper's colour, not the ink's.
void style_readout(const orb_style::Look &l, bool native) {
    if (!s_label) return;
    lv_obj_set_style_text_font(s_label, orb_style::font(orb_style::SMALL, 14), 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(native ? 0xF4E6FF : l.dark ? l.text : l.bg), 0);
    lv_obj_align(s_label, LV_ALIGN_BOTTOM_MID, 0, -40 - (int)lroundf((SCREEN / 2.0f - s_ballPx) * 1.15f));
}

void free_all() {
    s_ready = false;
    plasma::render_free(s_ren);
    if (s_eng) { ps_free(s_eng); s_eng = nullptr; }
}

} // namespace

namespace plasmaview {

void init() {
    if (s_scr) return;
    s_power = load_power();
    for (int y = 0; y < SCREEN; ++y) {
        const float dy = y + 0.5f - SCREEN / 2.0f;
        const float h2 = (SCREEN / 2.0f) * (SCREEN / 2.0f) - dy * dy;
        const float hw = h2 > 0 ? sqrtf(h2) : 0;
        s_chordL[y] = (int16_t)floorf(SCREEN / 2.0f - hw);
        s_chordR[y] = (int16_t)ceilf(SCREEN / 2.0f + hw) - 1;
    }

    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_obj = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_obj);
    lv_obj_set_size(s_obj, SCREEN, SCREEN);
    lv_obj_center(s_obj);
    lv_obj_clear_flag(s_obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_obj, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(s_obj, cover_cb, LV_EVENT_COVER_CHECK, nullptr);

    // Under the arc, inside the chord, small and spaced: a reading, not a headline.
    s_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xF4E6FF), 0);
    lv_obj_set_style_text_letter_space(s_label, 2, 0);
    lv_obj_set_style_bg_color(s_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_label, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_label, 10, 0);
    lv_obj_set_style_pad_hor(s_label, 10, 0);
    lv_obj_set_style_pad_ver(s_label, 3, 0);
    lv_label_set_text(s_label, "POWER");
    lv_obj_align(s_label, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_flag(s_label, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *screen() { return s_scr; }

void onEnter() {
    if (!s_scr) return;
    const orb_style::Look &look = orb_style::look("plasma");
    const plasma::Palette pal = palette_for(look);
    s_cut = !pal.native;
    s_ballPx = pal.native ? SCREEN / 2.0f : pal.ball * 2.0f;
    style_readout(look, pal.native);
    if (!s_backdrop) s_backdrop = orb_style::attach(s_scr, "plasma");
    if (!s_ready) {
        s_eng = (plasma::Engine *)ps_alloc(sizeof(plasma::Engine));
        if (!s_eng || !plasma::render_alloc(s_ren, pal, R0)) {
            PLOG("[plasma] PSRAM alloc failed; screen stays dark\n");
            free_all();
            return;
        }
        plasma::init(*s_eng, R0, 0x1234567u ^ lv_tick_get());
        // a few steps first, so the first frame is a ball rather than a flash of spokes
        for (int i = 0; i < 6; ++i) plasma::step(*s_eng, 1.0f / 30, s_power / 100.0f * POWER_TOP, RESTLESS);
        s_ready = true;
    }
    hide_readout();
    s_lastMs = s_logAt = s_activityMs = lv_tick_get();
    s_frames = s_simUs = s_rasterUs = s_drawUs = s_segs = 0;
    if (!s_timer) s_timer = lv_timer_create(tick_cb, FRAME_MS, nullptr);
    PLOG("[plasma] enter: ~%u KB of PSRAM taken\n",
         (unsigned)((sizeof(plasma::Engine) + plasma::RW * plasma::RW * 5 + plasma::LG * plasma::LG * 24
                     + s_ren.elecS * s_ren.elecS * 7 + s_ren.reflW * s_ren.reflH
                     + plasma::MAXF * plasma::FIBMAX * plasma::FIB * 4 + 1025 * 3) / 1024));
}

void onExit() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    hide_readout();   // and saves Power if it changed
    free_all();
    orb_style::release(s_backdrop);
    // nothing left to draw from: the next frame of this screen, if any, is black
    if (s_obj) lv_obj_invalidate(s_obj);
}

// Press: show the level without changing it. (A turn is what changes it.)
void onPress() {
    if (!s_ready) return;
    show_readout();
}

// A detent: Power, at once, whenever this screen is up.
void onTurn(int delta) {
    if (!s_ready) return;
    const int was = s_power;
    s_power += delta * POWER_STEP;
    if (s_power < 0) s_power = 0;
    if (s_power > 100) s_power = 100;
    if (s_power != was) s_dirty = true;
    show_readout();
}

} // namespace plasmaview

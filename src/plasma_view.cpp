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
// Knob. The Flight Tracker's old grammar: a press takes the knob, turning then sets Power
// (the drive voltage; how many filaments there are follows from it), and a press or five
// idle seconds gives it back. The readout is an arc of light on the inside of the glass
// along the bottom, with the number under it.
//
// Memory. Everything is taken on enter and given back on exit, all of it in PSRAM:
// engine ~110 KB, accumulator 163 KB, frame 109 KB, bloom 2 x 43 KB, sprites ~40 KB.
#include "plasma_view.h"
#include "plasma_engine.h"
#include "plasma_render.h"
#include "app_shell.h"
#include "display.h"      // orb_screen_covered()
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(ESP_PLATFORM)
#include <Arduino.h>
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
constexpr float HUE = 288.0f;             // the page's default: blue bodies, pink ends
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
constexpr uint32_t IDLE_MS = 5000;        // captured knob lets go after this (radar's SELECT_IDLE_MS)
constexpr uint32_t LOG_MS = 5000;
constexpr int BANDS = 12;

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_obj = nullptr;
lv_obj_t *s_label = nullptr;
lv_timer_t *s_timer = nullptr;

plasma::Engine *s_eng = nullptr;
plasma::Render s_ren = {};
bool s_ready = false;

int s_power = 55;                         // %, kept across visits (not across boots)
bool s_captured = false;
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
void draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    lv_obj_t *obj = lv_event_get_target(e);
    const uint32_t t0 = now_us();
    lv_area_t a;
    if (!_lv_area_intersect(&a, dc->clip_area, &obj->coords)) return;
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
    info->res = _lv_area_is_in(info->area, &obj->coords, 0) ? LV_COVER_RES_COVER : LV_COVER_RES_NOT_COVER;
}

void invalidate_disc() {
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

void show_readout() {
    if (!s_label) return;
    char b[24];
    snprintf(b, sizeof b, "POWER  %d", s_power);
    lv_label_set_text(s_label, b);
    lv_obj_clear_flag(s_label, LV_OBJ_FLAG_HIDDEN);
    s_ren.ring = s_power / 100.0f;
}

void hide_readout() {
    if (s_label) lv_obj_add_flag(s_label, LV_OBJ_FLAG_HIDDEN);
    s_ren.ring = -1;
}

void release() {
    s_captured = false;
    app_shell::setCaptured(false);
    hide_readout();
}

// ---- the frame -------------------------------------------------------------------------

void tick_cb(lv_timer_t *) {
    if (!s_ready || lv_scr_act() != s_scr || orb_screen_covered()) return;
    const uint32_t ms = lv_tick_get();
    float dt = (ms - s_lastMs) / 1000.0f;
    s_lastMs = ms;
    if (dt > 0.05f) dt = 0.05f;           // a hitch is not a lurch
    if (dt <= 0) return;

    if (s_captured && ms - s_activityMs >= IDLE_MS) release();

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

void free_all() {
    s_ready = false;
    plasma::render_free(s_ren);
    if (s_eng) { ps_free(s_eng); s_eng = nullptr; }
}

} // namespace

namespace plasmaview {

void init() {
    if (s_scr) return;
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
    s_captured = false;
    app_shell::setCaptured(false);
    if (!s_ready) {
        s_eng = (plasma::Engine *)ps_alloc(sizeof(plasma::Engine));
        if (!s_eng || !plasma::render_alloc(s_ren, HUE, R0)) {
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
    PLOG("[plasma] enter: %u bytes of PSRAM taken\n",
         (unsigned)(sizeof(plasma::Engine) + plasma::RW * plasma::RW * 5 + plasma::LG * plasma::LG * 24));
}

void onExit() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    if (s_captured) release();
    hide_readout();
    free_all();
    // nothing left to draw from: the next frame of this screen, if any, is black
    if (s_obj) lv_obj_invalidate(s_obj);
}

// Press: take the knob to set Power, or give it back.
void onPress() {
    if (!s_ready) return;
    if (s_captured) { release(); return; }
    s_captured = true;
    app_shell::setCaptured(true);
    s_activityMs = lv_tick_get();
    show_readout();
}

// A detent: Power, while the knob is ours. Uncaptured turns are the shell's business.
void onTurn(int delta) {
    if (!s_captured) return;
    s_power += delta * POWER_STEP;
    if (s_power < 0) s_power = 0;
    if (s_power > 100) s_power = 100;
    s_activityMs = lv_tick_get();
    show_readout();
}

} // namespace plasmaview

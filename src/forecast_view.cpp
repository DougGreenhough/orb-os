// Forecast screen. See forecast_view.h.
//
// Layout is worked out against the round glass, not the square buffer: every row sits
// inside the chord of the 233 px circle at its own height, which is why the day strip is
// four narrow columns rather than a wider row, and why the rain line under it is the
// smallest type on the screen.
//
// The icons are built from LVGL circles and lines rather than bitmaps, so they cost no
// theme art, no flash and no PSRAM, and scale to any size. Each refresh throws the old
// ones away and builds new ones, which is a few dozen objects every fifteen minutes.
//
// Dress: every colour and typeface comes from orb_style::look("forecast"), worked into a
// Palette below. With no theme the palette is the one this screen was first drawn in. On a
// light plate the pictures get ink outlines; on a one-colour instrument they are drawn in
// its colour alone, the fair-weather cloud as an outline; where the look glows, the shapes
// carry a shadow of their own colour and the temperature is set through curved_text into a
// small canvas (43 KB, taken on entry and given back on exit) so that it can glow too.
#include "forecast_view.h"
#include "weather.h"
#include "ui.h"
#include "orb_style.h"
#include "font_ladder.h"
#include "curved_text.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace {

constexpr int SCREEN = 466;
constexpr int MID    = SCREEN / 2;

// What the pictures and the type are drawn in. Filled from the look by palette_from().
struct Palette {
    uint32_t bg, text, dim, rule;
    uint32_t sun, ray, cloud, grey, rain, snow, bolt, fog;
    uint32_t ink;        // outline colour, where outline > 0
    uint32_t hollow;     // what an outlined fair-weather cloud is filled with
    float    outline;    // outline weight as a fraction of the icon's size; 0 = none
    bool     hollowCloud;
    bool     inked;      // every shape outlined in ink (a light plate)
    bool     mono;
    uint32_t haloText;   // what glowing text glows in
    uint8_t  glow;       // 0..3
};
Palette s_pal;

uint32_t mix(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int sh) { return (uint32_t)lroundf(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t) << sh; };
    return ch(16) | ch(8) | ch(0);
}

// Hue in degrees and saturation 0..1, for telling a colour that can be a sun from one that
// can be rain.
void hue_sat(uint32_t c, float &h, float &s) {
    const float r = ((c >> 16) & 255) / 255.0f, g = ((c >> 8) & 255) / 255.0f, b = (c & 255) / 255.0f;
    const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) { h = 0; return; }
    if (mx == r) h = 60 * fmodf((g - b) / d, 6);
    else if (mx == g) h = 60 * ((b - r) / d + 2);
    else h = 60 * ((r - g) / d + 4);
    if (h < 0) h += 360;
}
bool warm(uint32_t c) { float h, s; hue_sat(c, h, s); return s > 0.3f && (h >= 295 || h <= 62); }
bool cool(uint32_t c) { float h, s; hue_sat(c, h, s); return s > 0.25f && h >= 150 && h <= 265; }

// The look with no theme at all, which must draw exactly what this screen always drew.
bool plain(const orb_style::Look &l) {
    const orb_style::Look d;
    return l.bg == d.bg && l.text == d.text && l.dim == d.dim && l.accent == d.accent &&
           l.accent2 == d.accent2 && l.rule == d.rule && l.dark && !l.mono && !l.glow && !l.plate[0];
}

Palette palette_from(const orb_style::Look &l) {
    Palette p = {};
    p.bg = l.bg; p.text = l.text; p.dim = l.dim; p.rule = l.rule; p.glow = l.glow;
    p.ink = l.text; p.hollow = l.bg; p.mono = l.mono; p.haloText = l.accent;
    if (l.mono) {
        // One colour: brightness and outline-or-filled are all there is to tell things apart.
        p.sun = p.ray = p.rain = p.snow = p.bolt = l.accent;
        p.cloud = l.text; p.grey = l.dim; p.fog = l.dim;
        p.hollowCloud = true; p.hollow = mix(l.bg, l.rule, 0.6f);
        p.outline = 1.0f / 30.0f;
        return p;
    }
    if (!l.dark) {
        // Ink on paper: every shape is drawn round in ink and filled with a wash.
        p.sun = l.accent; p.ray = l.accent;
        p.cloud = mix(l.bg, 0xFFFFFF, 0.55f);
        p.grey  = mix(l.dim, l.bg, 0.45f);
        p.rain  = cool(l.accent2) ? l.accent2 : mix(0x1F5F96, l.text, 0.15f);
        p.snow  = p.cloud;
        p.bolt  = l.accent;
        p.fog   = l.dim;
        p.outline = 1.0f / 34.0f; p.inked = true;
        return p;
    }
    // Light on dark. The sun takes the theme's accent when that is a colour a sun can be,
    // and rain the cooler of its two; otherwise they keep their own.
    p.sun = warm(l.accent) ? l.accent : warm(l.accent2) ? l.accent2 : 0xFFB23F;
    p.ray = p.sun;
    float h, sa = 0, sb = 0;
    if (cool(l.accent))  hue_sat(l.accent, h, sa);
    if (cool(l.accent2)) hue_sat(l.accent2, h, sb);
    p.rain = (sa > 0 || sb > 0) ? (sa > sb ? l.accent : l.accent2) : 0x4FA3FF;
    p.bolt = 0xFFD84A;
    p.fog  = l.dim;
    if (plain(l)) { p.cloud = 0xD8DEE9; p.grey = 0x7C8594; p.snow = 0xCFE8FF; }
    else { p.cloud = mix(l.text, l.dim, 0.12f); p.grey = mix(l.dim, l.bg, 0.12f); p.snow = mix(p.rain, l.text, 0.7f); }
    if (l.glow >= 3) {
        // Neon: tubes, not fills. The fair-weather cloud is bent glass with nothing in it.
        p.hollowCloud = true; p.hollow = l.bg; p.outline = 1.0f / 30.0f;
    }
    return p;
}

// Day strip: four columns centred on the dial, 76 px apart.
constexpr int DAYS = 4;
constexpr int DAY_X[DAYS] = { MID - 114, MID - 38, MID + 38, MID + 114 };

// The temperature's canvas, when the look glows enough to want one.
constexpr int TEMP_W = 210, TEMP_H = 68, TEMP_PAD = 10;
constexpr int TEMP_X = MID + 2, TEMP_Y = 124;   // where the label's top-left has always been

lv_obj_t *s_scr = nullptr;
orb_style::Backdrop *s_backdrop = nullptr;   // plate, glass and sparks while showing
lv_obj_t *s_body = nullptr;          // everything that needs data; hidden while there is none
lv_obj_t *s_empty = nullptr;         // what shows instead
lv_obj_t *s_updated = nullptr;
lv_obj_t *s_nowIcon = nullptr;
lv_obj_t *s_temp = nullptr;
lv_obj_t *s_tempCanvas = nullptr;
uint8_t  *s_tempBuf = nullptr;
lv_obj_t *s_cond = nullptr;
lv_obj_t *s_caption[3] = {};
lv_obj_t *s_metric[3] = {};
lv_obj_t *s_rule = nullptr;
lv_obj_t *s_dayName[DAYS] = {};
lv_obj_t *s_dayIcon[DAYS] = {};
lv_obj_t *s_dayTemps[DAYS] = {};
lv_obj_t *s_dayRain[DAYS] = {};

// lv_line keeps a pointer to its points rather than a copy, so they live here. Every icon
// is rebuilt together in refresh(), after the old lines are deleted, so the pool is simply
// rewound at the start of each rebuild.
lv_point_t s_pts[512];
int s_ptsUsed = 0;

// ---- small builders ------------------------------------------------------------------

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *blank(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// A shadow of the shape's own colour, which is all a glow is. `r` is the shape's radius;
// the halo scales with it so a 40 px icon and a 112 px one glow alike.
void glow_on(lv_obj_t *o, float r, uint32_t color) {
    if (!s_pal.glow) return;
    static const float   reach[4] = { 0, 0.45f, 0.9f, 1.25f };
    static const lv_opa_t opa[4]  = { 0, 70, 120, 170 };
    int w = (int)lroundf(r * reach[s_pal.glow]);
    if (w < 3) w = 3;
    if (w > 36) w = 36;
    lv_obj_set_style_shadow_width(o, w, 0);
    lv_obj_set_style_shadow_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(o, opa[s_pal.glow], 0);
}

lv_obj_t *circle(lv_obj_t *parent, float cx, float cy, float r, uint32_t color) {
    lv_obj_t *c = blank(parent);
    const int d = (int)lroundf(r * 2);
    lv_obj_set_size(c, d, d);
    lv_obj_set_pos(c, (int)lroundf(cx - r), (int)lroundf(cy - r));
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    return c;
}

lv_obj_t *pill(lv_obj_t *parent, float x, float y, float w, float h, uint32_t color) {
    lv_obj_t *c = blank(parent);
    lv_obj_set_size(c, (int)lroundf(w), (int)lroundf(h));
    lv_obj_set_pos(c, (int)lroundf(x), (int)lroundf(y));
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    return c;
}

void stroke(lv_obj_t *parent, lv_point_t *p, int n, float width, uint32_t color, lv_opa_t opa) {
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, p, n);
    lv_obj_set_style_line_width(l, (lv_coord_t)fmaxf(1.0f, width), 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_opa(l, opa, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
}

void line(lv_obj_t *parent, const float *xy, int n, float width, uint32_t color) {
    if (s_ptsUsed + n > (int)(sizeof(s_pts) / sizeof(s_pts[0]))) return;   // out of pool: skip the stroke, not the screen
    lv_point_t *p = &s_pts[s_ptsUsed];
    for (int i = 0; i < n; ++i) {
        p[i].x = (lv_coord_t)lroundf(xy[2 * i]);
        p[i].y = (lv_coord_t)lroundf(xy[2 * i + 1]);
    }
    s_ptsUsed += n;
    // A line has no shadow to give, so where the look glows hard it gets a wider, fainter
    // stroke of itself underneath.
    if (s_pal.glow >= 2) stroke(parent, p, n, width * (s_pal.glow == 3 ? 2.6f : 2.2f), color, s_pal.glow == 3 ? 80 : 55);
    stroke(parent, p, n, width, color, LV_OPA_COVER);
}

// ---- icons ---------------------------------------------------------------------------

enum Kind { K_CLEAR, K_PARTLY, K_CLOUD, K_FOG, K_RAIN, K_SNOW, K_STORM, K_UNKNOWN };

// The same WMO buckets weather_condition() names, so the picture and the word agree.
Kind kind_of(int code) {
    if (code == 0) return K_CLEAR;
    if (code == 1 || code == 2) return K_PARTLY;
    if (code == 3) return K_CLOUD;
    if (code == 45 || code == 48) return K_FOG;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return K_RAIN;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return K_SNOW;
    if (code >= 95) return K_STORM;
    return K_UNKNOWN;
}

// Outline weight for an icon of size s, or 0 where the look draws none.
float edge_w(float s) { return s_pal.outline > 0 ? fmaxf(1.0f, s * s_pal.outline) : 0; }

void sun(lv_obj_t *box, float cx, float cy, float r, float s) {
    const float w = fmaxf(2.0f, s / 28.0f);
    for (int i = 0; i < 8; ++i) {
        const float a = i * (float)M_PI / 4.0f;
        const float xy[4] = { cx + cosf(a) * r * 1.4f, cy + sinf(a) * r * 1.4f,
                              cx + cosf(a) * r * 1.8f, cy + sinf(a) * r * 1.8f };
        line(box, xy, 2, w, s_pal.ray);
    }
    if (s_pal.inked) circle(box, cx, cy, r + edge_w(s), s_pal.ink);
    glow_on(circle(box, cx, cy, r, s_pal.sun), r, s_pal.sun);
}

// A cloud whose flat base spans w and whose base line is at y = base, in an icon of size s.
// `fair` is the white cloud of a fine day, as against the grey one that brings weather.
void cloud(lv_obj_t *box, float cx, float base, float w, bool fair, float s) {
    const float h = w * 0.34f;
    const uint32_t color = fair ? s_pal.cloud : s_pal.grey;
    const bool hollow = fair && s_pal.hollowCloud;
    const float e = edge_w(s);
    const float r1 = w * 0.22f, r2 = w * 0.30f;
    const float x1 = cx - w * 0.16f, y1 = base - h * 0.95f, x2 = cx + w * 0.12f, y2 = base - h * 1.05f;
    if (e > 0 && (hollow || s_pal.inked)) {
        // The outline is the same three shapes a little larger, underneath.
        const uint32_t edge = hollow ? color : s_pal.ink;
        lv_obj_t *a = circle(box, x1, y1, r1 + e, edge);
        lv_obj_t *b = circle(box, x2, y2, r2 + e, edge);
        lv_obj_t *c = pill(box, cx - w / 2 - e, base - h - e, w + 2 * e, h + 2 * e, edge);
        if (hollow) { glow_on(a, r1, edge); glow_on(b, r2, edge); glow_on(c, h / 2, edge); }
    }
    const uint32_t fill = hollow ? s_pal.hollow : color;
    lv_obj_t *a = circle(box, x1, y1, r1, fill);
    lv_obj_t *b = circle(box, x2, y2, r2, fill);
    lv_obj_t *c = pill(box, cx - w / 2, base - h, w, h, fill);
    if (!hollow && !s_pal.inked) { glow_on(a, r1 * 0.6f, color); glow_on(b, r2 * 0.6f, color); glow_on(c, h * 0.3f, color); }
}

// Build the icon for `code` inside `box`, an s x s square.
void draw_icon(lv_obj_t *box, int code, float s) {
    lv_obj_clean(box);
    const float c = s / 2;
    const float w = fmaxf(1.5f, s / 26.0f);
    switch (kind_of(code)) {
    case K_CLEAR:
        sun(box, c, c, s * 0.2f, s);
        break;
    case K_PARTLY:
        sun(box, s * 0.62f, s * 0.36f, s * 0.15f, s * 0.8f);
        cloud(box, s * 0.44f, s * 0.8f, s * 0.7f, true, s);
        break;
    case K_CLOUD:
        cloud(box, c, s * 0.72f, s * 0.84f, false, s);
        break;
    case K_FOG:
        for (int i = 0; i < 3; ++i) {
            const float y = s * (0.38f + 0.16f * i), inset = (i == 1) ? 0.12f : 0.2f;
            const float xy[4] = { s * inset, y, s * (1 - inset), y };
            line(box, xy, 2, w * 1.4f, s_pal.fog);
        }
        break;
    case K_RAIN:
        cloud(box, c, s * 0.6f, s * 0.8f, false, s);
        for (int i = 0; i < 3; ++i) {
            const float x = s * (0.34f + 0.16f * i);
            const float xy[4] = { x, s * 0.7f, x - s * 0.06f, s * 0.9f };
            line(box, xy, 2, w * 1.2f, s_pal.rain);
        }
        break;
    case K_SNOW:
        cloud(box, c, s * 0.6f, s * 0.8f, true, s);
        for (int i = 0; i < 3; ++i) {
            const float x = s * (0.32f + 0.18f * i), y = s * (i == 1 ? 0.86f : 0.76f), r = fmaxf(1.5f, s * 0.045f);
            if (s_pal.inked) circle(box, x, y, r + edge_w(s), s_pal.ink);
            glow_on(circle(box, x, y, r, s_pal.snow), r * 2, s_pal.snow);
        }
        break;
    case K_STORM: {
        cloud(box, c, s * 0.58f, s * 0.8f, false, s);
        const float xy[8] = { s * 0.54f, s * 0.6f, s * 0.42f, s * 0.78f,
                              s * 0.56f, s * 0.78f, s * 0.44f, s * 0.96f };
        line(box, xy, 4, w * 1.3f, s_pal.bolt);
        break;
    }
    case K_UNKNOWN:
        break;   // no picture beats a wrong one; the word underneath still says "Unknown"
    }
}

lv_obj_t *icon_box(lv_obj_t *parent, int cx, int cy, int s) {
    lv_obj_t *b = blank(parent);
    lv_obj_set_size(b, s, s);
    lv_obj_set_pos(b, cx - s / 2, cy - s / 2);
    lv_obj_add_flag(b, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   // a glow reaches past the box
    return b;
}

// ---- dress ---------------------------------------------------------------------------

// Every character of `s` has a glyph in f itself.
bool covers(const lv_font_t *f, const char *s) {
    lv_font_glyph_dsc_t g;
    for (; *s; ++s) if (*s != ' ' && !f->get_glyph_dsc(f, &g, (uint32_t)(uint8_t)*s, 0)) return false;
    return true;
}

// A theme's faces are baked as plain ASCII, and a forecast is full of degree signs. This is
// the same face with Montserrat behind it for whatever it lacks: a copy of the small font
// header (the glyphs stay where they are), because the theme's own must not be altered
// under the screens it was made for.
const lv_font_t *backed(const lv_font_t *f, const lv_font_t *behind) {
    static lv_font_t pool[8];
    static const lv_font_t *from[8];
    static int n = 0;
    for (int i = 0; i < n; ++i) if (from[i] == f && pool[i].fallback == behind) return &pool[i];
    if (n == 8) return behind;
    pool[n] = *f;
    pool[n].fallback = behind;
    from[n] = f;
    return &pool[n++];
}

int text_w(const char *s, const lv_font_t *f) {
    lv_point_t sz;
    lv_txt_get_size(&sz, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

// The typeface for one line of this screen: the theme's when it has one near `px` that can
// set `needs` and whose widest likely string, `worst`, stays inside maxW; else Montserrat.
const lv_font_t *face(orb_style::Role role, int px, const char *needs, const char *worst, int maxW) {
    const lv_font_t *m = font_ladder(px);
    const lv_font_t *f = orb_style::font(role, px);
    if (f == m || !covers(f, needs)) return m;
    f = backed(f, m);
    return text_w(worst, f) <= maxW ? f : m;
}

constexpr char ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr char DIGITS[] = "0123456789-%";

// Put a label where its Montserrat original sat: centred on cx, and vertically centred on
// the line a Montserrat label of `px` with its top at y would occupy, whatever face it is
// actually in.
void dress(lv_obj_t *l, const lv_font_t *f, int px, uint32_t color, int cx, int y, int w) {
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    const int dy = (lv_font_get_line_height(font_ladder(px)) - lv_font_get_line_height(f)) / 2;
    if (w > 0) { lv_obj_set_width(l, w); lv_obj_set_pos(l, cx - w / 2, y + dy); }
    else lv_obj_set_pos(l, cx, y + dy);
}

// Colours, faces and positions from the look. Cheap, and the look only changes across a
// restart, so this runs in init() and again each time the screen is entered.
void restyle() {
    const orb_style::Look &l = orb_style::look("forecast");
    s_pal = palette_from(l);
    using namespace orb_style;

    lv_obj_set_style_bg_color(s_scr, lv_color_hex(s_pal.bg), 0);

    // The caption face a size down, if the theme's is too small to pass for 14 px.
    const lv_font_t *upd = face(SMALL, 14, "UPDATED:0123456789", "UPDATED 00:00", 220);
    if (upd == font_ladder(14) && themed_font(SMALL, 12) && covers(font(SMALL, 12), "UPDATED:0123456789")) upd = font(SMALL, 12);
    dress(s_updated, upd, 14, s_pal.dim, MID, 62, 220);
    dress(s_temp, font_ladder(48), 48, s_pal.text, TEMP_X, TEMP_Y, 0);
    dress(s_cond, face(BODY, 22, ALPHA, "Thunderstorm", 300), 22, s_pal.text, MID, 214, 320);

    // The three readings. A theme's face is usually wider than the Montserrat these columns
    // were drawn for, so they spread to suit it, as far as the glass allows at this height.
    const lv_font_t *cap = face(SMALL, 12, "FEELSHUMIDITYWIND", "HUMIDITY", 100);
    const lv_font_t *val = face(BODY, 18, "NESWkmph/0123456789-%", "NW 88 km/h", 130);
    int pitch = 108;
    if (val != font_ladder(18)) {
        pitch = text_w("NW 88 km/h", val) + 14;
        if (pitch < 108) pitch = 108;
        if (pitch > 136) pitch = 136;
    }
    for (int i = 0; i < 3; ++i) {
        const int cx = MID + (i - 1) * pitch;
        dress(s_caption[i], cap, 12, s_pal.dim, cx, 250, pitch - 4);
        dress(s_metric[i], val, 18, s_pal.text, cx, 266, pitch - 4);
    }
    lv_obj_set_style_line_color(s_rule, lv_color_hex(s_pal.rule), 0);

    // The day strip cannot spread (the glass is narrowing), so it takes the theme's face
    // only when a cold week still fits its columns, and takes it for names and numbers
    // together or not at all.
    const lv_font_t *name = face(BODY, 16, ALPHA, "Today", 72);
    const lv_font_t *temps = face(BODY, 16, DIGITS, "-10\xC2\xB0 -12\xC2\xB0", 76);
    if (name == font_ladder(16) || temps == font_ladder(16)) name = temps = font_ladder(16);
    const lv_font_t *rain = face(SMALL, 12, DIGITS, "100%", 70);
    for (int d = 0; d < DAYS; ++d) {
        dress(s_dayName[d], name, 16, s_pal.text, DAY_X[d], 316, 78);
        dress(s_dayTemps[d], temps, 16, s_pal.text, DAY_X[d], 382, 78);
        dress(s_dayRain[d], rain, 12, s_pal.mono ? s_pal.dim : s_pal.rain, DAY_X[d], 403, 78);
    }
    dress(s_empty, face(BODY, 18, ALPHA, "W", 300), 18, s_pal.dim, MID, MID - 30, 300);
}

// ---- the temperature, glowing ----------------------------------------------------------

void temp_canvas_take() {
    if (s_tempBuf || s_pal.glow < 2) return;
    const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(TEMP_W, TEMP_H);
#if defined(ESP_PLATFORM)
    s_tempBuf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
    s_tempBuf = (uint8_t *)malloc(sz);
#endif
    if (!s_tempBuf) return;   // the plain label stays, and nothing else changes
    memset(s_tempBuf, 0, sz);
    s_tempCanvas = lv_canvas_create(s_body);
    lv_canvas_set_buffer(s_tempCanvas, s_tempBuf, TEMP_W, TEMP_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_obj_set_pos(s_tempCanvas, TEMP_X - TEMP_PAD, TEMP_Y - TEMP_PAD);
    lv_obj_clear_flag(s_tempCanvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

void temp_canvas_give() {
    if (s_tempCanvas) { lv_obj_del(s_tempCanvas); s_tempCanvas = nullptr; }
    if (s_tempBuf) {
#if defined(ESP_PLATFORM)
        heap_caps_free(s_tempBuf);
#else
        free(s_tempBuf);
#endif
        s_tempBuf = nullptr;
    }
    if (s_temp) lv_obj_clear_flag(s_temp, LV_OBJ_FLAG_HIDDEN);
}

// `latin1` is the temperature with the degree sign as the single byte 0xB0, which is how
// curved_text reads a string.
void temp_draw(const char *utf8, const char *latin1) {
    lv_label_set_text(s_temp, utf8);
    if (!s_tempBuf) { lv_obj_clear_flag(s_temp, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_add_flag(s_temp, LV_OBJ_FLAG_HIDDEN);
    memset(s_tempBuf, 0, LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(TEMP_W, TEMP_H));
    const lv_font_t *f = font_ladder(48);
    const curved_text::Target dst = { s_tempBuf, TEMP_W, TEMP_H };
    const uint32_t halo = s_pal.mono ? s_pal.dim : s_pal.haloText;
    curved_text::draw_straight(dst, f, latin1, (float)TEMP_PAD, TEMP_PAD + lv_font_get_line_height(f) / 2.0f,
                               lv_color_hex(s_pal.text), s_pal.glow == 3 ? 8 : 6, lv_color_hex(halo), 0);
    lv_obj_invalidate(s_tempCanvas);
}

// ---- values --------------------------------------------------------------------------

float temp(float c) { return ui_wx_imperial() ? c * 1.8f + 32.0f : c; }
float wind(float kmh) { return ui_wx_imperial() ? kmh * 0.621371f : kmh; }

const char *compass(int deg) {
    static const char *pts[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return pts[((deg % 360 + 360) % 360 + 22) / 45 % 8];
}

} // namespace

namespace forecastview {

void init() {
    if (s_scr) return;
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_body = blank(s_scr);
    lv_obj_set_size(s_body, SCREEN, SCREEN);

    // Faces, colours and exact positions are restyle()'s; this only makes the objects.
    const lv_font_t *f = font_ladder(16);
    s_updated = label(s_body, f, 0);

    // Current conditions: icon left of centre, temperature right of it, word underneath.
    s_nowIcon = icon_box(s_body, MID - 72, 150, 112);
    s_temp = label(s_body, f, 0);
    lv_obj_set_style_text_align(s_temp, LV_TEXT_ALIGN_LEFT, 0);
    s_cond = label(s_body, f, 0);

    static const char *captions[3] = { "FEELS", "HUMIDITY", "WIND" };
    for (int i = 0; i < 3; ++i) {
        s_caption[i] = label(s_body, f, 0);
        lv_label_set_text(s_caption[i], captions[i]);
        s_metric[i] = label(s_body, f, 0);
    }

    static lv_point_t rule[2] = { { 0, 0 }, { 300, 0 } };
    s_rule = lv_line_create(s_body);
    lv_line_set_points(s_rule, rule, 2);
    lv_obj_set_pos(s_rule, MID - 150, 304);
    lv_obj_set_style_line_width(s_rule, 1, 0);

    for (int d = 0; d < DAYS; ++d) {
        s_dayName[d]  = label(s_body, f, 0);
        s_dayIcon[d]  = icon_box(s_body, DAY_X[d], 358, 40);
        s_dayTemps[d] = label(s_body, f, 0);
        lv_label_set_recolor(s_dayTemps[d], true);
        s_dayRain[d]  = label(s_body, f, 0);
    }

    s_empty = label(s_scr, f, 0);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_empty, "No forecast yet\n\nIt arrives over WiFi a minute or so after the Orb has a location.");

    restyle();
    refresh();
}

lv_obj_t *screen() { return s_scr; }

void onEnter() {
    restyle();
    temp_canvas_take();
    refresh();
    s_backdrop = orb_style::attach(s_scr, "forecast");
}
void onExit() {
    orb_style::release(s_backdrop);
    temp_canvas_give();
}

void refresh() {
    if (!s_scr) return;
    WeatherSnapshot w;
    const bool have = weather_get(w) && w.valid;
    if (have) lv_obj_clear_flag(s_body, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);
    if (have) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    if (!have) return;

    // Old icons first, so their lines are gone before the point pool is reused under them.
    lv_obj_clean(s_nowIcon);
    for (int d = 0; d < DAYS; ++d) lv_obj_clean(s_dayIcon[d]);
    s_ptsUsed = 0;

    const char *unit = ui_wx_imperial() ? "F" : "C";
    char buf[48], alt[48];

    snprintf(buf, sizeof(buf), "UPDATED %s", w.updated);
    lv_label_set_text(s_updated, buf);

    draw_icon(s_nowIcon, w.code, 112);
    snprintf(buf, sizeof(buf), "%.0f\xC2\xB0%s", (double)temp(w.tempC), unit);
    snprintf(alt, sizeof(alt), "%.0f\xB0%s", (double)temp(w.tempC), unit);
    temp_draw(buf, alt);
    lv_label_set_text(s_cond, weather_condition(w.code));

    snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", (double)temp(w.feelsC));
    lv_label_set_text(s_metric[0], buf);
    snprintf(buf, sizeof(buf), "%d%%", w.humidity);
    lv_label_set_text(s_metric[1], buf);
    snprintf(buf, sizeof(buf), "%s %.0f %s", compass(w.windDeg), (double)wind(w.windKmh),
             ui_wx_imperial() ? "mph" : "km/h");
    lv_label_set_text(s_metric[2], buf);

    for (int d = 0; d < DAYS; ++d) {
        const bool ok = d < w.dayCount;
        const WeatherDay &day = w.days[d];
        lv_label_set_text(s_dayName[d], !ok ? "" : d == 0 ? "Today" : weather_day_name(day.date));
        if (ok) draw_icon(s_dayIcon[d], day.code, 40);
        // The night's low in the look's dim colour, by LVGL's inline recolour.
        if (ok) snprintf(buf, sizeof(buf), "%.0f\xC2\xB0 #%06X %.0f\xC2\xB0#",
                         (double)temp(day.tempMaxC), (unsigned)s_pal.dim, (double)temp(day.tempMinC));
        lv_label_set_text(s_dayTemps[d], ok ? buf : "");
        // Nothing under a dry day, rather than "0%" four times over.
        if (ok && day.rainChance >= 10) snprintf(buf, sizeof(buf), "%d%%", day.rainChance);
        lv_label_set_text(s_dayRain[d], ok && day.rainChance >= 10 ? buf : "");
    }
}

} // namespace forecastview

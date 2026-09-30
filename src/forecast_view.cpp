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
#include "forecast_view.h"
#include "weather.h"
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr int SCREEN = 466;
constexpr int MID    = SCREEN / 2;

constexpr uint32_t COL_TEXT  = 0xF2F2F2;
constexpr uint32_t COL_DIM   = 0x8A8F98;
constexpr uint32_t COL_RULE  = 0x2A2E36;
constexpr uint32_t COL_SUN   = 0xFFB23F;
constexpr uint32_t COL_CLOUD = 0xD8DEE9;
constexpr uint32_t COL_GREY  = 0x7C8594;   // overcast, rain and storm clouds
constexpr uint32_t COL_RAIN  = 0x4FA3FF;
constexpr uint32_t COL_SNOW  = 0xCFE8FF;
constexpr uint32_t COL_BOLT  = 0xFFD84A;

// Day strip: four columns centred on the dial, 76 px apart.
constexpr int DAYS = 4;
constexpr int DAY_X[DAYS] = { MID - 114, MID - 38, MID + 38, MID + 114 };

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_body = nullptr;          // everything that needs data; hidden while there is none
lv_obj_t *s_empty = nullptr;         // what shows instead
lv_obj_t *s_updated = nullptr;
lv_obj_t *s_nowIcon = nullptr;
lv_obj_t *s_temp = nullptr;
lv_obj_t *s_cond = nullptr;
lv_obj_t *s_metric[3] = {};
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

// A label of fixed width whose centre sits at (cx, y-top).
lv_obj_t *centred(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int cx, int y, int w) {
    lv_obj_t *l = label(parent, font, color);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, cx - w / 2, y);
    return l;
}

lv_obj_t *blank(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void circle(lv_obj_t *parent, float cx, float cy, float r, uint32_t color) {
    lv_obj_t *c = blank(parent);
    const int d = (int)lroundf(r * 2);
    lv_obj_set_size(c, d, d);
    lv_obj_set_pos(c, (int)lroundf(cx - r), (int)lroundf(cy - r));
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
}

void pill(lv_obj_t *parent, float x, float y, float w, float h, uint32_t color) {
    lv_obj_t *c = blank(parent);
    lv_obj_set_size(c, (int)lroundf(w), (int)lroundf(h));
    lv_obj_set_pos(c, (int)lroundf(x), (int)lroundf(y));
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
}

void line(lv_obj_t *parent, const float *xy, int n, float width, uint32_t color) {
    if (s_ptsUsed + n > (int)(sizeof(s_pts) / sizeof(s_pts[0]))) return;   // out of pool: skip the stroke, not the screen
    lv_point_t *p = &s_pts[s_ptsUsed];
    for (int i = 0; i < n; ++i) {
        p[i].x = (lv_coord_t)lroundf(xy[2 * i]);
        p[i].y = (lv_coord_t)lroundf(xy[2 * i + 1]);
    }
    s_ptsUsed += n;
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, p, n);
    lv_obj_set_style_line_width(l, (lv_coord_t)fmaxf(1.0f, width), 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
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

void sun(lv_obj_t *box, float cx, float cy, float r, float s) {
    const float w = fmaxf(2.0f, s / 28.0f);
    for (int i = 0; i < 8; ++i) {
        const float a = i * (float)M_PI / 4.0f;
        const float xy[4] = { cx + cosf(a) * r * 1.4f, cy + sinf(a) * r * 1.4f,
                              cx + cosf(a) * r * 1.8f, cy + sinf(a) * r * 1.8f };
        line(box, xy, 2, w, COL_SUN);
    }
    circle(box, cx, cy, r, COL_SUN);
}

// A cloud whose flat base spans w and whose base line is at y = base.
void cloud(lv_obj_t *box, float cx, float base, float w, uint32_t color) {
    const float h = w * 0.34f;
    circle(box, cx - w * 0.16f, base - h * 0.95f, w * 0.22f, color);
    circle(box, cx + w * 0.12f, base - h * 1.05f, w * 0.30f, color);
    pill(box, cx - w / 2, base - h, w, h, color);
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
        cloud(box, s * 0.44f, s * 0.8f, s * 0.7f, COL_CLOUD);
        break;
    case K_CLOUD:
        cloud(box, c, s * 0.72f, s * 0.84f, COL_GREY);
        break;
    case K_FOG:
        for (int i = 0; i < 3; ++i) {
            const float y = s * (0.38f + 0.16f * i), inset = (i == 1) ? 0.12f : 0.2f;
            const float xy[4] = { s * inset, y, s * (1 - inset), y };
            line(box, xy, 2, w * 1.4f, COL_DIM);
        }
        break;
    case K_RAIN:
        cloud(box, c, s * 0.6f, s * 0.8f, COL_GREY);
        for (int i = 0; i < 3; ++i) {
            const float x = s * (0.34f + 0.16f * i);
            const float xy[4] = { x, s * 0.7f, x - s * 0.06f, s * 0.9f };
            line(box, xy, 2, w * 1.2f, COL_RAIN);
        }
        break;
    case K_SNOW:
        cloud(box, c, s * 0.6f, s * 0.8f, COL_CLOUD);
        for (int i = 0; i < 3; ++i)
            circle(box, s * (0.32f + 0.18f * i), s * (i == 1 ? 0.86f : 0.76f), fmaxf(1.5f, s * 0.045f), COL_SNOW);
        break;
    case K_STORM: {
        cloud(box, c, s * 0.58f, s * 0.8f, COL_GREY);
        const float xy[8] = { s * 0.54f, s * 0.6f, s * 0.42f, s * 0.78f,
                              s * 0.56f, s * 0.78f, s * 0.44f, s * 0.96f };
        line(box, xy, 4, w * 1.3f, COL_BOLT);
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
    return b;
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

    s_updated = centred(s_body, &lv_font_montserrat_14, COL_DIM, MID, 62, 220);

    // Current conditions: icon left of centre, temperature right of it, word underneath.
    s_nowIcon = icon_box(s_body, MID - 72, 150, 112);
    s_temp = label(s_body, &lv_font_montserrat_48, COL_TEXT);
    lv_obj_set_style_text_align(s_temp, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_temp, MID + 2, 124);
    s_cond = centred(s_body, &lv_font_montserrat_22, COL_TEXT, MID, 214, 320);

    static const char *captions[3] = { "FEELS", "HUMIDITY", "WIND" };
    for (int i = 0; i < 3; ++i) {
        const int cx = MID + (i - 1) * 108;
        lv_label_set_text(centred(s_body, &lv_font_montserrat_12, COL_DIM, cx, 250, 104), captions[i]);
        s_metric[i] = centred(s_body, &lv_font_montserrat_18, COL_TEXT, cx, 266, 104);
    }

    static lv_point_t rule[2] = { { 0, 0 }, { 300, 0 } };
    lv_obj_t *r = lv_line_create(s_body);
    lv_line_set_points(r, rule, 2);
    lv_obj_set_pos(r, MID - 150, 304);
    lv_obj_set_style_line_width(r, 1, 0);
    lv_obj_set_style_line_color(r, lv_color_hex(COL_RULE), 0);

    for (int d = 0; d < DAYS; ++d) {
        s_dayName[d]  = centred(s_body, &lv_font_montserrat_16, COL_TEXT, DAY_X[d], 316, 78);
        s_dayIcon[d]  = icon_box(s_body, DAY_X[d], 358, 40);
        s_dayTemps[d] = centred(s_body, &lv_font_montserrat_16, COL_TEXT, DAY_X[d], 382, 78);
        lv_label_set_recolor(s_dayTemps[d], true);
        s_dayRain[d]  = centred(s_body, &lv_font_montserrat_12, COL_RAIN, DAY_X[d], 403, 78);
    }

    s_empty = centred(s_scr, &lv_font_montserrat_18, COL_DIM, MID, MID - 30, 300);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_empty, "No forecast yet\n\nIt arrives over WiFi a minute or so after the Orb has a location.");

    refresh();
}

lv_obj_t *screen() { return s_scr; }

void onEnter() { refresh(); }
void onExit() {}

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
    char buf[48];

    snprintf(buf, sizeof(buf), "UPDATED %s", w.updated);
    lv_label_set_text(s_updated, buf);

    draw_icon(s_nowIcon, w.code, 112);
    snprintf(buf, sizeof(buf), "%.0f\xC2\xB0%s", (double)temp(w.tempC), unit);
    lv_label_set_text(s_temp, buf);
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
        if (ok) snprintf(buf, sizeof(buf), "%.0f\xC2\xB0 #8A8F98 %.0f\xC2\xB0#",
                         (double)temp(day.tempMaxC), (double)temp(day.tempMinC));
        lv_label_set_text(s_dayTemps[d], ok ? buf : "");
        // Nothing under a dry day, rather than "0%" four times over.
        if (ok && day.rainChance >= 10) snprintf(buf, sizeof(buf), "%d%%", day.rainChance);
        lv_label_set_text(s_dayRain[d], ok && day.rainChance >= 10 ? buf : "");
    }
}

} // namespace forecastview

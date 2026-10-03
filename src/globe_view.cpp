// Globe: the Earth from the Sun's point of view. See globe_view.h.
//
// Ported from the background globe on Doug's homepage (~/Claude/homepage, the three.js
// section): the globe is turned so the subsolar point faces you, so you always see the
// daylit half, with a thin terminator at the lower-left limb where the page's slightly
// off-axis light leaves one. Cities are orange dots, home is green, as on the page.
//
// Drawing. globe_render.cpp makes a 233 x 233 frame; the draw event here doubles it
// straight into the band of LVGL's draw buffer it is asked for, exactly as Plasma does, so
// there is no full-resolution canvas. The cities are small LVGL circles over it, at full
// resolution, so they stay crisp.
//
// When it redraws. The Sun moves the globe a quarter of a degree a minute, so at rest the
// frame is redrawn about once a minute (when the subsolar longitude has moved 0.25 deg) and
// the orientation tables only when the subsolar latitude has moved 0.1 deg. While the knob
// is spinning it, every 40 ms tick that has something new.
//
// Knob. A turn spins the globe, SPIN_STEP degrees of longitude a detent, eased; clockwise
// turns it the way the Earth turns. RETURN_MS after the last detent it eases back to the
// Sun's view, the short way round, as the homepage does after a drag. A press toggles the
// city names (off by default, so the resting screen is just the globe).
//
// Time. time(nullptr): NTP/RTC on the device, the system clock in the simulator (where
// ORB_GLOBE_TIME=<unix seconds> pins it, for screenshots). Before the clock is set (before
// 2023) the globe sits at a fixed view over Europe and Africa rather than at 1970's.
//
// Cities. From orb-ponderer, fn=globe.cities, on enter and then hourly: Doug's homepage
// city list, {"cities":[{"lat","lon","color","label"}...], "home":{"lat","lon"}|null}.
// Until it answers (or with no relay configured) a built-in list stands in.
//
// Home. The device's own location if it has one (NVS "capsuleradar"/homeLat, homeLon,
// guarded by locSet exactly as main.cpp reads them; main.cpp keeps them in a file-static,
// and NVS is updated whenever it changes, so reading NVS on enter is both current and
// needs no new shared accessor). Otherwise the relay's home, otherwise no green dot. In the
// simulator: ORBLAT/ORBLON if set, else the relay's home, else the simulator's default.
//
// Themes (orb_style). With no theme it is the photograph on black, with stars, as it always
// was. A theme changes how the Earth itself is drawn (globe_render.h, "Styles"): the
// photograph with the theme's accent for an atmosphere (Modern, and any dark theme this
// file has no better idea for), a phosphor outline for a one-colour instrument (Cold War),
// an engraved atlas for ink on paper (Aviator), brass and verdigris for a theme with a
// warm metal accent (Steam Punk), neon glass for one that glows hard (Plasma). Dots and
// names take the theme's colours and its small typeface; home is the accent, ringed.
// Under a theme the frame is no longer the whole picture: the theme's backdrop is behind
// it, so the draw event writes only the globe, blends the one ring of pixels its limb
// passes through, and adds the limb's glow to what is already in LVGL's buffer (or, on
// paper, leaves it alone), and it only claims to cover areas wholly inside the globe.
//
// Memory. Taken on enter, given back on exit, all PSRAM: frame 106 KB, orientation and
// lighting tables 256 KB (seven blocks, none over 75 KB), the dots and their label
// scratch 5 KB: about 367 KB; a drawn globe has two more tables, 74 KB. A theme's backdrop
// is orb_style's while this screen shows: up to 424 KB for a plate and 636 KB for a glass
// layer (Cold War has both: ~1.5 MB in all), freed on exit. Nothing static but a few pointers in internal RAM. The two small
// city lists the relay client fills (~5 KB) are taken once in init() and never freed:
// the network task writes them and the UI thread reads them, and never freeing them means
// neither can be reading while the other takes them away (docs/memory.md).
// The map itself (256 KB + 16 KB water mask) and the coast map (128 KB) are const, in flash.
#include "globe_view.h"
#include "globe_render.h"
#include "orb_style.h"
#include "ponderer.h"
#include "display.h"      // orb_screen_covered()
#include <ArduinoJson.h>
#include <atomic>
#include <mutex>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(ESP_PLATFORM)
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
static void *ps_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void  ps_free(void *p) { heap_caps_free(p); }
static uint32_t now_ms() { return millis(); }
static uint32_t now_us() { return (uint32_t)micros(); }
#define GLOG(...) Serial.printf(__VA_ARGS__)
#else
#include <chrono>
static void *ps_alloc(size_t n) { return malloc(n); }
static void  ps_free(void *p) { free(p); }
static uint32_t now_ms() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static uint32_t now_us() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
#define GLOG(...) printf(__VA_ARGS__)
#endif

namespace {

constexpr int SCREEN = 466;
constexpr int MID = SCREEN / 2;
constexpr float RS = globe::R * 2.0f;     // globe radius on the panel
constexpr uint32_t TICK_MS = 40;
constexpr float SPIN_STEP = 6.0f;         // degrees of longitude per detent
constexpr uint32_t RETURN_MS = 5000;      // after the last detent, ease home
constexpr float SPIN_RATE = 14.0f;        // per second, following the knob
constexpr float HOME_RATE = 2.2f;         // per second, going back to the Sun's view
constexpr float REDRAW_LON = 0.25f;       // degrees: about a minute of the Sun
constexpr float RETILT = 0.1f;            // degrees of subsolar latitude
constexpr int BANDS = 10;
constexpr int BANDS_THEMED = 8;           // each split in three: see invalidate_globe()

constexpr int MAX_CITIES = 64;
constexpr uint32_t COL_CITY = 0xE08A2E;
constexpr uint32_t COL_HOME = 0x68D348;
constexpr uint32_t REFRESH_MS = 3600u * 1000u;

// ---- the city list, shared with the network side -----------------------------------------

struct City { float lat, lon; uint32_t color; char label[25]; };   // the relay clips labels to 24
struct CityList {
    int n;
    bool fromRelay;
    bool hasHome;
    float homeLat, homeLon;
    City c[MAX_CITIES];
};

// Doug's homepage list, for when the relay has not answered.
const City FALLBACK[] = {
    {  34.05f, -118.24f, COL_CITY, "LA" },
    {  40.71f,  -74.01f, COL_CITY, "NYC" },
    {  51.51f,   -0.13f, COL_CITY, "London" },
    {  46.20f,    6.14f, COL_CITY, "Geneva" },
    {  45.46f,    9.19f, COL_CITY, "Milan" },
    {  41.90f,   12.50f, COL_CITY, "Rome" },
    {  52.23f,   21.01f, COL_CITY, "Warsaw" },
    {  22.32f,  114.17f, COL_CITY, "Hong Kong" },
    {   1.35f,  103.82f, COL_CITY, "Singapore" },
    {  35.68f,  139.69f, COL_CITY, "Tokyo" },
    { -37.81f,  144.96f, COL_CITY, "Melbourne" },
    { -43.53f,  172.64f, COL_CITY, "Christchurch" },
};

std::mutex s_mu;
CityList *s_shared = nullptr;             // under s_mu; never freed
CityList *s_net = nullptr;                // the network side's own; never freed
std::atomic<bool> s_active{false};        // the screen is up: the net side may ask
std::atomic<uint32_t> s_nextMs{0};
std::atomic<int> s_fails{0};

bool due(uint32_t at) { return (int32_t)(now_ms() - at) >= 0; }

uint32_t parse_colour(const char *s, uint32_t dflt) {
    if (!s || s[0] != '#' || strlen(s) != 7) return dflt;
    char *end = nullptr;
    const unsigned long v = strtoul(s + 1, &end, 16);
    return (end && *end == 0) ? (uint32_t)v : dflt;
}

void clean_label(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (const char *p = src ? src : ""; *p && n + 1 < cap; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c <= 0x7E) dst[n++] = (char)c;
    }
    dst[n] = 0;
}

bool net_step() {
    if (!s_active.load() || !s_net || !ponderer::configured() || !due(s_nextMs)) return false;
#if defined(ESP_PLATFORM)
    if (WiFi.status() != WL_CONNECTED) { s_nextMs = now_ms() + 5000; return false; }
#endif
    uint8_t *body = nullptr; size_t len = 0;
    bool ok = false;
    if (ponderer::get("globe.cities", "", &body, &len, 16384)) {
        JsonDocument doc;
        if (!deserializeJson(doc, (const char *)body, len) && doc["cities"].is<JsonArrayConst>()) {
            CityList &L = *s_net;
            L.n = 0;
            for (JsonObjectConst c : doc["cities"].as<JsonArrayConst>()) {
                if (L.n >= MAX_CITIES) break;
                if (!c["lat"].is<float>() || !c["lon"].is<float>()) continue;
                City &o = L.c[L.n++];
                o.lat = c["lat"].as<float>();
                o.lon = c["lon"].as<float>();
                o.color = parse_colour(c["color"] | "", COL_CITY);
                clean_label(o.label, sizeof(o.label), c["label"] | "");
            }
            L.fromRelay = true;
            L.hasHome = doc["home"]["lat"].is<float>() && doc["home"]["lon"].is<float>();
            L.homeLat = L.hasHome ? doc["home"]["lat"].as<float>() : 0;
            L.homeLon = L.hasHome ? doc["home"]["lon"].as<float>() : 0;
            ok = true;
        }
        ponderer::release(body);
    }
    if (!ok) {
        // 1, 2, 4... minutes, up to fifteen; the built-in list stays up meanwhile
        const int f = ++s_fails;
        uint32_t wait = 60000u << (f > 4 ? 4 : f - 1);
        if (wait > 900000u) wait = 900000u;
        s_nextMs = now_ms() + wait;
        return false;
    }
    s_fails = 0;
    s_nextMs = now_ms() + REFRESH_MS;
    {
        std::lock_guard<std::mutex> g(s_mu);
        memcpy(s_shared, s_net, sizeof(CityList));
    }
    GLOG("[globe] %d cities from the relay%s\n", s_net->n, s_net->hasHome ? ", with home" : "");
    return true;
}

void ui_apply();   // below

// ---- the screen ----------------------------------------------------------------------

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_obj = nullptr;
lv_timer_t *s_timer = nullptr;
globe::Render s_ren = {};
bool s_ready = false;

// What the theme asked for, set on enter.
orb_style::Backdrop *s_backdrop = nullptr;   // its plate, glass and sparks, while showing
bool s_themed = false;                    // draw only the globe, over the backdrop
bool s_skyGlow = false;                   // ...and add the limb's glow to it
struct DotLook {
    bool themed;                          // false: the list's own colours, as with no theme
    uint32_t city, home, ring, text, tag;
    lv_opa_t ringOpa, tagOpa;
    bool halo;                            // a glowing theme: a soft ring of the dot's colour round it
    const lv_font_t *font;
};
DotLook s_dl = {};

struct Dot {
    float lat, lon;
    lv_obj_t *dot, *lab;
    int size, labW, labH;
    // where the last place_dots() put it
    int sx, sy;
    float z;
    lv_opa_t opa;                         // 0 = not showing
};
Dot *s_dots = nullptr;                    // PSRAM, MAX_CITIES + 1, while the screen is up
int s_dotN = 0;
bool s_labels = false;

float s_spin = 0, s_spinTarget = 0;       // knob offset, degrees (positive = clockwise)
uint32_t s_lastTurnMs = 0, s_lastTickMs = 0;
float s_drawnFace = 1e9f, s_face = 0, s_tilt = 0;
bool s_clockOk = false;
uint32_t s_drawUs = 0, s_draws = 0;       // what frames cost, logged at the end of each spin
bool s_wasMoving = false;

// The labels' placement scratch, with the dots, in PSRAM while the screen is up.
struct LabelScratch {
    lv_area_t taken[MAX_CITIES + 1];
    int16_t order[MAX_CITIES + 1];
    const char *shown[MAX_CITIES + 1];
};
LabelScratch *s_lab = nullptr;

// Half the width of a circle of radius r at panel row y (pixel centres), for clipping to
// the glass and for the invalidation bands. Computed as needed rather than kept in tables:
// every byte of static here is internal RAM.
inline void chord(float r, int y, int *l, int *rr) {
    const float dy = y + 0.5f - SCREEN / 2.0f;
    const float h2 = r * r - dy * dy;
    const float hw = h2 > 0 ? sqrtf(h2) : 0;
    *l = (int)floorf(SCREEN / 2.0f - hw);
    *rr = (int)ceilf(SCREEN / 2.0f + hw) - 1;
}

// ---- the Sun -------------------------------------------------------------------------

time_t clock_now() {
#if !defined(ARDUINO)
    const char *e = getenv("ORB_GLOBE_TIME");   // 0 = pretend the clock is not set yet
    if (e && e[0]) return (time_t)atoll(e);
#endif
    return time(nullptr);
}

// The homepage's subsolarPoint(): longitude from UTC hours, latitude from the day of the
// year. Returns false (and a fixed, pleasant view) while the clock is not set.
bool subsolar(float *lat, float *lon) {
    const time_t t = clock_now();
    if (t < 1672531200) { *lat = 12.0f; *lon = 12.0f; return false; }   // before 2023
    struct tm g;
    gmtime_r(&t, &g);
    const double h = g.tm_hour + g.tm_min / 60.0 + g.tm_sec / 3600.0;
    *lon = (float)(fmod((12.0 - h) * 15.0 + 540.0, 360.0) - 180.0);
    const int doy = g.tm_yday + 1;   // the page's floor((t - Date.UTC(y, 0, 0)) / day)
    *lat = (float)(23.44 * sin(2 * M_PI / 365.0 * (doy - 81)));
    return true;
}

// ---- home ----------------------------------------------------------------------------

// The device's own location, if it has one.
bool device_home(float *lat, float *lon) {
#if defined(ESP_PLATFORM)
    Preferences p;
    bool ok = false;
    if (p.begin("capsuleradar", true)) {
        // the same guard as main.cpp: a stored coordinate is a location, 0,0 defaults are not
        if (p.getBool("locSet", p.isKey("homeLat"))) {
            *lat = (float)p.getDouble("homeLat", 0.0);
            *lon = (float)p.getDouble("homeLon", 0.0);
            ok = true;
        }
        p.end();
    }
    return ok;
#else
    const char *a = getenv("ORBLAT"), *b = getenv("ORBLON");
    if (a && b) { *lat = (float)atof(a); *lon = (float)atof(b); return true; }
    return false;
#endif
}

// ---- drawing -------------------------------------------------------------------------

inline uint16_t avg2(uint16_t a, uint16_t b) { return (uint16_t)((a & b) + (((a ^ b) & 0xF7DE) >> 1)); }

inline uint16_t up2(const uint16_t *r0, const uint16_t *r1, int X, bool odd) {
    constexpr int RW = globe::RW;
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

// light added to what is there, each channel stopping at full
inline uint16_t add565(uint16_t d, uint16_t v) {
    uint32_t r = (d >> 11) + (v >> 11), g = ((d >> 5) & 63) + ((v >> 5) & 63), b = (d & 31) + (v & 31);
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Under a theme: LVGL's buffer already holds the backdrop. The globe is written over it;
// the pixels its limb passes through are blended by how much of each the globe covers (at
// the panel's resolution, so the limb is sharper than the half-resolution frame); and
// outside, the frame's glow is added to the backdrop, or on paper nothing is drawn at all.
void draw_themed(lv_draw_ctx_t *dc, lv_obj_t *obj, const lv_area_t &a) {
    if (!s_ready) return;
    const int stride = lv_area_get_width(dc->buf_area);
    lv_color_t *buf = (lv_color_t *)dc->buf;
    const int ox = obj->coords.x1, oy = obj->coords.y1;
    const uint16_t *src = s_ren.out;
    constexpr int RW = globe::RW;
    const float mid = SCREEN / 2.0f, rb = RS;
    for (int y = a.y1; y <= a.y2; ++y) {
        const int Y = y - oy;
        if (Y < 0 || Y >= SCREEN) continue;
        const float dy = Y + 0.5f - mid;
        const float ho2 = (rb + 1) * (rb + 1) - dy * dy, hi2 = (rb - 1) * (rb - 1) - dy * dy;
        const float ho = ho2 > 0 ? sqrtf(ho2) : -1.0f, hi = hi2 > 0 ? sqrtf(hi2) : -1.0f;
        // the limb touches [bl, br]; [il, ir] is wholly inside it
        const int bl = ho >= 0 ? (int)floorf(mid - ho) + ox : 1, br = ho >= 0 ? (int)ceilf(mid + ho) - 1 + ox : 0;
        const int il = hi >= 0 ? (int)ceilf(mid - hi - 0.5f) + ox : 1, ir = hi >= 0 ? (int)floorf(mid + hi - 0.5f) + ox : 0;
        int xl = bl, xr = br;
        if (s_skyGlow) { chord(mid, Y, &xl, &xr); xl += ox; xr += ox; }   // out to the glass's edge
        if (xl < a.x1) xl = a.x1;
        if (xr > a.x2) xr = a.x2;
        const int sy = Y >> 1, sy1 = sy + 1 < RW ? sy + 1 : RW - 1;
        const uint16_t *r0 = src + sy * RW, *r1 = src + sy1 * RW;
        const bool odd = Y & 1;
        lv_color_t *o = buf + (size_t)(y - dc->buf_area->y1) * stride + (xl - dc->buf_area->x1);
        for (int x = xl; x <= xr; ++x, ++o) {
            const int X = x - ox;
            if (x >= il && x <= ir) { o->full = up2(r0, r1, X, odd); continue; }
            const uint16_t v = up2(r0, r1, X, odd);
            if (x < bl || x > br) { if (v) o->full = add565(o->full, v); continue; }   // sky
            const float dx = X + 0.5f - mid;
            const float cov = rb + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov >= 1) { o->full = v; continue; }
            const uint16_t under = s_skyGlow ? add565(o->full, v) : o->full;
            o->full = cov <= 0 ? under : mix565(under, v, (uint32_t)(cov * 32.0f));
        }
    }
}

// Plasma's doubler: output pixel 2i is source i, 2i+1 the mean of i and i+1; rows likewise.
void draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    if (!_lv_area_intersect(&a, dc->clip_area, &obj->coords)) return;
    if (s_themed) { draw_themed(dc, obj, a); return; }
    const int stride = lv_area_get_width(dc->buf_area);
    lv_color_t *buf = (lv_color_t *)dc->buf;
    const int ox = obj->coords.x1, oy = obj->coords.y1;
    const uint16_t *src = s_ready ? s_ren.out : nullptr;
    constexpr int RW = globe::RW;
    for (int y = a.y1; y <= a.y2; ++y) {
        lv_color_t *row = buf + (size_t)(y - dc->buf_area->y1) * stride + (a.x1 - dc->buf_area->x1);
        const int Y = y - oy;
        int xl = 1, xr = 0;
        if (src && Y >= 0 && Y < SCREEN) { chord(SCREEN / 2.0f, Y, &xl, &xr); xl += ox; xr += ox; }
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
}

void cover_cb(lv_event_t *e) {
    lv_cover_check_info_t *info = (lv_cover_check_info_t *)lv_event_get_param(e);
    lv_obj_t *obj = lv_event_get_target(e);
    if (info->res == LV_COVER_RES_MASKED) return;
    if (s_themed) {
        // only what lies wholly inside the globe is covered; the backdrop is needed elsewhere
        bool in = s_ready;
        const float lim = (RS - 2) * (RS - 2), mid = SCREEN / 2.0f;
        const float xs[2] = { info->area->x1 - obj->coords.x1 - mid, info->area->x2 - obj->coords.x1 + 1 - mid };
        const float ys[2] = { info->area->y1 - obj->coords.y1 - mid, info->area->y2 - obj->coords.y1 + 1 - mid };
        for (float x : xs) for (float y : ys) if (x * x + y * y > lim) in = false;
        info->res = in ? LV_COVER_RES_COVER : LV_COVER_RES_NOT_COVER;
        return;
    }
    info->res = _lv_area_is_in(info->area, &obj->coords, 0) ? LV_COVER_RES_COVER : LV_COVER_RES_NOT_COVER;
}

// Under a theme each band is cut in three. The middle piece lies wholly inside the globe,
// so LVGL (which asks cover_cb per piece) starts drawing there at this object and does not
// paint the backdrop under it first; the two ends are where the limb runs.
void invalidate_themed() {
    const lv_area_t &c = s_obj->coords;
    const int top = MID - (int)RS - 2, bot = MID + (int)RS + 2;
    const int h = (bot - top + 1) / BANDS_THEMED + 1;
    for (int y0 = top; y0 <= bot; y0 += h) {
        const int y1 = y0 + h - 1 < bot ? y0 + h - 1 : bot;
        const int ym = (y0 <= MID && y1 >= MID) ? MID : (y1 < MID ? y1 : y0);
        const int yf = abs(y0 - MID) > abs(y1 + 1 - MID) ? y0 - 1 : y1 + 1;   // the edge furthest from the middle
        int l, r, il, ir;
        chord(RS + 4, ym, &l, &r);
        chord(RS - 3, yf, &il, &ir);
        auto inv = [&](int xa, int xb) {
            lv_area_t a = { (lv_coord_t)(c.x1 + xa), (lv_coord_t)(c.y1 + y0), (lv_coord_t)(c.x1 + xb), (lv_coord_t)(c.y1 + y1) };
            lv_obj_invalidate_area(s_obj, &a);
        };
        if (ir - il < 80) { inv(l, r); continue; }
        ++il; --ir;
        inv(l, il - 1);
        inv(il, ir);
        inv(ir + 1, r);
    }
}

// Only the globe changes between frames; invalidate it as bands that follow its circle.
void invalidate_globe() {
    if (s_themed) { invalidate_themed(); return; }
    const lv_area_t &c = s_obj->coords;
    const int top = MID - (int)RS - 2, bot = MID + (int)RS + 2;
    const int h = (bot - top + 1) / BANDS + 1;
    for (int y0 = top; y0 <= bot; y0 += h) {
        const int y1 = y0 + h - 1 < bot ? y0 + h - 1 : bot;
        const int ym = (y0 <= MID && y1 >= MID) ? MID : (y1 < MID ? y1 : y0);
        int l, r;
        chord(RS + 4, ym, &l, &r);   // the globe and the first few px of its glow
        lv_area_t a = { (lv_coord_t)(c.x1 + l), (lv_coord_t)(c.y1 + y0),
                        (lv_coord_t)(c.x1 + r), (lv_coord_t)(c.y1 + y1) };
        lv_obj_invalidate_area(s_obj, &a);
    }
}

// ---- the dots ------------------------------------------------------------------------

void delete_dots() {
    if (!s_dots) return;
    for (int i = 0; i < s_dotN; ++i) {
        if (s_dots[i].dot) lv_obj_del(s_dots[i].dot);
        if (s_dots[i].lab) lv_obj_del(s_dots[i].lab);
    }
    s_dotN = 0;
}

void add_dot(float lat, float lon, uint32_t colour, int size, const char *label, bool home) {
    if (s_dotN >= MAX_CITIES + 1) return;
    Dot &d = s_dots[s_dotN++];
    d.lat = lat; d.lon = lon; d.size = size;
    d.dot = lv_obj_create(s_scr);
    lv_obj_remove_style_all(d.dot);
    lv_obj_clear_flag(d.dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(d.dot, size, size);
    lv_obj_set_style_radius(d.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d.dot, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(d.dot, LV_OPA_COVER, 0);
    // a dark ring so a dot reads on bright desert and cloud as well as on sea
    // (the paper's own colour on an ink-on-paper theme)
    lv_obj_set_style_border_width(d.dot, 1, 0);
    lv_obj_set_style_border_color(d.dot, lv_color_hex(s_dl.ring), 0);
    lv_obj_set_style_border_opa(d.dot, s_dl.ringOpa, 0);
    if (s_dl.themed && home) {
        // home: the same dot inside a drawn ring, so it is told apart by shape as well as colour
        lv_obj_set_style_outline_width(d.dot, 2, 0);
        lv_obj_set_style_outline_pad(d.dot, 2, 0);
        lv_obj_set_style_outline_color(d.dot, lv_color_hex(colour), 0);
        lv_obj_set_style_outline_opa(d.dot, LV_OPA_COVER, 0);
    } else if (s_dl.halo) {
        lv_obj_set_style_outline_width(d.dot, 3, 0);
        lv_obj_set_style_outline_pad(d.dot, 0, 0);
        lv_obj_set_style_outline_color(d.dot, lv_color_hex(colour), 0);
        lv_obj_set_style_outline_opa(d.dot, LV_OPA_30, 0);
    } else {
        lv_obj_set_style_outline_width(d.dot, 0, 0);
    }
    lv_obj_add_flag(d.dot, LV_OBJ_FLAG_HIDDEN);
    d.lab = nullptr; d.labW = d.labH = 0;
    d.sx = d.sy = 0; d.z = -1; d.opa = 0;
    if (label && label[0]) {
        d.lab = lv_label_create(s_scr);
        lv_obj_set_style_text_font(d.lab, s_dl.font, 0);
        lv_obj_set_style_text_color(d.lab, lv_color_hex(s_dl.text), 0);
        lv_obj_set_style_bg_color(d.lab, lv_color_hex(s_dl.tag), 0);
        lv_obj_set_style_bg_opa(d.lab, s_dl.tagOpa, 0);
        lv_obj_set_style_radius(d.lab, 3, 0);
        lv_obj_set_style_pad_hor(d.lab, 3, 0);
        lv_obj_set_style_pad_ver(d.lab, 1, 0);
        lv_label_set_text(d.lab, label);
        lv_point_t sz;
        lv_txt_get_size(&sz, label, s_dl.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        d.labW = sz.x + 6; d.labH = sz.y + 2;
        lv_obj_add_flag(d.lab, LV_OBJ_FLAG_HIDDEN);
    }
}

// Build the dots from the shared list: cities first, home last so it sits on top.
void build_dots() {
    if (!s_dots || !s_shared) return;
    delete_dots();
    CityList *L = (CityList *)ps_alloc(sizeof(CityList));
    if (!L) return;
    { std::lock_guard<std::mutex> g(s_mu); memcpy(L, s_shared, sizeof(CityList)); }

    float hLat = 0, hLon = 0;
    bool home = device_home(&hLat, &hLon);
    const char *from = home ? "device" : "none";
    if (!home && L->hasHome) { hLat = L->homeLat; hLon = L->homeLon; home = true; from = "relay"; }
#if !defined(ARDUINO)
    if (!home) { hLat = 33.4484f; hLon = -112.0740f; home = true; from = "sim default"; }   // sim_main.cpp's SIM_HOME_LAT/LON
#endif
    // The homepage groups several dots under one clock row ("Geneva - Milan - Rome -
    // Warsaw"), and the relay gives every dot of a row that row's label. Where the names
    // split into exactly as many parts as the row has dots, each dot gets its own name;
    // otherwise (the relay clips labels at 24 characters) they keep the shared one and
    // place_labels() shows it once, at the most central dot of the row.
    for (int i = 0; i < L->n;) {
        int j = i + 1;
        while (j < L->n && L->c[i].label[0] && !strcmp(L->c[j].label, L->c[i].label)) ++j;
        const int run = j - i;
        int parts = 1;
        for (const char *p = strstr(L->c[i].label, " - "); p; p = strstr(p + 3, " - ")) ++parts;
        if (run > 1 && parts == run && !strstr(L->c[i].label, "...")) {
            char row[sizeof(L->c[i].label)];
            memcpy(row, L->c[i].label, sizeof(row));
            const char *p = row;
            for (int k = i; k < j; ++k) {
                const char *e = strstr(p, " - ");
                const size_t len = e ? (size_t)(e - p) : strlen(p);
                snprintf(L->c[k].label, sizeof(L->c[k].label), "%.*s", (int)len, p);
                p = e ? e + 3 : p + len;
            }
        }
        i = j;
    }
    // A city that is also home gets no orange dot under the green one.
    const char *homeLabel = "Home";
    for (int i = 0; i < L->n; ++i) {
        const City &c = L->c[i];
        if (home && fabsf(c.lat - hLat) < 0.5f && fabsf(c.lon - hLon) < 0.5f) {
            if (c.label[0]) homeLabel = c.label;
            continue;
        }
        add_dot(c.lat, c.lon, s_dl.themed ? s_dl.city : c.color, 7, c.label, false);
    }
    if (home) add_dot(hLat, hLon, s_dl.themed ? s_dl.home : COL_HOME, 9, homeLabel, true);
    orb_style::raise(s_backdrop);         // the theme's glass and sparks go back in front
    GLOG("[globe] %d dots (%s list), home from %s\n", s_dotN, L->fromRelay ? "relay" : "built-in", from);
    ps_free(L);
}

bool overlaps(const lv_area_t &a, const lv_area_t &b) {
    return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}

// Inside the glass, with a little margin: all four corners within the circle.
bool on_dial(const lv_area_t &a) {
    const int lim = (MID - 6) * (MID - 6);
    const int xs[2] = { a.x1 - MID, a.x2 - MID }, ys[2] = { a.y1 - MID, a.y2 - MID };
    for (int x : xs) for (int y : ys) if (x * x + y * y > lim) return false;
    return true;
}

// Names, when they are on: each tries the right of its dot, then the left, above, below,
// and takes the first place that covers no other name and no other dot. One that finds
// none stays hidden, so a cluster like Geneva / Milan / Rome reads rather than piles up.
// Home goes first, then the cities nearest the middle of the disc.
void place_labels() {
    if (!s_lab) return;
    lv_area_t *taken = s_lab->taken;
    int16_t *order = s_lab->order;
    const char **shown = s_lab->shown;
    int nTaken = 0;
    int n = 0;
    for (int i = s_dotN - 1; i >= 0; --i) {           // home is last in the array
        Dot &d = s_dots[i];
        if (!d.lab) continue;
        // names wait until their dot is well clear of the limb, then fade in over the rest
        if (!s_labels || d.opa < 128) { lv_obj_add_flag(d.lab, LV_OBJ_FLAG_HIDDEN); continue; }
        order[n++] = (int16_t)i;
    }
    // home first, the rest by how squarely they face us (insertion sort; n <= 65)
    const bool homeFirst = n && order[0] == s_dotN - 1 && s_dots[s_dotN - 1].size > 7;
    for (int a = homeFirst ? 2 : 1; a < n; ++a) {
        const int16_t v = order[a];
        int b = a - 1;
        while (b >= (homeFirst ? 1 : 0) && s_dots[order[b]].z < s_dots[v].z) { order[b + 1] = order[b]; --b; }
        order[b + 1] = v;
    }
    int nShown = 0;
    for (int a = 0; a < n; ++a) {
        Dot &d = s_dots[order[a]];
        const char *text = lv_label_get_text(d.lab);
        bool dup = false;
        for (int t = 0; t < nShown && !dup; ++t) dup = !strcmp(shown[t], text);
        if (dup) { lv_obj_add_flag(d.lab, LV_OBJ_FLAG_HIDDEN); continue; }
        const int gap = d.size / 2 + (s_dl.themed && d.size > 7 ? 7 : 3), w = d.labW, h = d.labH;   // clear of home's ring
        const bool rightSide = d.sx > MID + RS * 0.45f;
        lv_area_t cand[4] = {
            { (lv_coord_t)(d.sx + gap), (lv_coord_t)(d.sy - h / 2), (lv_coord_t)(d.sx + gap + w - 1), (lv_coord_t)(d.sy - h / 2 + h - 1) },
            { (lv_coord_t)(d.sx - gap - w), (lv_coord_t)(d.sy - h / 2), (lv_coord_t)(d.sx - gap - 1), (lv_coord_t)(d.sy - h / 2 + h - 1) },
            { (lv_coord_t)(d.sx - w / 2), (lv_coord_t)(d.sy - gap - h), (lv_coord_t)(d.sx - w / 2 + w - 1), (lv_coord_t)(d.sy - gap - 1) },
            { (lv_coord_t)(d.sx - w / 2), (lv_coord_t)(d.sy + gap), (lv_coord_t)(d.sx - w / 2 + w - 1), (lv_coord_t)(d.sy + gap + h - 1) },
        };
        if (rightSide) { const lv_area_t t = cand[0]; cand[0] = cand[1]; cand[1] = t; }
        int pick = -1;
        for (int c = 0; c < 4 && pick < 0; ++c) {
            if (!on_dial(cand[c])) continue;
            bool clear = true;
            for (int t = 0; t < nTaken && clear; ++t) clear = !overlaps(cand[c], taken[t]);
            for (int o = 0; o < s_dotN && clear; ++o) {
                const Dot &e = s_dots[o];
                if (&e == &d || !e.opa) continue;
                const lv_area_t box = { (lv_coord_t)(e.sx - e.size / 2), (lv_coord_t)(e.sy - e.size / 2),
                                        (lv_coord_t)(e.sx + e.size / 2), (lv_coord_t)(e.sy + e.size / 2) };
                clear = !overlaps(cand[c], box);
            }
            if (clear) pick = c;
        }
        if (pick < 0) { lv_obj_add_flag(d.lab, LV_OBJ_FLAG_HIDDEN); continue; }
        taken[nTaken++] = cand[pick];
        shown[nShown++] = text;
        lv_obj_set_pos(d.lab, cand[pick].x1, cand[pick].y1);
        lv_obj_set_style_opa(d.lab, (lv_opa_t)((d.opa - 128) * 2 + 1), 0);
        lv_obj_clear_flag(d.lab, LV_OBJ_FLAG_HIDDEN);
    }
}

void place_dots() {
    for (int i = 0; i < s_dotN; ++i) {
        Dot &d = s_dots[i];
        float x, y, z;
        globe::project(d.lat, d.lon, s_tilt, s_face, &x, &y, &z);
        d.z = z;
        // fade out over the last stretch before the limb, gone behind it
        float k = (z - 0.03f) / 0.2f;
        if (k <= 0) {
            d.opa = 0;
            lv_obj_add_flag(d.dot, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        if (k > 1) k = 1;
        d.opa = (lv_opa_t)(k * 254 + 1);
        d.sx = (int)lroundf(MID + x * RS);
        d.sy = (int)lroundf(MID - y * RS);
        lv_obj_set_pos(d.dot, d.sx - d.size / 2, d.sy - d.size / 2);
        lv_obj_set_style_opa(d.dot, d.opa, 0);
        lv_obj_clear_flag(d.dot, LV_OBJ_FLAG_HIDDEN);
    }
    place_labels();
}

void ui_apply() {
    if (s_ready) { build_dots(); place_dots(); }
}

// ---- the frame -------------------------------------------------------------------------

float wrap180(float a) {
    a = fmodf(a + 180.0f, 360.0f);
    if (a < 0) a += 360.0f;
    return a - 180.0f;
}

// Bring the tables, the frame and the dots up to date. `force` redraws regardless.
void refresh(bool force) {
    float lat, lon;
    s_clockOk = subsolar(&lat, &lon);
    bool redraw = force;
    if (!s_ren.tablesOk || fabsf(lat - s_ren.tilt) > RETILT) {
        const uint32_t t0 = now_us();
        globe::build_tables(s_ren, lat);
        GLOG("[globe] tables for tilt %.2f: %.1f ms\n", lat, (now_us() - t0) / 1000.0f);
        redraw = true;
    }
    s_tilt = s_ren.tilt;
    const float face = wrap180(lon - s_spin);
    if (fabsf(wrap180(face - s_drawnFace)) >= REDRAW_LON || s_drawnFace > 1e8f) redraw = true;
    if (!redraw) return;
    s_face = face;
    s_drawnFace = face;
    const uint32_t t0 = now_us();
    globe::draw(s_ren, face);
    s_drawUs += now_us() - t0;
    s_draws++;
    // the globe's bands first: the dots' own invalidations then fall inside areas LVGL
    // already has, rather than each taking a slot of its small list
    invalidate_globe();
    place_dots();
}

void log_draws(const char *what) {
    if (!s_draws) return;
    GLOG("[globe] %s: %u frames, %.2f ms each to render (plus the flush)\n",
         what, (unsigned)s_draws, s_drawUs / 1000.0f / s_draws);
    s_drawUs = s_draws = 0;
}

void tick_cb(lv_timer_t *) {
    if (!s_ready || lv_scr_act() != s_scr || orb_screen_covered()) return;
    const uint32_t ms = lv_tick_get();
    float dt = (ms - s_lastTickMs) / 1000.0f;
    s_lastTickMs = ms;
    if (dt > 0.2f) dt = 0.2f;

    // after a pause, go back to the Sun's view the short way round
    if (s_spinTarget != 0 && ms - s_lastTurnMs >= RETURN_MS) {
        s_spin = wrap180(s_spin);
        s_spinTarget = 0;
    }
    bool moving = false;
    if (s_spin != s_spinTarget) {
        const float rate = (s_spinTarget == 0 && ms - s_lastTurnMs >= RETURN_MS) ? HOME_RATE : SPIN_RATE;
        s_spin += (s_spinTarget - s_spin) * (1 - expf(-rate * dt));
        if (fabsf(s_spinTarget - s_spin) < 0.05f) s_spin = s_spinTarget;
        moving = true;
    }
    if (moving) {
        // while it turns every tick is a frame; the threshold is for the Sun alone
        s_drawnFace = 1e9f;
    }
    refresh(false);
    if (s_wasMoving && !moving) log_draws("spin");
    s_wasMoving = moving;
}

// ---- the theme -------------------------------------------------------------------------

void hue_sat(uint32_t rgb, float *h, float *s) {
    const float r = ((rgb >> 16) & 255) / 255.0f, g = ((rgb >> 8) & 255) / 255.0f, b = (rgb & 255) / 255.0f;
    const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    const float l = (mx + mn) / 2;
    *s = d <= 0 ? 0 : d / (1 - fabsf(2 * l - 1) + 1e-6f);
    float hh = 0;
    if (d > 0) {
        if (mx == r) hh = fmodf((g - b) / d, 6.0f);
        else if (mx == g) hh = (b - r) / d + 2;
        else hh = (r - g) / d + 4;
    }
    hh *= 60;
    *h = hh < 0 ? hh + 360 : hh;
}

// Which globe a look gets. See the note on themes at the top.
globe::Kind kind_for(const orb_style::Look &l) {
    const orb_style::Look plain;
    if (l.text == plain.text && l.accent == plain.accent && l.accent2 == plain.accent2 && l.bg == plain.bg
        && !l.plate[0] && !l.overlay[0] && !l.mono && l.dark && !l.sparks) return globe::PLAIN;
    if (l.mono) return globe::PHOSPHOR;
    if (!l.dark) return globe::ATLAS;
    if (l.glow >= 3) return globe::NEON;
    float h, s;
    hue_sat(l.accent, &h, &s);
    if (h >= 18 && h <= 62 && s > 0.3f) return globe::ANTIQUE;   // a warm metal: brass, gold, copper
    return globe::PHOTO;
}

void dress(const orb_style::Look &l, globe::Kind kind, globe::Style &st) {
    const globe::Colours c = { l.bg, l.text, l.dim, l.accent, l.accent2, l.rule, l.glow };
    globe::make_style(st, kind, c);
    s_themed = kind != globe::PLAIN;
    s_skyGlow = s_themed && st.sky;
    s_dl.themed = s_themed;
    s_dl.font = orb_style::font(orb_style::SMALL, 12);
    if (!s_themed) {
        s_dl.city = COL_CITY; s_dl.home = COL_HOME;
        s_dl.ring = 0x000000; s_dl.ringOpa = LV_OPA_60;
        s_dl.text = 0xF0F0F0; s_dl.tag = 0x000000; s_dl.tagOpa = LV_OPA_50;
        s_dl.halo = false;
        return;
    }
    // home is the accent; the cities the second colour, or on paper (where the second
    // colour is a pale metal that the land's wash is made of) the accent too
    // (and plain text where the second colour is what the globe itself is drawn in)
    s_dl.home = l.accent;
    s_dl.city = !l.dark ? l.accent : (kind == globe::PHOSPHOR || kind == globe::ANTIQUE) ? l.text : l.accent2;
    s_dl.ring = l.dark ? 0x000000 : l.bg;
    s_dl.ringOpa = l.dark ? LV_OPA_60 : LV_OPA_COVER;
    s_dl.text = l.text;
    s_dl.tag = l.bg;
    s_dl.tagOpa = l.dark ? LV_OPA_60 : LV_OPA_80;
    s_dl.halo = l.glow >= 2;
}

void free_all() {
    s_ready = false;
    delete_dots();
    if (s_dots) { ps_free(s_dots); s_dots = nullptr; }
    if (s_lab) { ps_free(s_lab); s_lab = nullptr; }
    globe::render_free(s_ren);
}

} // namespace

namespace globeview {

void init() {
    if (s_scr) return;
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

    // The city lists: the built-in one until the relay answers.
    s_shared = (CityList *)ps_alloc(sizeof(CityList));
    s_net = (CityList *)ps_alloc(sizeof(CityList));
    if (s_shared) {
        memset(s_shared, 0, sizeof(CityList));
        const int n = (int)(sizeof(FALLBACK) / sizeof(FALLBACK[0]));
        memcpy(s_shared->c, FALLBACK, sizeof(FALLBACK));
        s_shared->n = n;
    }
    if (s_net) memset(s_net, 0, sizeof(CityList));
    ponderer::add({ "globe", net_step, ui_apply });
}

lv_obj_t *screen() { return s_scr; }

void onEnter() {
    if (!s_scr) return;
    const orb_style::Look &look = orb_style::look("globe");
    const globe::Kind kind = kind_for(look);
    if (!s_backdrop) s_backdrop = orb_style::attach(s_scr, "globe");
    if (!s_ready) {
        s_dots = (Dot *)ps_alloc(sizeof(Dot) * (MAX_CITIES + 1));
        s_lab = (LabelScratch *)ps_alloc(sizeof(LabelScratch));
        globe::Style *st = (globe::Style *)ps_alloc(sizeof(globe::Style));   // 800 bytes: not on the stack
        if (st) dress(look, kind, *st);
        const bool ok = s_dots && s_lab && st && globe::render_alloc(s_ren, *st);
        if (st) ps_free(st);
        if (!ok) {
            GLOG("[globe] PSRAM alloc failed; screen stays dark\n");
            free_all();
            return;
        }
        s_dotN = 0;
        s_ready = true;
    }
    s_spin = s_spinTarget = 0;
    s_lastTickMs = s_lastTurnMs = lv_tick_get();
    s_drawnFace = 1e9f;
    build_dots();
    s_drawUs = s_draws = 0;
    s_wasMoving = false;
    refresh(true);
    log_draws("first frame");
    s_active = true;
    s_nextMs = now_ms();                  // ask the relay now; hourly after that
    if (!s_timer) s_timer = lv_timer_create(tick_cb, TICK_MS, nullptr);
    static const char *const KINDS[] = { "photo", "photo, themed", "phosphor", "atlas", "antique", "neon" };
    GLOG("[globe] enter: %s globe, ~%u KB of PSRAM taken, subsolar tilt %.1f, facing %.1f%s\n", KINDS[kind],
         (unsigned)((globe::RW * globe::RW * 2 + globe::BOX * globe::BOX * (s_ren.st->chart ? 9 : 7) + sizeof(Dot) * (MAX_CITIES + 1) + sizeof(LabelScratch)) / 1024),
         s_tilt, s_face, s_clockOk ? "" : " (clock not set)");
}

void onExit() {
    s_active = false;
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    free_all();
    orb_style::release(s_backdrop);
    if (s_obj) lv_obj_invalidate(s_obj);
}

// Press: city names on or off.
void onPress() {
    if (!s_ready) return;
    s_labels = !s_labels;
    place_dots();
}

// A detent: spin the globe; it goes back to the Sun's view RETURN_MS after the last one.
void onTurn(int delta) {
    if (!s_ready) return;
    s_spinTarget += delta * SPIN_STEP;
    s_lastTurnMs = lv_tick_get();
}

} // namespace globeview

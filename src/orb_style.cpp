// How this fork's screens dress for the theme. See orb_style.h.
#include "orb_style.h"
#include "config.h"
#include "theme_select.h"
#include "theme_style.h"
#include "theme_font.h"
#include "theme_sd.h"
#include "theme_art.h"
#include <ArduinoJson.h>
#include <PNGdec.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#endif

namespace orb_style {

int constrain_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

namespace {

void *big_alloc(size_t n) {
#ifdef ARDUINO
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}
void big_free(void *p) {
#ifdef ARDUINO
    if (p) heap_caps_free(p);
#else
    free(p);
#endif
}
uint32_t rnd() {
#ifdef ARDUINO
    return esp_random();
#else
    return (uint32_t)arc4random();
#endif
}
float frand() { return (rnd() & 0xFFFF) / 65535.0f; }

// ---- the looks ------------------------------------------------------------------------

constexpr int MAX_SCREENS = 8;
struct PerScreen { char name[12]; Look look; };

bool      s_loaded = false;
Look      s_base;
PerScreen s_screens[MAX_SCREENS];
int       s_screenN = 0;

float luminance(uint32_t c) {
    const float r = powf(((c >> 16) & 255) / 255.0f, 2.2f);
    const float g = powf(((c >> 8) & 255) / 255.0f, 2.2f);
    const float b = powf((c & 255) / 255.0f, 2.2f);
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}
uint32_t mix(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int sh) { return (uint32_t)lroundf(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t) << sh; };
    return ch(16) | ch(8) | ch(0);
}

struct Preset { const char *key; const char *match; Look look; };

Look mk(uint32_t bg, uint32_t text, uint32_t dim, uint32_t accent, uint32_t accent2, uint32_t rule,
        bool dark, bool mono, uint8_t glow, bool sparks, uint8_t plateDim, const char *plate, const char *overlay) {
    Look l;
    l.bg = bg; l.text = text; l.dim = dim; l.accent = accent; l.accent2 = accent2; l.rule = rule;
    l.dark = dark; l.mono = mono; l.glow = glow; l.sparks = sparks; l.plateDim = plateDim;
    snprintf(l.plate, sizeof(l.plate), "%s", plate);
    snprintf(l.overlay, sizeof(l.overlay), "%s", overlay);
    return l;
}

// Tuned by eye against each stock theme's own screens. `match` is looked for, lower-case,
// in the theme's display name.
const Preset *presets(int &n) {
    static const Preset P[] = {
        // Aviator: ink on aged parchment, oxblood and brass.
        { "aviator",   "aviator",   mk(0xD9CDB4, 0x15171A, 0x5C4E3A, 0x7A2A1C, 0x7D6226, 0xA8997C, false, false, 0, false, 0, "menu_plate.png", "") },
        // Cold War 1983: green phosphor behind CRT glass, one colour.
        { "coldwar",   "cold war",  mk(0x07130A, 0xC0F8AA, 0x4CAF6D, 0xC0F8AA, 0x4CAF6D, 0x1D4A2A, true,  true,  2, false, 0, "menu_plate.png", "menu_overlay.png") },
        // Modern: near-black with a cool blue wash, white type.
        { "modern",    "modern",    mk(0x000000, 0xE8EDF2, 0x7A8290, 0x5B8CFF, 0x9FB8FF, 0x232A36, true,  false, 1, false, 0, "menu_plate.png", "") },
        // Steam Punk: cream on dark leather, brass and verdigris.
        { "steampunk", "steam",     mk(0x0E0A06, 0xFCF1C1, 0xB89A6A, 0xDCAE72, 0x4FA7A0, 0x5A4630, true,  false, 1, false, 60, "menu_plate.png", "") },
        // The fork's own: neon and glass on black. Its theme folder carries the real thing
        // in extras_style.json; this is the same look for the simulator's ORB_STYLE.
        { "plasma",    "plasma",    mk(0x000000, 0xF4EAFF, 0x9A7FC0, 0xFF4FD8, 0x4FD8FF, 0x2A1840, true,  false, 3, true,  0, "extras_plate.png", "extras_overlay.png") },
        // No theme: as these screens were first drawn.
        { "plain",     "\x01",      Look() },
    };
    n = (int)(sizeof(P) / sizeof(P[0]));
    return P;
}

uint32_t json_color(JsonVariantConst v, uint32_t fallback) {
    if (v.is<const char *>()) {
        const char *s = v.as<const char *>();
        if (s && s[0] == '#' && strlen(s) == 7) return (uint32_t)strtoul(s + 1, nullptr, 16);
        return fallback;
    }
    if (v.is<uint32_t>()) return v.as<uint32_t>() & 0xFFFFFF;
    return fallback;
}

void merge(Look &l, JsonVariantConst j) {
    if (j.isNull()) return;
    l.bg      = json_color(j["bg"], l.bg);
    l.text    = json_color(j["text"], l.text);
    l.dim     = json_color(j["dim"], l.dim);
    l.accent  = json_color(j["accent"], l.accent);
    l.accent2 = json_color(j["accent2"], l.accent2);
    l.rule    = json_color(j["rule"], l.rule);
    if (j["dark"].is<bool>())   l.dark = j["dark"].as<bool>();
    if (j["mono"].is<bool>())   l.mono = j["mono"].as<bool>();
    if (j["sparks"].is<bool>()) l.sparks = j["sparks"].as<bool>();
    if (j["glow"].is<int>())    l.glow = (uint8_t)constrain_int(j["glow"].as<int>(), 0, 3);
    if (j["plateDim"].is<int>()) l.plateDim = (uint8_t)constrain_int(j["plateDim"].as<int>(), 0, 255);
    if (j["plate"].is<const char *>())   snprintf(l.plate, sizeof(l.plate), "%s", j["plate"].as<const char *>());
    if (j["overlay"].is<const char *>()) snprintf(l.overlay, sizeof(l.overlay), "%s", j["overlay"].as<const char *>());
}

// What any theme gives without knowing these screens exist.
Look derive() {
    Look l;
    const theme_style::Menu &m = theme_style::menu();
    l.text = m.current.color;
    l.dark = luminance(l.text) > 0.2f;
    l.bg   = l.dark ? 0x000000 : 0xE4DCCB;
    l.dim  = mix(l.text, l.bg, 0.45f);
    l.rule = mix(l.text, l.bg, 0.78f);
    l.accent  = m.prev.show ? m.prev.color : l.text;
    if (fabsf(luminance(l.accent) - luminance(l.bg)) < 0.08f) l.accent = l.text;
    l.accent2 = l.dim;
    if (theme_style::hasAsset("menu_plate.png"))   snprintf(l.plate, sizeof(l.plate), "menu_plate.png");
    if (theme_style::hasAsset("menu_overlay.png")) snprintf(l.overlay, sizeof(l.overlay), "menu_overlay.png");
    return l;
}

void load() {
    s_loaded = true;
    s_screenN = 0;
    s_base = Look();
    const char *slug = theme_select::activeSlug();
    const bool themed = slug && slug[0];

    int n = 0;
    const Preset *P = presets(n);
    const Preset *hit = nullptr;
#ifndef ARDUINO
    if (const char *force = getenv("ORB_STYLE"))
        for (int i = 0; i < n; ++i) if (!strcasecmp(force, P[i].key)) hit = &P[i];
#endif
    if (themed && !hit) {
        s_base = derive();
        char name[64];
        snprintf(name, sizeof(name), "%s", theme_style::themeLabel());
        for (char *c = name; *c; ++c) *c = (char)tolower((unsigned char)*c);
        for (int i = 0; i < n; ++i) if (strstr(name, P[i].match)) { hit = &P[i]; break; }
    }
    if (hit) s_base = hit->look;

    // The theme's own word on the matter, when it has one.
    if (themed) {
        char path[96];
        snprintf(path, sizeof(path), "/themes/%s/extras_style.json", slug);
        size_t len = 0;
        if (uint8_t *buf = theme_sd::read_whole(path, len, 16 * 1024)) {
            JsonDocument doc;
            if (!deserializeJson(doc, buf, len)) {
                merge(s_base, doc.as<JsonVariantConst>());
                for (JsonPairConst kv : doc["screens"].as<JsonObjectConst>()) {
                    if (s_screenN >= MAX_SCREENS) break;
                    PerScreen &ps = s_screens[s_screenN++];
                    snprintf(ps.name, sizeof(ps.name), "%s", kv.key().c_str());
                    ps.look = s_base;
                    merge(ps.look, kv.value());
                }
            }
            theme_sd::free(buf);
        }
    }
    printf("[orb_style] %s: text #%06x on #%06x, accent #%06x, plate '%s', glass '%s'%s%s\n",
           hit ? hit->key : (themed ? "derived" : "plain"), (unsigned)s_base.text, (unsigned)s_base.bg,
           (unsigned)s_base.accent, s_base.plate, s_base.overlay,
           s_base.mono ? ", mono" : "", s_base.sparks ? ", sparks" : "");
}

// ---- pictures ---------------------------------------------------------------------------

PNG *s_png = nullptr;
uint8_t *s_dst = nullptr;
int      s_dstW = 0;
bool     s_dstAlpha = false;

int png_line(PNGDRAW *d) {
    const uint8_t *src = d->pPixels;
    const bool rgba = d->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA && d->iBpp == 8;
    const bool rgb  = d->iPixelType == PNG_PIXEL_TRUECOLOR && d->iBpp == 8;
    if (!rgba && !rgb) return 1;   // left as zeros; the caller treats an unreadable picture as none
    const int step = rgba ? 4 : 3;
    if (s_dstAlpha) {
        uint8_t *dst = s_dst + (size_t)d->y * s_dstW * 3;
        for (int x = 0; x < d->iWidth; ++x, dst += 3, src += step) {
            const uint16_t v = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3));
            dst[0] = v & 0xFF; dst[1] = v >> 8; dst[2] = rgba ? src[3] : 255;
        }
    } else {
        uint16_t *dst = (uint16_t *)s_dst + (size_t)d->y * s_dstW;
        for (int x = 0; x < d->iWidth; ++x, src += step)
            dst[x] = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3));
    }
    return 1;
}

// A picture from the active theme as LVGL pixels. `owned` says whether to free it: a hit
// in the flash cache is memory-mapped and must not be.
uint8_t *load_image(const char *name, bool alpha, int &w, int &h, bool &owned) {
    owned = false;
    if (!name[0]) return nullptr;
    const char *slug = theme_select::activeSlug();
    if (!slug || !slug[0]) return nullptr;
    if (const uint8_t *p = theme_art::find_active(name, alpha ? theme_art::FMT_RGB565_ALPHA : theme_art::FMT_RGB565, w, h))
        return (uint8_t *)p;
    char path[96];
    snprintf(path, sizeof(path), "/themes/%s/%s", slug, name);
    size_t len = 0;
    uint8_t *file = theme_sd::read_whole(path, len, 2 * 1024 * 1024);
    if (!file) return nullptr;
    if (!s_png) { if (void *mem = big_alloc(sizeof(PNG))) s_png = new (mem) PNG(); }
    uint8_t *out = nullptr;
    if (s_png && s_png->openRAM(file, (int)len, png_line) == PNG_SUCCESS) {
        w = s_png->getWidth(); h = s_png->getHeight();
        const size_t bytes = (size_t)w * h * (alpha ? 3 : 2);
        if (w > 0 && h > 0 && w <= 1024 && (out = (uint8_t *)big_alloc(bytes))) {
            memset(out, 0, bytes);
            s_dst = out; s_dstW = w; s_dstAlpha = alpha;
            if (s_png->decode(nullptr, 0) != PNG_SUCCESS) { big_free(out); out = nullptr; }
        }
        s_png->close();
    }
    theme_sd::free(file);
    owned = out != nullptr;
    return out;
}

const lv_font_t *montserrat(int px) {
    switch (px) {
        case 12: return &lv_font_montserrat_12; case 14: return &lv_font_montserrat_14;
        case 16: return &lv_font_montserrat_16; case 18: return &lv_font_montserrat_18;
        case 20: return &lv_font_montserrat_20; case 22: return &lv_font_montserrat_22;
        case 24: return &lv_font_montserrat_24; case 26: return &lv_font_montserrat_26;
        case 28: return &lv_font_montserrat_28; case 32: return &lv_font_montserrat_32;
        case 36: return &lv_font_montserrat_36; case 40: return &lv_font_montserrat_40;
        case 44: return &lv_font_montserrat_44; case 48: return &lv_font_montserrat_48;
    }
    return px < 12 ? &lv_font_montserrat_12 : px > 48 ? &lv_font_montserrat_48 : &lv_font_montserrat_16;
}

// The theme's face for a role, or null when it has none for that slot.
const lv_font_t *theme_face(Role role) {
    const char *slug = theme_select::activeSlug();
    if (!slug || !slug[0]) return nullptr;
    switch (role) {
        case TITLE: return theme_font::intel_has_font(0) ? theme_font::intel_title() : nullptr;
        case BODY:  return theme_font::intel_has_font(1) ? theme_font::intel_text() : nullptr;
        case SMALL: return theme_font::intel_has_font(2) ? theme_font::intel_source() : nullptr;
    }
    return nullptr;
}

// ---- sparks -----------------------------------------------------------------------------

constexpr int SPARKS = 18;
struct Spark { float x, y, vx, vy, life, span; uint32_t color; };

}  // namespace

struct Backdrop {
    lv_obj_t *screen = nullptr;
    lv_obj_t *plate = nullptr, *dimmer = nullptr, *glass = nullptr, *sparks = nullptr;
    uint8_t  *plateBuf = nullptr, *glassBuf = nullptr;
    bool      plateOwned = false, glassOwned = false;
    lv_img_dsc_t plateDsc = {}, glassDsc = {};
    lv_timer_t *timer = nullptr;
    Spark     spark[SPARKS] = {};
    uint32_t  c1 = 0, c2 = 0;
};

namespace {

void spark_born(Backdrop *b, Spark &s) {
    // Born somewhere in the middle two thirds of the glass, drifting up and outwards.
    const float a = frand() * 6.2832f, r = 40 + frand() * 150;
    s.x = SCREEN_W / 2 + cosf(a) * r;
    s.y = SCREEN_H / 2 + sinf(a) * r;
    const float d = frand() * 6.2832f, v = 12 + frand() * 38;
    s.vx = cosf(d) * v;
    s.vy = sinf(d) * v - 18;
    s.span = 0.5f + frand() * 1.3f;
    s.life = s.span;
    const float pick = frand();
    s.color = pick < 0.45f ? b->c1 : pick < 0.85f ? b->c2 : 0xFFFFFF;
}

void spark_area(const Spark &s, lv_area_t &a) {
    a.x1 = (lv_coord_t)s.x - 6; a.y1 = (lv_coord_t)s.y - 6;
    a.x2 = (lv_coord_t)s.x + 6; a.y2 = (lv_coord_t)s.y + 6;
}

void sparks_tick(lv_timer_t *t) {
    Backdrop *b = (Backdrop *)t->user_data;
    if (!b->sparks || lv_scr_act() != b->screen) return;
    const float dt = 0.05f;
    for (Spark &s : b->spark) {
        lv_area_t a;
        if (s.life > 0) { spark_area(s, a); lv_obj_invalidate_area(b->sparks, &a); }
        s.life -= dt;
        if (s.life <= 0) {
            if (frand() < 0.12f) spark_born(b, s);   // a trickle, not a burst
            else continue;
        }
        s.x += s.vx * dt; s.y += s.vy * dt;
        s.vy -= 6 * dt;                               // they rise
        spark_area(s, a);
        lv_obj_invalidate_area(b->sparks, &a);
    }
}

void sparks_draw(lv_event_t *e) {
    Backdrop *b = (Backdrop *)lv_event_get_user_data(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    for (const Spark &s : b->spark) {
        if (s.life <= 0) continue;
        const float k = s.life / s.span;                     // 1 at birth, 0 at death
        const float a = k < 0.8f ? k / 0.8f : (1 - k) / 0.2f; // quick in, slow out
        d.bg_color = lv_color_hex(s.color);
        lv_area_t halo = { (lv_coord_t)(s.x - 4), (lv_coord_t)(s.y - 4), (lv_coord_t)(s.x + 4), (lv_coord_t)(s.y + 4) };
        d.bg_opa = (lv_opa_t)(a * 46);
        lv_draw_rect(ctx, &d, &halo);
        lv_area_t core = { (lv_coord_t)(s.x - 1), (lv_coord_t)(s.y - 1), (lv_coord_t)(s.x + 1), (lv_coord_t)(s.y + 1) };
        d.bg_opa = (lv_opa_t)(a * 255);
        lv_draw_rect(ctx, &d, &core);
    }
}

lv_obj_t *layer(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, SCREEN_W, SCREEN_H);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    return o;
}

void set_dsc(lv_img_dsc_t &d, const uint8_t *px, int w, int h, bool alpha) {
    d.header.always_zero = 0;
    d.header.w = w; d.header.h = h;
    d.header.cf = alpha ? LV_IMG_CF_TRUE_COLOR_ALPHA : LV_IMG_CF_TRUE_COLOR;
    d.data_size = (uint32_t)w * h * (alpha ? 3 : 2);
    d.data = px;
}

}  // namespace

const Look &look(const char *screen) {
    if (!s_loaded) load();
    if (screen)
        for (int i = 0; i < s_screenN; ++i) if (!strcmp(s_screens[i].name, screen)) return s_screens[i].look;
    return s_base;
}

void reload() { s_loaded = false; }

bool themed_font(Role role, int px) {
    const lv_font_t *f = theme_face(role);
    if (!f) return false;
    // A theme face is one fixed size. Take it when it is near what was asked for; a face
    // made for 14 px captions is no use as a 40 px headline, and the other way round.
    const float want = px * 1.22f, have = (float)f->line_height;
    return have >= want * 0.72f && have <= want * 1.4f;
}

const lv_font_t *font(Role role, int px) {
    return themed_font(role, px) ? theme_face(role) : montserrat(px);
}

Backdrop *attach(lv_obj_t *screen, const char *screenName) {
    const Look &l = look(screenName);
    Backdrop *b = new Backdrop();
    b->screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(l.bg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    int w = 0, h = 0;
    if ((b->plateBuf = load_image(l.plate, false, w, h, b->plateOwned))) {
        set_dsc(b->plateDsc, b->plateBuf, w, h, false);
        b->plate = lv_img_create(screen);
        lv_obj_add_flag(b->plate, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
        lv_obj_clear_flag(b->plate, LV_OBJ_FLAG_CLICKABLE);
        lv_img_set_src(b->plate, &b->plateDsc);
        lv_obj_center(b->plate);
        lv_obj_move_background(b->plate);
        if (l.plateDim) {
            b->dimmer = layer(screen);
            lv_obj_set_style_bg_color(b->dimmer, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(b->dimmer, l.plateDim, 0);
            lv_obj_move_to_index(b->dimmer, 1);
        }
    }
    if (l.sparks) {
        b->c1 = l.accent; b->c2 = l.accent2;
        b->sparks = layer(screen);
        lv_obj_add_event_cb(b->sparks, sparks_draw, LV_EVENT_DRAW_MAIN, b);
        b->timer = lv_timer_create(sparks_tick, 50, b);
    }
    if ((b->glassBuf = load_image(l.overlay, true, w, h, b->glassOwned))) {
        set_dsc(b->glassDsc, b->glassBuf, w, h, true);
        b->glass = lv_img_create(screen);
        lv_obj_add_flag(b->glass, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
        lv_obj_clear_flag(b->glass, LV_OBJ_FLAG_CLICKABLE);
        lv_img_set_src(b->glass, &b->glassDsc);
        lv_obj_center(b->glass);
    }
    raise(b);
    return b;
}

void raise(Backdrop *b) {
    if (!b) return;
    if (b->sparks) lv_obj_move_foreground(b->sparks);
    if (b->glass)  lv_obj_move_foreground(b->glass);
}

void release(Backdrop *&b) {
    if (!b) return;
    if (b->timer) lv_timer_del(b->timer);
    for (lv_obj_t *o : { b->plate, b->dimmer, b->sparks, b->glass }) if (o) lv_obj_del(o);
    // The image cache may still hold these descriptors; drop them before the pixels go.
    lv_img_cache_invalidate_src(&b->plateDsc);
    lv_img_cache_invalidate_src(&b->glassDsc);
    if (b->plateOwned) big_free(b->plateBuf);
    if (b->glassOwned) big_free(b->glassBuf);
    delete b;
    b = nullptr;
}

}  // namespace orb_style

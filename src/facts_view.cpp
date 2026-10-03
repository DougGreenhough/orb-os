// Facts: one fact at a time, set as large as it will go on the round glass.
//
// Three layers, all inside one full-screen card so a single opacity fades them together:
//   - the fact itself, one centred label whose line breaks facts_layout chose against the
//     circle's chord at each line's own height, at the largest size that fits;
//   - the topic, small tracked capitals curved along the top of the dial, in a colour of
//     its own so consecutive topics are told apart at a glance;
//   - the source, smaller still and dim, curved along the bottom.
// The two arcs are drawn by curved_text into small RGB565+alpha canvases (65 KB each, in
// PSRAM, taken on entry and given back on exit). If either allocation fails the arc is
// drawn as a straight label instead, and the serial log says so.
//
// Data comes from facts_client: one fact is always kept prefetched, so a change (every
// FACT_MS, or on a press) is instant. When the screen has nothing to show, it says which
// thing is unwell in the fact's own place.
//
// Dress: colours, faces and glow come from orb_style::look("facts"), read into s_pal. With no
// theme they are the ones this screen was first drawn in. A theme's own body face is used
// for a fact whenever the whole fact sets in it inside the circle (it exists at one size
// only); otherwise the fact is set from the Montserrat ladder as before. Where the look
// glows hard the fact is drawn through curved_text into a canvas of its own (415 KB of
// PSRAM, with the arcs' 137 KB; taken on entry, given back on exit) so the letters can
// glow, and falls back to the plain label if that cannot be had.
#include "facts_view.h"
#include "facts_client.h"
#include "facts_layout.h"
#include "ponderer.h"
#include "curved_text.h"
#include "orb_style.h"
#include "font_ladder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif
#ifdef ARDUINO
#include <WiFi.h>
#endif

namespace {

constexpr int SCREEN = 466;
constexpr int MID = SCREEN / 2;

constexpr uint32_t FACT_MS    = 45000;   // how long a fact stays up
constexpr uint32_t FADE_OUT_MS = 180;
constexpr uint32_t FADE_IN_MS  = 320;
constexpr uint32_t TURN_GAP_MS = 600;   // at most one change per this, from turning
constexpr uint32_t PRESS_WAIT_MS = 6000; // a press with nothing in hand gives up after this

// The fact is fitted inside this circle. The topic and source arcs live outside it, in the
// ring between it and the glass, so the three can never collide whatever the text does.
constexpr int BODY_R = 184;
constexpr int ARC_R  = 205;              // centreline of the arc text
constexpr int TOPIC_TRACK  = 2;          // px between the topic's capitals
constexpr int SOURCE_TRACK = 1;          // and the source's mixed case, which needs less

// The arc canvases: a band across the top of the dial and its mirror at the bottom.
constexpr int ARC_W = 300, ARC_H = 76, ARC_Y = 14;
constexpr int ARC_X = MID - ARC_W / 2;

// The fact's canvas, when the look glows enough to want one: the body circle's square.
constexpr int BODY_W = 2 * BODY_R + 4;

// A theme's body face exists at one size, and for the stock themes that is 16 to 21 px:
// a short fact the ladder would set at 48 px comes out a third of the size in it. At 0 the
// theme's face is used whenever the fact fits in it; at, say, 60 it is used only when it is
// at least 60% of the height the ladder would have given the same fact.
constexpr int THEME_FACE_MIN_PCT = 60;

// What this screen is drawn in. Filled from the look by palette_from().
constexpr int MAX_TOPIC_COLS = 6;
struct Palette {
    uint32_t bg, text, dim, source, halo;
    // Topic colours. Chosen by the topic's name, so "Planets" is always the same colour and
    // a change of topic is visible before a word is read.
    uint32_t topic[MAX_TOPIC_COLS];
    int      topics;
    uint8_t  glow;       // 0..3
    bool     mono;
};
Palette s_pal;

uint32_t mix(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int sh) { return (uint32_t)lroundf(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t) << sh; };
    return ch(16) | ch(8) | ch(0);
}

// The look with no theme at all, which must draw exactly what this screen always drew.
bool plain(const orb_style::Look &l) {
    const orb_style::Look d;
    return l.bg == d.bg && l.text == d.text && l.dim == d.dim && l.accent == d.accent &&
           l.accent2 == d.accent2 && l.rule == d.rule && l.dark && !l.mono && !l.glow && !l.plate[0];
}

Palette palette_from(const orb_style::Look &l) {
    Palette p = {};
    p.bg = l.bg; p.glow = l.glow; p.mono = l.mono;
    if (plain(l)) {
        p.text = 0xF4F2EC;   // a warm white: less glare than 0xFFFFFF on AMOLED
        p.dim = 0x8A8F98;
        p.source = 0x5E636C;
        static const uint32_t cols[] = { 0xF2B45A, 0x6FD3C7, 0xA99BFF, 0x8FD18B, 0xFF8A7A, 0x7FB8FF };
        for (uint32_t c : cols) p.topic[p.topics++] = c;
        return p;
    }
    p.text = l.text; p.dim = l.dim;
    // The source is the quietest thing on the glass, but ink on paper has less room to be
    // quiet in than light on black.
    p.source = (l.dark && !l.mono) ? mix(l.dim, l.bg, 0.2f) : l.dim;
    p.halo = l.mono ? l.dim : l.accent;
    if (l.mono) {
        p.topic[p.topics++] = l.accent;
    } else {
        // The theme's two accents and a step from each towards the text colour: four that
        // belong to the theme and can still be told apart.
        p.topic[p.topics++] = l.accent;
        p.topic[p.topics++] = l.accent2;
        p.topic[p.topics++] = mix(l.accent, l.text, 0.45f);
        p.topic[p.topics++] = mix(l.accent2, l.text, 0.45f);
    }
    return p;
}

// Faces. The theme's where it has one that can set what is asked of it, Montserrat where not.
const lv_font_t *s_topicFont = &lv_font_montserrat_16;
const lv_font_t *s_sourceFont = &lv_font_montserrat_12;
const lv_font_t *s_bodyFace = nullptr;    // the theme's body face, or null

struct Arc {
    lv_obj_t *canvas = nullptr;
    uint8_t  *buf = nullptr;
    lv_obj_t *fallback = nullptr;   // straight label, used only if the canvas could not be had
    bool      bottom = false;
};

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_card = nullptr;       // everything that fades
lv_obj_t *s_body = nullptr;       // the fact
lv_obj_t *s_bodyCanvas = nullptr; // the fact again, glowing, where the look asks for it
uint8_t  *s_bodyBuf = nullptr;
orb_style::Backdrop *s_backdrop = nullptr;   // plate, glass and sparks while showing
lv_obj_t *s_emptyTitle = nullptr;
lv_obj_t *s_emptyText = nullptr;
Arc s_topic, s_source;
lv_timer_t *s_cycle = nullptr;    // FACT_MS between facts
lv_timer_t *s_giveUp = nullptr;   // undoes a press's dimming if nothing arrives
lv_timer_t *s_recheck = nullptr;  // keeps an empty state's reason current

facts::Fact s_cur = {};
bool s_have = false;              // s_cur is on screen
bool s_showing = false;
bool s_wantNext = false;          // the next arrival should replace s_cur at once
bool s_fading = false;            // a fade-out is running; its end shows s_pending
facts::Fact s_pending = {};

// ---- small builders ------------------------------------------------------------------

lv_obj_t *blank(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

uint32_t topic_colour(const char *topic) {
    uint32_t h = 2166136261u;   // FNV-1a over the lower-cased name
    for (const char *p = topic; *p; ++p) h = (h ^ (uint8_t)tolower((unsigned char)*p)) * 16777619u;
    return s_pal.topic[h % (uint32_t)s_pal.topics];
}

// ---- legibility on the plate ---------------------------------------------------------
//
// The arcs sit in the outer ring of the glass, which is exactly where a theme's backdrop
// does the most: Aviator's parchment burns to brown there and Modern's blue wash is at its
// brightest. The look's colours were chosen for the middle of the plate, so on entry the
// plate is sampled under each arc and an arc colour that would not stand out from it is
// moved towards the text colour until it does.

uint32_t s_under[2] = { 0, 0 };   // what is behind the topic arc and the source arc
bool s_sampled = false;

float lum(uint32_t c) {
    auto ch = [&](int sh) { return powf(((c >> sh) & 255) / 255.0f, 2.2f); };
    return 0.2126f * ch(16) + 0.7152f * ch(8) + 0.0722f * ch(0);
}
float contrast(uint32_t a, uint32_t b) {
    const float la = lum(a) + 0.05f, lb = lum(b) + 0.05f;
    return la > lb ? la / lb : lb / la;
}

// The backdrop's layers are the screen's children behind the card: a plate image and
// perhaps a wash of black over it. Read what they come to under each arc.
void sample_backdrop() {
    s_sampled = false;
    float acc[2][3] = {};
    int n[2] = {};
    bool any = false;
    const uint32_t kids = lv_obj_get_child_cnt(s_scr);
    for (uint32_t i = 0; i < kids; ++i) {
        lv_obj_t *o = lv_obj_get_child(s_scr, i);
        if (o == s_card) break;
        if (lv_obj_check_type(o, &lv_img_class)) {
            const void *src = lv_img_get_src(o);
            if (!src || lv_img_src_get_type(src) != LV_IMG_SRC_VARIABLE) continue;
            const lv_img_dsc_t *d = (const lv_img_dsc_t *)src;
            if (d->header.cf != LV_IMG_CF_TRUE_COLOR || !d->data) continue;
            const int w = d->header.w, h = d->header.h;
            for (int arc = 0; arc < 2; ++arc) {
                for (int k = -4; k <= 4; ++k) {
                    const float a = k * 0.09f + (arc ? (float)M_PI : 0.0f);
                    const int x = w / 2 + (int)lroundf(sinf(a) * ARC_R), y = h / 2 - (int)lroundf(cosf(a) * ARC_R);
                    if (x < 0 || y < 0 || x >= w || y >= h) continue;
                    const lv_color_t c = ((const lv_color_t *)d->data)[y * w + x];
                    const uint32_t rgb = lv_color_to32(c);
                    acc[arc][0] += (rgb >> 16) & 255; acc[arc][1] += (rgb >> 8) & 255; acc[arc][2] += rgb & 255;
                    ++n[arc];
                }
            }
            any = true;
        } else if (any) {
            // The dimmer: black at some opacity over the plate.
            const float keep = 1.0f - lv_obj_get_style_bg_opa(o, 0) / 255.0f;
            for (auto &a : acc) for (float &v : a) v *= keep;
        }
    }
    if (!any || !n[0] || !n[1]) return;
    for (int arc = 0; arc < 2; ++arc)
        s_under[arc] = ((uint32_t)(acc[arc][0] / n[arc]) << 16) | ((uint32_t)(acc[arc][1] / n[arc]) << 8) |
                       (uint32_t)(acc[arc][2] / n[arc]);
    s_sampled = true;
}

// `col`, or the nearest step from it towards the text colour that reaches `want` contrast
// against what is behind arc `arc`. Unchanged when there is no plate.
uint32_t legible(uint32_t col, int arc, float want) {
    if (!s_sampled) return col;
    // Small thin letters in ink on paper need far more than the same letters lit on black.
    if (lum(s_under[arc]) > 0.25f) want *= 2.4f;
    uint32_t c = col;
    for (int step = 1; step <= 5 && contrast(c, s_under[arc]) < want; ++step) c = mix(col, s_pal.text, step * 0.2f);
    return c;
}

// How far letters glow at each level of the look, in px: arcs, then the fact itself.
int arc_glow()  { static const int g[4] = { 0, 1, 3, 5 }; return g[s_pal.glow]; }
int body_glow() { static const int g[4] = { 0, 0, 4, 6 }; return g[s_pal.glow]; }

void *big_alloc(size_t sz) {
#if defined(ESP_PLATFORM)
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
    return malloc(sz);
#endif
}
void big_free(void *p) {
#if defined(ESP_PLATFORM)
    heap_caps_free(p);
#else
    free(p);
#endif
}

// Every byte of `s` is a character f itself can set (a theme's faces are plain ASCII).
bool covers(const lv_font_t *f, const char *s) {
    lv_font_glyph_dsc_t g;
    for (; *s; ++s) {
        if (*s == ' ' || *s == '\n') continue;
        if ((uint8_t)*s >= 0x80 || !f->get_glyph_dsc(f, &g, (uint32_t)(uint8_t)*s, 0)) return false;
    }
    return true;
}

// curved_text reads glyph bitmaps as 4 bits a pixel and strings a byte at a time.
bool canvas_can_set(const lv_font_t *f, const char *s) {
    if (f->get_glyph_dsc != lv_font_get_glyph_dsc_fmt_txt) return false;
    if (((const lv_font_fmt_txt_dsc_t *)f->dsc)->bpp != 4) return false;
    for (; *s; ++s) if ((uint8_t)*s >= 0x80) return false;
    return true;
}

// ---- the arcs ------------------------------------------------------------------------

void arc_attach(Arc &a) {
    if (a.buf) return;
    const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(ARC_W, ARC_H);
    a.buf = (uint8_t *)big_alloc(sz);
    if (!a.buf) {
        printf("[facts] arc canvas alloc FAILED (%u bytes) - drawing it straight\n", (unsigned)sz);
        return;
    }
    a.canvas = lv_canvas_create(s_card);
    lv_canvas_set_buffer(a.canvas, a.buf, ARC_W, ARC_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_obj_set_pos(a.canvas, ARC_X, a.bottom ? SCREEN - ARC_Y - ARC_H : ARC_Y);
    lv_obj_clear_flag(a.canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

void arc_release(Arc &a) {
    if (a.canvas) { lv_obj_del(a.canvas); a.canvas = nullptr; }
    if (a.buf) { big_free(a.buf); a.buf = nullptr; }
}

// The fact's own canvas. Only where the look glows enough for it to show.
void body_canvas_take() {
    if (s_bodyBuf || !body_glow()) return;
    const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(BODY_W, BODY_W);
    s_bodyBuf = (uint8_t *)big_alloc(sz);
    if (!s_bodyBuf) {
        printf("[facts] body canvas alloc FAILED (%u bytes) - no glow\n", (unsigned)sz);
        return;
    }
    memset(s_bodyBuf, 0, sz);
    s_bodyCanvas = lv_canvas_create(s_card);
    lv_canvas_set_buffer(s_bodyCanvas, s_bodyBuf, BODY_W, BODY_W, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_obj_center(s_bodyCanvas);
    lv_obj_clear_flag(s_bodyCanvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_bodyCanvas, LV_OBJ_FLAG_HIDDEN);
}

void body_canvas_give() {
    if (s_bodyCanvas) { lv_obj_del(s_bodyCanvas); s_bodyCanvas = nullptr; }
    if (s_bodyBuf) { big_free(s_bodyBuf); s_bodyBuf = nullptr; }
}

// Letter-spaced text along the arc. curved_text lays glyphs edge to edge, which is right
// for words and cramped for small capitals, so each character goes down as its own
// one-glyph arc at an angle this works out, and the rotation maths stays in curved_text.
void arc_draw(Arc &a, const char *str, const lv_font_t *font, uint32_t col, int track) {
    if (a.fallback) {
        lv_label_set_text(a.fallback, str);
        lv_obj_set_style_text_color(a.fallback, lv_color_hex(col), 0);
        lv_obj_set_style_text_font(a.fallback, font, 0);
        if (a.buf) lv_obj_add_flag(a.fallback, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(a.fallback, LV_OBJ_FLAG_HIDDEN);
    }
    if (!a.buf) return;
    memset(a.buf, 0, LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(ARC_W, ARC_H));
    const int n = (int)strlen(str);
    float total = 0;
    for (int i = 0; i < n; ++i) total += lv_txt_get_width(&str[i], 1, font, 0, LV_TEXT_FLAG_NONE);
    total += track * (n > 1 ? n - 1 : 0);
    // The dial's centre in canvas coordinates.
    const float cx = MID - ARC_X;
    const float cy = a.bottom ? (float)(MID - (SCREEN - ARC_Y - ARC_H)) : (float)(MID - ARC_Y);
    const float centreDeg = a.bottom ? 180.0f : 0.0f;
    const float dir = a.bottom ? -1.0f : 1.0f;   // left to right runs anticlockwise underneath
    float cursor = -total / 2;
    const curved_text::Target dst = { a.buf, ARC_W, ARC_H };
    for (int i = 0; i < n; ++i) {
        const float w = lv_txt_get_width(&str[i], 1, font, 0, LV_TEXT_FLAG_NONE);
        const float deg = centreDeg + dir * (cursor + w / 2) / ARC_R * 180.0f / (float)M_PI;
        cursor += w + track;
        if (str[i] == ' ') continue;
        const char one[2] = { str[i], 0 };
        curved_text::draw_arc(dst, font, one, cx, cy, ARC_R, deg, lv_color_hex(col), arc_glow(),
                              lv_color_hex(col));
    }
    lv_obj_invalidate(a.canvas);
}

void arc_build(Arc &a, bool bottom) {
    a.bottom = bottom;
    a.fallback = label(s_card, &lv_font_montserrat_14, s_pal.dim);
    lv_obj_align(a.fallback, bottom ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_TOP_MID, 0, bottom ? -34 : 34);
    lv_obj_add_flag(a.fallback, LV_OBJ_FLAG_HIDDEN);
}

// ---- drawing -------------------------------------------------------------------------

void show(lv_obj_t *o, bool on) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void upper_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; ++i) dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = 0;
}

void draw_arcs() {
    char buf[40];
    if (s_have) {
        upper_copy(buf, sizeof(buf), s_cur.topic[0] ? s_cur.topic : "Fact");
        arc_draw(s_topic, buf, covers(s_topicFont, buf) ? s_topicFont : font_ladder(16),
                 legible(topic_colour(s_cur.topic), 0, 3.0f), TOPIC_TRACK);
        arc_draw(s_source, s_cur.source, covers(s_sourceFont, s_cur.source) ? s_sourceFont : font_ladder(12),
                 legible(s_pal.source, 1, 2.4f), SOURCE_TRACK);
    } else {
        arc_draw(s_topic, "FACTS", s_topicFont, legible(s_pal.source, 0, 2.4f), TOPIC_TRACK);
        arc_draw(s_source, "", s_sourceFont, s_pal.source, SOURCE_TRACK);
    }
}

void draw_fact() {
    static facts_layout::Result r;   // 300+ bytes; not on the LVGL task's stack
    // The theme's own face when the whole fact sets in it; the ladder when it does not.
    bool themed = s_bodyFace && covers(s_bodyFace, s_cur.text) &&
                  facts_layout::fit_face(s_cur.text, BODY_R, s_bodyFace, r);
    if (themed && THEME_FACE_MIN_PCT > 0) {
        static facts_layout::Result ladder;
        if (facts_layout::fit(s_cur.text, BODY_R, ladder) &&
            lv_font_get_line_height(s_bodyFace) * 100 < lv_font_get_line_height(ladder.font) * THEME_FACE_MIN_PCT) {
            r = ladder;
            themed = false;
        }
    }
    if (!themed && !facts_layout::fit(s_cur.text, BODY_R, r)) return;
    lv_obj_set_style_text_font(s_body, r.font, 0);
    lv_obj_set_style_text_line_space(s_body, r.lineSpace, 0);
    lv_label_set_text(s_body, r.text);
    lv_obj_center(s_body);
    printf("[facts] %d px%s, %d lines, widest %d: %s\n", r.size, themed ? " (theme face)" : "",
           r.lines, r.widest, s_cur.id);

    // The same block again with a glow, in place of the label, where the look wants one.
    const bool glowing = s_bodyBuf && canvas_can_set(r.font, r.text);
    show(s_body, !glowing);
    if (s_bodyCanvas) show(s_bodyCanvas, glowing);
    if (!glowing) return;
    memset(s_bodyBuf, 0, LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(BODY_W, BODY_W));
    const curved_text::Target dst = { s_bodyBuf, BODY_W, BODY_W };
    curved_text::draw_straight(dst, r.font, r.text, BODY_W / 2.0f, BODY_W / 2.0f, lv_color_hex(s_pal.text),
                               body_glow(), lv_color_hex(s_pal.halo), 1, 255, curved_text::Pill(), r.lineSpace);
    lv_obj_invalidate(s_bodyCanvas);
}

const char *const *empty_words(facts::Status st) {
    static const char *const waiting[]  = { "One moment", "Fetching a fact." };
    static const char *const unconf[]   = { "No relay key", "Facts come from orb-ponderer. Put its device key in src/ponderer_secrets.h." };
    static const char *const offline[]  = { "No WiFi", "Facts arrive once the Orb is back online." };
    static const char *const unreach[]  = { "Relay not answering", "orb-ponderer did not reply. Trying again shortly." };
    static const char *const empty[]    = { "No facts yet", "orb-ponderer is up but has no facts to give." };
    static const char *const relayErr[] = { "Relay error", "orb-ponderer answered but gave no fact. Trying again shortly." };
    switch (st) {
        case facts::ST_UNCONFIGURED: return unconf;
        case facts::ST_OFFLINE:      return offline;
        case facts::ST_UNREACHABLE:  return unreach;
        case facts::ST_EMPTY:        return empty;
        case facts::ST_RELAY_ERROR:  return relayErr;
        default:                     return waiting;
    }
}

// Why there is nothing to show. WiFi is asked here as well as on the net side because the
// device's network task only runs the relay modules while connected, so it never gets the
// chance to report that it is not.
facts::Status empty_reason() {
    if (!ponderer::configured()) return facts::ST_UNCONFIGURED;
    const facts::Status st = facts::status();
#ifdef ARDUINO
    if (st != facts::ST_UNCONFIGURED && WiFi.status() != WL_CONNECTED) return facts::ST_OFFLINE;
#endif
    return st == facts::ST_OFFLINE ? facts::ST_WAITING : st;   // WiFi is back: asking again
}

void draw_empty() {
    const char *const *w = empty_words(empty_reason());
    if (strcmp(lv_label_get_text(s_emptyTitle), w[0])) lv_label_set_text(s_emptyTitle, w[0]);
    if (strcmp(lv_label_get_text(s_emptyText), w[1])) lv_label_set_text(s_emptyText, w[1]);
}

// Everything on screen from s_cur/s_have and the client's status.
void redraw() {
    show(s_body, s_have);
    if (s_bodyCanvas && !s_have) show(s_bodyCanvas, false);
    show(s_emptyTitle, !s_have);
    show(s_emptyText, !s_have);
    if (s_have) draw_fact();
    else draw_empty();
    draw_arcs();
}

// ---- dress ---------------------------------------------------------------------------

constexpr char ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.,-";

// A theme face for `role` near `px` that has the whole alphabet, or Montserrat at px.
const lv_font_t *face(orb_style::Role role, int px) {
    const lv_font_t *f = orb_style::font(role, px);
    return covers(f, ALPHA) ? f : font_ladder(px);
}

// Colours and faces from the look. Cheap, and the look only changes across a restart, so
// this runs in init() and again each time the screen is entered.
void restyle() {
    const orb_style::Look &l = orb_style::look("facts");
    s_pal = palette_from(l);
    using namespace orb_style;

    lv_obj_set_style_bg_color(s_scr, lv_color_hex(s_pal.bg), 0);
    lv_obj_set_style_text_color(s_body, lv_color_hex(s_pal.text), 0);

    s_topicFont = face(SMALL, 16);
    if (s_topicFont == font_ladder(16)) s_topicFont = face(BODY, 16);   // a caption face too small for it
    s_sourceFont = face(SMALL, 12);

    // The theme's body face, at whatever one size it was baked: font() gives it up for the
    // rung of the ladder nearest its size, so ask at each until it does.
    s_bodyFace = nullptr;
    static const int rungs[] = { 48, 44, 40, 36, 32, 28, 26, 24, 22, 20, 18, 16, 14, 12 };
    for (int px : rungs) {
        if (!themed_font(BODY, px)) continue;
        const lv_font_t *f = font(BODY, px);
        if (covers(f, ALPHA)) s_bodyFace = f;
        break;
    }

    // The empty state: its heading in the theme's title face if that is near enough in size,
    // else its body face, so the two lines are never in different families by accident.
    const lv_font_t *text = face(BODY, 18);
    const lv_font_t *title = face(TITLE, 24);
    if (title == font_ladder(24)) title = face(BODY, 24);
    if (title == font_ladder(24) && text != font_ladder(18)) title = text;
    lv_obj_set_style_text_font(s_emptyTitle, title, 0);
    lv_obj_set_style_text_color(s_emptyTitle, lv_color_hex(s_pal.text), 0);
    lv_obj_set_style_text_font(s_emptyText, text, 0);
    lv_obj_set_style_text_color(s_emptyText, lv_color_hex(s_pal.dim), 0);
    // The heading's centre line stays where a 24 px Montserrat heading had it, and the
    // text hangs from where it always hung.
    lv_obj_align(s_emptyTitle, LV_ALIGN_CENTER, 0, -34);
    lv_obj_align(s_emptyText, LV_ALIGN_TOP_MID, 0, MID - 8);
}

// ---- transitions ---------------------------------------------------------------------

void set_opa(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

void fade(int32_t to, uint32_t ms, lv_anim_ready_cb_t done) {
    lv_anim_del(s_card, set_opa);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_card);
    lv_anim_set_exec_cb(&a, set_opa);
    lv_anim_set_values(&a, lv_obj_get_style_opa(s_card, 0), to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, to ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    if (done) lv_anim_set_ready_cb(&a, done);
    lv_anim_start(&a);
}

void faded_out(lv_anim_t *) {
    s_fading = false;
    s_cur = s_pending;
    s_have = true;
    redraw();
    fade(LV_OPA_COVER, FADE_IN_MS, nullptr);
}

// Put `f` up: straight away when nothing was showing, through a quick fade otherwise.
void present(const facts::Fact &f, bool animate) {
    s_wantNext = false;
    if (s_giveUp) lv_timer_pause(s_giveUp);
    if (!animate || !s_have) {
        lv_anim_del(s_card, set_opa);
        s_fading = false;
        s_cur = f;
        s_have = true;
        redraw();
        if (animate) { lv_obj_set_style_opa(s_card, LV_OPA_TRANSP, 0); fade(LV_OPA_COVER, FADE_IN_MS, nullptr); }
        else lv_obj_set_style_opa(s_card, LV_OPA_COVER, 0);
        return;
    }
    s_pending = f;
    s_fading = true;
    fade(LV_OPA_TRANSP, FADE_OUT_MS, faded_out);
}

// Next fact now if one is in hand; otherwise the next one to arrive replaces this at once.
bool advance(bool pressed) {
    facts::Fact f;
    if (facts::take(f)) { present(f, true); return true; }
    s_wantNext = true;
    if (pressed && s_have && !s_fading) {
        // Say the press was heard: the fact dims while the next one is on its way.
        fade(LV_OPA_50, FADE_OUT_MS, nullptr);
        if (s_giveUp) { lv_timer_reset(s_giveUp); lv_timer_resume(s_giveUp); }
    }
    return false;
}

void on_cycle(lv_timer_t *) { if (s_showing) advance(false); }

void on_give_up(lv_timer_t *t) {
    lv_timer_pause(t);
    if (!s_fading) fade(LV_OPA_COVER, FADE_IN_MS, nullptr);
}

void on_recheck(lv_timer_t *) { if (s_showing && !s_have) draw_empty(); }

// UI side of facts_client: a fact arrived or the status changed.
void on_news() {
    if (!s_scr) return;
    if (!s_showing) return;   // hidden: the prefetched fact waits, and onEnter picks it up
    if (!s_have || s_wantNext) {
        facts::Fact f;
        if (facts::take(f)) {
            const bool animate = s_have;
            present(f, animate);
            if (!animate) lv_timer_reset(s_cycle);
            return;
        }
    }
    if (!s_have) redraw();   // the empty state names the new reason
}

}  // namespace

namespace factsview {

void init() {
    if (s_scr) return;
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_card = blank(s_scr);
    lv_obj_set_size(s_card, SCREEN, SCREEN);

    // Colours and faces are restyle()'s; this only makes the objects.
    s_body = label(s_card, &lv_font_montserrat_28, 0);

    // The empty state sits where the fact would, inside the same circle.
    s_emptyTitle = label(s_card, &lv_font_montserrat_24, 0);
    lv_obj_set_width(s_emptyTitle, 300);
    s_emptyText = label(s_card, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(s_emptyText, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_emptyText, 290);
    lv_obj_set_style_text_line_space(s_emptyText, 3, 0);
    restyle();

    arc_build(s_topic, false);
    arc_build(s_source, true);

    s_cycle = lv_timer_create(on_cycle, FACT_MS, nullptr);
    lv_timer_pause(s_cycle);
    s_giveUp = lv_timer_create(on_give_up, PRESS_WAIT_MS, nullptr);
    lv_timer_pause(s_giveUp);
    s_recheck = lv_timer_create(on_recheck, 1000, nullptr);
    lv_timer_pause(s_recheck);

    redraw();
    facts::start(on_news);
}

lv_obj_t *screen() { return s_scr; }

void onEnter() {
    s_showing = true;
    facts::setShowing(true);
    restyle();
    arc_attach(s_topic);
    arc_attach(s_source);
    body_canvas_take();
    lv_anim_del(s_card, set_opa);
    lv_obj_set_style_opa(s_card, LV_OPA_COVER, 0);
    s_fading = false;
    s_wantNext = false;
    // A fresh fact every time the screen is looked at, if one is in hand.
    facts::Fact f;
    if (facts::take(f)) { s_cur = f; s_have = true; }
    // After the canvases, so the plate goes behind them and the glass and sparks in front;
    // before anything is drawn, so the arcs can be coloured against the plate.
    s_backdrop = orb_style::attach(s_scr, "facts");
    sample_backdrop();
    redraw();
    lv_timer_reset(s_cycle);
    lv_timer_resume(s_cycle);
    lv_timer_resume(s_recheck);
}

void onExit() {
    s_showing = false;
    facts::setShowing(false);
    lv_timer_pause(s_cycle);
    lv_timer_pause(s_giveUp);
    lv_timer_pause(s_recheck);
    lv_anim_del(s_card, set_opa);
    if (s_fading) { s_cur = s_pending; s_fading = false; }
    lv_obj_set_style_opa(s_card, LV_OPA_COVER, 0);
    orb_style::release(s_backdrop);
    s_sampled = false;
    arc_release(s_topic);
    arc_release(s_source);
    body_canvas_give();
    show(s_body, s_have);
}

void onPress() {
    if (!s_showing) return;
    lv_timer_reset(s_cycle);
    if (s_fading) return;   // already on its way
    advance(true);
}

// A turn either way is the same as a press: next fact. A spin delivers a detent every few
// milliseconds, so turns are limited to one change per TURN_GAP_MS and the rest are dropped
// rather than queued; otherwise a flick of the knob would burn through a dozen facts (and a
// dozen relay requests) in a second.
void onTurn(int delta) {
    static uint32_t last = 0;
    static bool any = false;
    if (!delta || !s_showing) return;
    if (any && lv_tick_elaps(last) < TURN_GAP_MS) return;
    any = true;
    last = lv_tick_get();
    onPress();
}

}  // namespace factsview

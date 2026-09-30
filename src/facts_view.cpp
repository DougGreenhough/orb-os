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
#include "facts_view.h"
#include "facts_client.h"
#include "facts_layout.h"
#include "curved_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace {

constexpr int SCREEN = 466;
constexpr int MID = SCREEN / 2;

constexpr uint32_t FACT_MS    = 45000;   // how long a fact stays up
constexpr uint32_t FADE_OUT_MS = 180;
constexpr uint32_t FADE_IN_MS  = 320;
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

constexpr uint32_t COL_TEXT   = 0xF4F2EC;   // a warm white: less glare than 0xFFFFFF on AMOLED
constexpr uint32_t COL_DIM    = 0x8A8F98;
constexpr uint32_t COL_SOURCE = 0x5E636C;
// Topic colours. Chosen by the topic's name, so "Planets" is always the same colour and a
// change of topic is visible before a word is read.
constexpr uint32_t TOPIC_COLS[] = { 0xF2B45A, 0x6FD3C7, 0xA99BFF, 0x8FD18B, 0xFF8A7A, 0x7FB8FF };

struct Arc {
    lv_obj_t *canvas = nullptr;
    uint8_t  *buf = nullptr;
    lv_obj_t *fallback = nullptr;   // straight label, used only if the canvas could not be had
    bool      bottom = false;
};

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_card = nullptr;       // everything that fades
lv_obj_t *s_body = nullptr;       // the fact
lv_obj_t *s_emptyTitle = nullptr;
lv_obj_t *s_emptyText = nullptr;
Arc s_topic, s_source;
lv_timer_t *s_cycle = nullptr;    // FACT_MS between facts
lv_timer_t *s_giveUp = nullptr;   // undoes a press's dimming if nothing arrives

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
    return TOPIC_COLS[h % (sizeof(TOPIC_COLS) / sizeof(TOPIC_COLS[0]))];
}

// ---- the arcs ------------------------------------------------------------------------

void arc_attach(Arc &a) {
    if (a.buf) return;
    const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(ARC_W, ARC_H);
#if defined(ESP_PLATFORM)
    a.buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
    a.buf = (uint8_t *)malloc(sz);
#endif
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
    if (a.buf) {
#if defined(ESP_PLATFORM)
        heap_caps_free(a.buf);
#else
        free(a.buf);
#endif
        a.buf = nullptr;
    }
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
        curved_text::draw_arc(dst, font, one, cx, cy, ARC_R, deg, lv_color_hex(col), 0,
                              lv_color_black());
    }
    lv_obj_invalidate(a.canvas);
}

void arc_build(Arc &a, bool bottom) {
    a.bottom = bottom;
    a.fallback = label(s_card, &lv_font_montserrat_14, COL_DIM);
    lv_obj_align(a.fallback, bottom ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_TOP_MID, 0, bottom ? -34 : 34);
    lv_obj_add_flag(a.fallback, LV_OBJ_FLAG_HIDDEN);
}

// ---- drawing -------------------------------------------------------------------------

void upper_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; ++i) dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = 0;
}

void draw_arcs() {
    char buf[40];
    if (s_have) {
        upper_copy(buf, sizeof(buf), s_cur.topic[0] ? s_cur.topic : "Fact");
        arc_draw(s_topic, buf, &lv_font_montserrat_16, topic_colour(s_cur.topic), TOPIC_TRACK);
        arc_draw(s_source, s_cur.source, &lv_font_montserrat_12, COL_SOURCE, SOURCE_TRACK);
    } else {
        arc_draw(s_topic, "FACTS", &lv_font_montserrat_16, COL_SOURCE, TOPIC_TRACK);
        arc_draw(s_source, "", &lv_font_montserrat_12, COL_SOURCE, SOURCE_TRACK);
    }
}

void draw_fact() {
    static facts_layout::Result r;   // 300+ bytes; not on the LVGL task's stack
    if (!facts_layout::fit(s_cur.text, BODY_R, r)) return;
    lv_obj_set_style_text_font(s_body, r.font, 0);
    lv_obj_set_style_text_line_space(s_body, r.lineSpace, 0);
    lv_label_set_text(s_body, r.text);
    lv_obj_center(s_body);
    printf("[facts] %d px, %d lines, widest %d: %s\n", r.size, r.lines, r.widest, s_cur.id);
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

void show(lv_obj_t *o, bool on) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Everything on screen from s_cur/s_have and the client's status.
void redraw() {
    show(s_body, s_have);
    show(s_emptyTitle, !s_have);
    show(s_emptyText, !s_have);
    if (s_have) {
        draw_fact();
    } else {
        const char *const *w = empty_words(facts::status());
        lv_label_set_text(s_emptyTitle, w[0]);
        lv_label_set_text(s_emptyText, w[1]);
    }
    draw_arcs();
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

    s_body = label(s_card, &lv_font_montserrat_28, COL_TEXT);

    // The empty state sits where the fact would, inside the same circle.
    s_emptyTitle = label(s_card, &lv_font_montserrat_24, COL_TEXT);
    lv_obj_set_width(s_emptyTitle, 300);
    lv_obj_align(s_emptyTitle, LV_ALIGN_CENTER, 0, -34);
    s_emptyText = label(s_card, &lv_font_montserrat_18, COL_DIM);
    lv_label_set_long_mode(s_emptyText, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_emptyText, 290);
    lv_obj_set_style_text_line_space(s_emptyText, 3, 0);
    lv_obj_align(s_emptyText, LV_ALIGN_TOP_MID, 0, MID - 8);

    arc_build(s_topic, false);
    arc_build(s_source, true);

    s_cycle = lv_timer_create(on_cycle, FACT_MS, nullptr);
    lv_timer_pause(s_cycle);
    s_giveUp = lv_timer_create(on_give_up, PRESS_WAIT_MS, nullptr);
    lv_timer_pause(s_giveUp);

    redraw();
    facts::start(on_news);
}

lv_obj_t *screen() { return s_scr; }

void onEnter() {
    s_showing = true;
    facts::setShowing(true);
    arc_attach(s_topic);
    arc_attach(s_source);
    lv_anim_del(s_card, set_opa);
    lv_obj_set_style_opa(s_card, LV_OPA_COVER, 0);
    s_fading = false;
    s_wantNext = false;
    // A fresh fact every time the screen is looked at, if one is in hand.
    facts::Fact f;
    if (facts::take(f)) { s_cur = f; s_have = true; }
    redraw();
    lv_timer_reset(s_cycle);
    lv_timer_resume(s_cycle);
}

void onExit() {
    s_showing = false;
    facts::setShowing(false);
    lv_timer_pause(s_cycle);
    lv_timer_pause(s_giveUp);
    lv_anim_del(s_card, set_opa);
    if (s_fading) { s_cur = s_pending; s_fading = false; }
    lv_obj_set_style_opa(s_card, LV_OPA_COVER, 0);
    arc_release(s_topic);
    arc_release(s_source);
}

void onPress() {
    if (!s_showing) return;
    lv_timer_reset(s_cycle);
    if (s_fading) return;   // already on its way
    advance(true);
}

void onTurn(int) {}

}  // namespace factsview

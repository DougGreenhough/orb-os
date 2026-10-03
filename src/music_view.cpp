// Music: Spotify now-playing and control, through orb-ponderer. See music_view.h; the
// network half is music_client.cpp.
//
// Layout, against the round glass rather than the square buffer:
//
//   - the ring IS the progress bar: a thin track just inside the bezel, filled clockwise
//     from twelve o'clock, in the theme's accent while playing and its dim while paused;
//   - the playing device's name, small and spaced, where the chord is still 330 px wide;
//   - the cover, 200 px, whose corners sit 172 px from the centre and so well clear of the
//     ring;
//   - title (scrolls when long), artist (truncates), and a line with the play state and
//     the time.
//
// Knob. Every detent reaches this screen (the app switcher is the rock gesture, not a turn),
// so the turn is spent on what a music knob is for: at rest, turning sets the volume, 5% a
// detent, shown on a dial over the cover that fades 1.5 s after the knob stops, and sent as
// one `vol` command once it has. A press puts three controls on the cover (previous,
// play/pause, next) with play/pause highlighted; turning then moves the highlight, a press
// does it, and six seconds without a touch puts them away. The line under the artist names
// whatever is highlighted or being changed, so the mode is never a guess.
//
// Dress. Everything is coloured from orb_style::look("music") on every enter (restyle()),
// and the theme's backdrop is attached while showing. Ink-on-paper themes get a paper scrim
// and ink controls over the cover; one-colour themes get the cover re-drawn in their colour
// when it arrives; themes that glow get a halo on the cover's frame, the chosen control and
// the head of the ring. With no theme the screen is exactly as it was first drawn, Spotify
// green included (PLAIN below), since the plain look's accent is another screen's amber.
//
// Memory: one cover, ART_PX^2 RGB565 (80 KB), in PSRAM, owned by this file from the moment
// music::takeArt() hands it over until onExit() or the next cover releases it.
#include "music_view.h"
#include "music_client.h"
#include "ponderer.h"
#include "orb_style.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr int SCREEN = 466;
constexpr int MID    = SCREEN / 2;

constexpr int RING_D   = 452;   // outer diameter of the progress ring
constexpr int RING_W   = 6;
constexpr int ART      = music::ART_PX;
constexpr int ART_Y    = 98;
constexpr int DEVICE_Y = 66;
constexpr int TITLE_Y  = 312;
constexpr int TITLE_W  = 340;         // 24 px title; the chord inside the ring is ~390 px here
constexpr int TITLE_W_SMALL = 372;   // the 20 px step-down may use a little more of it
constexpr int ARTIST_Y = 346;
constexpr int LINE_Y   = 378;

// Every colour the screen uses, worked out from the theme's look in restyle(). The values
// here are the unthemed screen's, and what it is drawn with before the first enter.
struct Pal {
    uint32_t text     = 0xF2F2F2;
    uint32_t dim      = 0x8A8F98;
    uint32_t faint    = 0x454A53;   // the empty states' icon, the cover's placeholder
    uint32_t track    = 0x1B1E23;   // the ring's unfilled part
    uint32_t artBg    = 0x15171B;   // behind the cover
    uint32_t live     = 0x1ED760;   // playing. Unthemed: Spotify's green
    uint32_t paused   = 0x6B7280;
    uint32_t warn     = 0xFFB23F;   // a command bounced
    uint32_t volTrack = 0x3A3F47;
    uint32_t scrim    = 0x000000;   // laid over the cover under the controls and the dial
    uint32_t sel      = 0xF2F2F2;   // the highlighted control's fill...
    uint32_t onSel    = 0x000000;   // ...and the icon on it
    uint32_t frame    = 0x000000;   // the cover's border and halo
    lv_opa_t scrimCtl = 165, scrimVol = 215, ctlRim = LV_OPA_40;
    int      glow     = 0;
    bool     frameOn  = false;
    bool     mono     = false;
    uint32_t monoLo = 0, monoHi = 0;   // a one-colour cover runs between these
};
const Pal PLAIN;
Pal s_pal;

constexpr uint32_t TICK_MS     = 250;
constexpr uint32_t IDLE_MS     = 6000;   // the knob goes back after this much stillness
constexpr uint32_t VOL_SEND_MS = 350;    // a run of detents becomes one command
constexpr uint32_t VOL_HOLD_MS = 2500;   // trust our own volume over polls this long after
constexpr uint32_t TOAST_MS    = 2500;
constexpr int      VOL_STEP    = 5;

enum Mode { M_REST, M_CONTROL };
enum Ctl { C_PREV, C_TOGGLE, C_NEXT, C_COUNT };
constexpr int CTL_D     = 52;
constexpr int CTL_PITCH = 62;
constexpr uint32_t VOL_SHOW_MS = 1500;   // the volume dial stays this long after the last detent
constexpr uint32_t VOL_FADE_MS = 300;

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_ring = nullptr;
lv_obj_t *s_aura = nullptr;     // a wider, faint copy of the ring's filled part: its glow
lv_obj_t *s_head = nullptr;     // the bright point where the ring has got to
lv_obj_t *s_lineSym = nullptr;  // the play/pause mark, when the line wears a theme face
orb_style::Backdrop *s_backdrop = nullptr;
const lv_font_t *s_fLine = &lv_font_montserrat_16;
bool s_lineSplit = false;
lv_obj_t *s_body = nullptr;
lv_obj_t *s_device = nullptr;
lv_obj_t *s_artBox = nullptr;
lv_obj_t *s_artImg = nullptr;
lv_obj_t *s_artPh = nullptr;
lv_obj_t *s_scrim = nullptr;
lv_obj_t *s_ctl[C_COUNT] = {};
lv_obj_t *s_ctlIcon[C_COUNT] = {};
lv_obj_t *s_volBox = nullptr;   // scrim + dial + number, faded as one
lv_obj_t *s_volArc = nullptr;
lv_obj_t *s_volNum = nullptr;
lv_obj_t *s_volIcon = nullptr;
lv_obj_t *s_title = nullptr;
lv_obj_t *s_artist = nullptr;
lv_obj_t *s_line = nullptr;
lv_obj_t *s_empty = nullptr;
lv_obj_t *s_emIcon = nullptr;
lv_obj_t *s_emHead = nullptr;
lv_obj_t *s_emBody = nullptr;
lv_obj_t *s_emFoot = nullptr;
lv_timer_t *s_timer = nullptr;

music::Now s_now;            // the UI's copy; static, ~400 bytes
uint32_t   s_rxTick = 0;     // lv_tick_get() when s_now.progressMs was true
bool       s_entered = false;
Mode       s_mode = M_REST;
int        s_sel = C_TOGGLE;
uint32_t   s_lastInput = 0;
uint32_t   s_localSeq = 0;   // the newest command queued from here
int        s_volTarget = 0;
bool       s_volDirty = false;
uint32_t   s_volDirtyAt = 0;
uint32_t   s_volHoldUntil = 0;
bool       s_volShown = false;
bool       s_volFading = false;
uint32_t   s_volUntil = 0;
const char *s_toast = nullptr;
uint32_t   s_toastUntil = 0;
char       s_lastLine[96] = "";
int        s_lastRing = -1;

uint8_t     *s_artBody = nullptr;   // the ORB5 allocation s_artDsc points into
lv_img_dsc_t s_artDsc;
char         s_artId[sizeof(music::Now::artId)] = "";

uint32_t now() { return lv_tick_get(); }
bool before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

// ---- dress ---------------------------------------------------------------------------

uint32_t mix(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int sh) { return (uint32_t)lroundf(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t) << sh; };
    return ch(16) | ch(8) | ch(0);
}

const lv_font_t *montserrat(int px) {
    switch (px) {
        case 12: return &lv_font_montserrat_12; case 14: return &lv_font_montserrat_14;
        case 16: return &lv_font_montserrat_16; case 18: return &lv_font_montserrat_18;
        case 20: return &lv_font_montserrat_20; case 24: return &lv_font_montserrat_24;
        case 40: return &lv_font_montserrat_40; case 48: return &lv_font_montserrat_48;
    }
    return &lv_font_montserrat_16;
}

// A theme's face is made for its own screens and may carry only the glyphs those use; the
// fonts draw nothing at all for a letter they lack. So a face is only worn for words it
// can spell.
bool covers(const lv_font_t *f, const char *t) {
    lv_font_glyph_dsc_t g;
    for (uint32_t i = 0; t[i];) {
        const uint32_t c = _lv_txt_encoded_next(t, &i);
        if (c == '\n' || c == '\r') continue;
        if (!lv_font_get_glyph_dsc(f, &g, c, 0)) return false;
    }
    return true;
}

// The theme's face for these words at about this size, else Montserrat at exactly it.
const lv_font_t *face(orb_style::Role role, int px, const char *words) {
    if (orb_style::themed_font(role, px)) {
        const lv_font_t *f = orb_style::font(role, px);
        if (!words || covers(f, words)) return f;
    }
    return montserrat(px);
}

// Layouts were drawn against Montserrat's line heights; another face is centred on the
// line it replaces rather than hung from its top.
int y_for(int y, int px, const lv_font_t *f) {
    return y + (montserrat(px)->line_height - f->line_height) / 2;
}

void wear(lv_obj_t *l, orb_style::Role role, int px, const char *words, int y) {
    const lv_font_t *f = face(role, px, words);
    if (lv_obj_get_style_text_font(l, 0) != f) lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_y(l, y_for(y, px, f));
}

void glow(lv_obj_t *o, uint32_t color, int width, lv_opa_t opa, int spread = 0) {
    lv_obj_set_style_shadow_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(o, width, 0);
    lv_obj_set_style_shadow_spread(o, spread, 0);
    lv_obj_set_style_shadow_opa(o, width ? opa : LV_OPA_TRANSP, 0);
}

Pal palette(const orb_style::Look &l) {
    const orb_style::Look none;
    const bool plain = l.text == none.text && l.dim == none.dim && l.accent == none.accent &&
                       l.accent2 == none.accent2 && l.rule == none.rule && l.bg == none.bg &&
                       l.dark && !l.mono && !l.glow && !l.sparks && !l.plate[0] && !l.overlay[0];
    if (plain) return PLAIN;
    Pal p;
    p.text     = l.text;
    p.dim      = l.dim;
    p.faint    = mix(l.dim, l.bg, 0.4f);
    p.track    = l.rule;
    p.artBg    = mix(l.rule, l.bg, 0.5f);
    p.live     = l.accent;
    p.paused   = l.dim;
    p.warn     = l.accent;
    p.scrim    = l.bg;
    p.volTrack = l.dark ? mix(l.rule, l.text, 0.18f) : mix(l.rule, l.text, 0.1f);
    p.sel      = l.accent;
    p.onSel    = l.bg;
    p.frame    = l.accent2;
    p.frameOn  = true;
    p.glow     = l.glow;
    p.mono     = l.mono;
    p.monoLo   = l.bg;
    p.monoHi   = l.text;
    // Ink on paper: the cover is veiled with paper, heavily enough that ink reads on it
    // whatever the cover is.
    if (!l.dark) { p.scrimCtl = 215; p.scrimVol = 232; p.ctlRim = LV_OPA_70; }
    return p;
}

// ---- builders ------------------------------------------------------------------------

lv_obj_t *blank(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int cx, int y, int w) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, cx - w / 2, y);
    return l;
}

void show(lv_obj_t *o, bool on) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// A label's text is copied and the whole label invalidated on every set, so only set it
// when it actually changed; the ring and the time line are refreshed four times a second.
void set_text(lv_obj_t *l, const char *t) {
    if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

void fmt_time(char *out, size_t cap, int32_t ms) {
    if (ms < 0) ms = 0;
    const int s = ms / 1000;
    if (s >= 3600) snprintf(out, cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else           snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}

// ---- art -----------------------------------------------------------------------------

void drop_art() {
    if (!s_artBody) return;
    lv_img_set_src(s_artImg, nullptr);
    lv_img_cache_invalidate_src(&s_artDsc);   // the cache holds a pointer to these pixels
    ponderer::release(s_artBody);
    s_artBody = nullptr;
    s_artId[0] = 0;
}

// A one-colour instrument cannot show a colour photograph. Redraw the cover in the theme's
// own colour: each pixel's brightness picks a point between the ground and the text colour.
// Once per cover, in place (the allocation is ours from takeArt() on).
void tint_mono(uint16_t *px, int n) {
    uint16_t ramp[64];
    for (int i = 0; i < 64; ++i) {
        const uint32_t c = mix(s_pal.monoLo, s_pal.monoHi, i / 63.0f);
        ramp[i] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
    }
    for (int i = 0; i < n; ++i) {
        const uint16_t v = px[i];
        // 5-6-5 to a 6-bit luminance: (2R + 5G' + B) / 8 with G' already 6 bits wide.
        const int y = (((v >> 11) << 1) * 2 + ((v >> 5) & 63) * 5 + ((v & 31) << 1)) >> 3;
        px[i] = ramp[y > 63 ? 63 : y];
    }
}

void take_art() {
    uint8_t *body = nullptr; const uint16_t *px = nullptr; int w = 0, h = 0;
    char id[sizeof(s_artId)];
    if (!music::takeArt(&body, &px, &w, &h, id, sizeof(id))) return;
    drop_art();
    s_artBody = body;
    memcpy(s_artId, id, sizeof(s_artId));
    if (s_pal.mono) tint_mono((uint16_t *)px, w * h);
    memset(&s_artDsc, 0, sizeof(s_artDsc));
    s_artDsc.header.always_zero = 0;
    s_artDsc.header.w = w;
    s_artDsc.header.h = h;
    s_artDsc.header.cf = LV_IMG_CF_TRUE_COLOR;   // RGB565, no swap: ORB5 is already that
    s_artDsc.data_size = (uint32_t)w * h * 2;
    s_artDsc.data = (const uint8_t *)px;
    lv_img_set_src(s_artImg, &s_artDsc);
    lv_obj_align(s_artImg, LV_ALIGN_CENTER, 0, 0);
}

// ---- state ---------------------------------------------------------------------------

bool active() { return ponderer::configured() && s_now.status == music::ST_ACTIVE; }

int32_t progress_now() {
    int32_t p = s_now.progressMs;
    if (s_now.playing) p += (int32_t)(now() - s_rxTick);
    if (s_now.durationMs > 0 && p > s_now.durationMs) p = s_now.durationMs;
    return p < 0 ? 0 : p;
}

// Rebase the interpolation on "now" before changing playing, so the bar neither jumps
// nor keeps running after a pause.
void rebase() {
    s_now.progressMs = progress_now();
    s_rxTick = now();
}


void toast(const char *t) {
    s_toast = t;
    s_toastUntil = now() + TOAST_MS;
}

void flush_volume() {
    if (!s_volDirty) return;
    s_volDirty = false;
    s_localSeq = music::queue(music::CMD_VOL, s_volTarget);
    s_volHoldUntil = now() + VOL_HOLD_MS;
}

void set_mode(Mode m) {
    if (m != M_REST && s_mode == M_REST) s_sel = C_TOGGLE;
    s_mode = m;
    s_lastInput = now();
}

// ---- the volume dial -------------------------------------------------------------------

void vol_opa_cb(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
void vol_faded_cb(lv_anim_t *) {
    lv_obj_add_flag(s_volBox, LV_OBJ_FLAG_HIDDEN);
    s_volShown = s_volFading = false;
}

void vol_hide_now() {
    lv_anim_del(s_volBox, vol_opa_cb);
    lv_obj_add_flag(s_volBox, LV_OBJ_FLAG_HIDDEN);
    s_volShown = s_volFading = false;
}

void vol_show() {
    lv_anim_del(s_volBox, vol_opa_cb);
    lv_obj_set_style_opa(s_volBox, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_volBox, LV_OBJ_FLAG_HIDDEN);
    s_volShown = true;
    s_volFading = false;
    s_volUntil = now() + VOL_SHOW_MS;
}

void vol_fade() {
    s_volFading = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_volBox);
    lv_anim_set_exec_cb(&a, vol_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, VOL_FADE_MS);
    lv_anim_set_ready_cb(&a, vol_faded_cb);
    lv_anim_start(&a);
}

bool vol_visible() { return s_volShown && !s_volFading; }

// ---- drawing -------------------------------------------------------------------------

void render_ring() {
    int v = 0;
    if (active() && s_now.durationMs > 0)
        v = (int)((int64_t)progress_now() * 1000 / s_now.durationMs);
    const bool on = active();
    const uint32_t col = s_now.playing ? s_pal.live : s_pal.paused;
    if (v != s_lastRing) {
        lv_arc_set_value(s_ring, v);
        lv_arc_set_value(s_aura, v);
        s_lastRing = v;
        if (s_pal.glow) {
            // The head rides the middle of the ring's width.
            const float a = v * (6.2831853f / 1000.0f), r = (RING_D - RING_W) / 2.0f;
            const int d = lv_obj_get_width(s_head);
            lv_obj_set_pos(s_head, (int)lroundf(MID + sinf(a) * r) - d / 2, (int)lroundf(MID - cosf(a) * r) - d / 2);
        }
    }
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(col), LV_PART_INDICATOR);
    // Only what is live glows: paused, the ring is just a dim line.
    show(s_aura, on && s_pal.glow >= 2 && s_now.playing && v > 0);
    show(s_head, on && s_pal.glow && s_now.playing);
}

// The line under the artist: time and state at rest, the highlighted control's name while
// the knob is taken, a short failure notice when a command bounced.
void render_line() {
    char buf[96], a[16], b[16];
    char words[48] = "";            // the same line without its mark, for a theme's face
    const char *sym = nullptr;
    uint32_t symCol = 0;
    uint32_t col = s_pal.dim;
    if (s_toast && before(now(), s_toastUntil)) {
        snprintf(buf, sizeof(buf), "%s", s_toast);
        col = s_pal.warn;
    } else if (s_mode == M_CONTROL) {
        static const char *names[C_COUNT] = { "Previous", "", "Next" };
        if (s_sel == C_TOGGLE) snprintf(buf, sizeof(buf), "%s", s_now.playing ? "Pause" : "Play");
        else snprintf(buf, sizeof(buf), "%s", names[s_sel]);
        col = s_pal.text;
    } else if (vol_visible()) {
        if (s_now.canVolume) {
            snprintf(buf, sizeof(buf), "Volume %d%%", s_now.volume < 0 ? 0 : s_now.volume);
            col = s_pal.text;
        } else {
            snprintf(buf, sizeof(buf), "Volume is fixed on this device");
        }
    } else {
        fmt_time(a, sizeof(a), progress_now());
        fmt_time(b, sizeof(b), s_now.durationMs);
        sym = s_now.playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE;
        symCol = s_now.playing ? s_pal.live : s_pal.text;
        if (s_now.durationMs > 0) {
            snprintf(buf, sizeof(buf), "#%06X %s#   %s / %s", (unsigned)symCol, sym, a, b);
            snprintf(words, sizeof(words), "%s / %s", a, b);
        } else {
            snprintf(buf, sizeof(buf), "#%06X %s#   %s", (unsigned)symCol, sym, s_now.playing ? "Playing" : "Paused");
            snprintf(words, sizeof(words), "%s", s_now.playing ? "Playing" : "Paused");
        }
    }
    if (strcmp(buf, s_lastLine) != 0) {
        snprintf(s_lastLine, sizeof(s_lastLine), "%s", buf);
        lv_obj_set_style_text_color(s_line, lv_color_hex(col), 0);
        // A theme's face has no play or pause mark in it, so under one the mark is its own
        // label in Montserrat and the pair is centred by measuring. Otherwise it is one
        // string, as it always was.
        const bool split = s_lineSplit && sym;
        show(s_lineSym, split);
        if (split) {
            const int gap = 10;
            const int wS = lv_txt_get_width(sym, strlen(sym), &lv_font_montserrat_16, 0, LV_TEXT_FLAG_NONE);
            const int wT = lv_txt_get_width(words, strlen(words), s_fLine, 0, LV_TEXT_FLAG_NONE);
            lv_obj_set_style_text_color(s_lineSym, lv_color_hex(symCol), 0);
            set_text(s_lineSym, sym);
            lv_obj_set_x(s_lineSym, MID - (wS + gap + wT) / 2);
            lv_obj_set_x(s_line, MID - 150 + (wS + gap) / 2);
            lv_label_set_text(s_line, words);
        } else {
            lv_obj_set_x(s_line, MID - 150);
            lv_label_set_text(s_line, buf);
        }
    }
}

void render_controls() {
    const bool ctl = s_mode == M_CONTROL;
    show(s_scrim, ctl);
    for (int i = 0; i < C_COUNT; ++i) {
        show(s_ctl[i], ctl);
        const bool on = ctl && i == s_sel;
        lv_obj_set_style_bg_color(s_ctl[i], lv_color_hex(on ? s_pal.sel : s_pal.scrim), 0);
        lv_obj_set_style_bg_opa(s_ctl[i], on ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_set_style_border_opa(s_ctl[i], on ? LV_OPA_TRANSP : s_pal.ctlRim, 0);
        lv_obj_set_style_text_color(s_ctlIcon[i], lv_color_hex(on ? s_pal.onSel : s_pal.text), 0);
        glow(s_ctl[i], s_pal.sel, on ? s_pal.glow * 7 : 0, LV_OPA_80);
    }
    set_text(s_ctlIcon[C_TOGGLE], s_now.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    if (s_volShown) {
        const bool known = s_now.volume >= 0;
        const int v = known ? s_now.volume : 0;
        lv_arc_set_value(s_volArc, v);
        lv_obj_set_style_arc_color(s_volArc, lv_color_hex(s_now.canVolume ? s_pal.live : s_pal.paused),
                                   LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_volNum, lv_color_hex(s_now.canVolume ? s_pal.text : s_pal.dim), 0);
        char buf[8];
        snprintf(buf, sizeof(buf), known ? "%d" : "--", v);
        set_text(s_volNum, buf);
        set_text(s_volIcon, !s_now.canVolume ? LV_SYMBOL_CLOSE : v == 0 ? LV_SYMBOL_MUTE
                            : v < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
    }
}

// The title steps down a size before it resorts to moving: most long titles fit at 20 px,
// and a line that sits still is easier to read from across a room than one that scrolls.
// Only what still does not fit scrolls, circularly and slowly.
void set_title(const char *t) {
    if (strcmp(lv_label_get_text(s_title), t) == 0) return;
    // The theme's headline face when it can spell this title, its text face failing that.
    auto pick = [&](int px) {
        const lv_font_t *f = face(orb_style::TITLE, px, t);
        return f == montserrat(px) ? face(orb_style::BODY, px, t) : f;
    };
    const lv_font_t *f = pick(24);
    int box = TITLE_W;
    lv_coord_t w = lv_txt_get_width(t, strlen(t), f, 0, LV_TEXT_FLAG_NONE);
    if (w > box) {
        const lv_font_t *small = pick(20);
        const lv_coord_t ws = lv_txt_get_width(t, strlen(t), small, 0, LV_TEXT_FLAG_NONE);
        // A theme's face comes in one size, so "smaller" may be the same face again; step
        // down only when it actually helps.
        if (ws < w) { f = small; w = ws; }
        box = TITLE_W_SMALL;
    }
    // A scrolling line is cut off hard at both ends, so it gets the narrower box: those
    // cut edges read as deliberate a little way in from the ring, and as a mistake against it.
    if (w > box) box = TITLE_W;
    lv_obj_set_style_text_font(s_title, f, 0);
    lv_label_set_long_mode(s_title, w > box ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_title, box);
    lv_obj_set_pos(s_title, MID - box / 2,
                   TITLE_Y + (lv_font_montserrat_24.line_height - f->line_height) / 2);
    lv_label_set_text(s_title, t);
}

void empty(const char *icon, const char *head, const char *body, const char *foot) {
    const lv_font_t *hf = face(orb_style::TITLE, 24, head);
    if (hf == montserrat(24)) hf = face(orb_style::BODY, 24, head);
    lv_obj_set_style_text_font(s_emHead, hf, 0);
    lv_obj_set_y(s_emHead, y_for(202, 24, hf));
    lv_obj_set_style_text_font(s_emBody, face(orb_style::BODY, 18, body), 0);
    wear(s_emFoot, orb_style::SMALL, 14, foot ? foot : "", 332);
    set_text(s_emIcon, icon);
    set_text(s_emHead, head);
    set_text(s_emBody, body);
    set_text(s_emFoot, foot ? foot : "");
}

// The relay's host, without the key or the path, for the "cannot reach" notice.
const char *relay_host() {
    static char host[64];
    const char *u = ponderer::base_url();
    const char *p = strstr(u, "://");
    p = p ? p + 3 : u;
    size_t n = 0;
    while (p[n] && p[n] != '/' && n + 1 < sizeof(host)) { host[n] = p[n]; ++n; }
    host[n] = 0;
    return host;
}

void render() {
    const bool ok = active();
    show(s_body, ok);
    show(s_empty, !ok);
    if (!ok) {
        if (!ponderer::configured())
            empty(LV_SYMBOL_SETTINGS, "Not set up",
                  "This Orb has no relay key.\nAdd it to src/ponderer_secrets.h\nand rebuild.", nullptr);
        else switch (s_now.status) {
        case music::ST_WAITING:
            empty(LV_SYMBOL_AUDIO, "Music", "Asking Spotify what is playing...", nullptr);
            break;
        case music::ST_UNREACHABLE:
            empty(LV_SYMBOL_WARNING, "Relay not answering",
                  "The relay gave no usable answer.\nTrying again every few seconds.", relay_host());
            break;
        case music::ST_UNLINKED:
            empty(LV_SYMBOL_AUDIO, "Spotify not linked",
                  "Connect it on the\norb-ponderer setup page.", nullptr);
            break;
        default:
            empty(LV_SYMBOL_AUDIO, "Nothing playing",
                  "Start something in Spotify on\nany device and it shows here.", nullptr);
            break;
        }
        render_ring();
        return;
    }

    set_title(s_now.title[0] ? s_now.title : "Untitled");
    wear(s_artist, orb_style::BODY, 18, s_now.artist, ARTIST_Y);
    lv_obj_set_height(s_artist, lv_obj_get_style_text_font(s_artist, 0)->line_height);
    set_text(s_artist, s_now.artist);
    char dev[64];
    snprintf(dev, sizeof(dev), "%s", s_now.device[0] ? s_now.device : "Spotify");
    for (char *c = dev; *c; ++c) if (*c >= 'a' && *c <= 'z') *c -= 32;
    wear(s_device, orb_style::SMALL, 14, dev, DEVICE_Y);
    lv_obj_set_height(s_device, lv_obj_get_style_text_font(s_device, 0)->line_height);
    set_text(s_device, dev);

    // The cover shows only while it is THIS track's: a new song keeps the placeholder until
    // its own art arrives rather than wearing the last one's.
    const bool haveArt = s_artBody && s_now.artId[0] && strcmp(s_artId, s_now.artId) == 0;
    show(s_artImg, haveArt);
    show(s_artPh, !haveArt);
    lv_obj_set_style_img_opa(s_artImg, s_now.playing ? LV_OPA_COVER : LV_OPA_60, 0);

    render_ring();
    render_controls();
    render_line();
}

// ---- the network's answers ------------------------------------------------------------

void ui_apply() {
    music::Now n;
    if (music::takeNow(n)) {
        // An answer to a poll that started before our newest command would undo the
        // optimistic change the knob just made; the poll after it is the one to believe.
        const bool stale = n.cmdSeq < s_localSeq && n.status == music::ST_ACTIVE &&
                           s_now.status == music::ST_ACTIVE;
        if (!stale) {
            if (s_volShown || s_volDirty || before(now(), s_volHoldUntil)) n.volume = s_now.volume;
            s_now = n;
            s_rxTick = now();
            if (s_now.status != music::ST_ACTIVE) {
                if (s_mode != M_REST) set_mode(M_REST);
                if (s_volShown) vol_hide_now();
            }
        }
    }
    take_art();
    if (music::takeCmdFailed()) toast("Spotify did not take that");
    if (s_entered) render();
}

void tick_cb(lv_timer_t *) {
    if (!s_entered) return;
    if (s_volDirty && !before(now(), s_volDirtyAt + VOL_SEND_MS)) flush_volume();
    if (s_volShown && !s_volFading && !before(now(), s_volUntil)) {
        vol_fade();
        s_lastLine[0] = 0;   // the line goes back to the time as the dial fades
    }
    if (s_mode != M_REST && !before(now(), s_lastInput + IDLE_MS)) {
        set_mode(M_REST);
        render();
        return;
    }
    if (!active()) return;
    render_ring();
    render_line();
}

// Put the theme's colours on everything. On every enter: the look is not known when
// init() builds the screen, and it costs a few dozen style writes.
void restyle() {
    const orb_style::Look &l = orb_style::look("music");
    s_pal = palette(l);
    const Pal &p = s_pal;
    auto text = [](lv_obj_t *o, uint32_t c) { lv_obj_set_style_text_color(o, lv_color_hex(c), 0); };

    lv_obj_set_style_arc_color(s_ring, lv_color_hex(p.track), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_aura, lv_color_hex(p.live), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_aura, (lv_opa_t)(p.glow * 24), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_head, lv_color_hex(p.live), 0);
    glow(s_head, p.live, p.glow * 6, LV_OPA_COVER, p.glow > 1 ? 2 : 1);

    text(s_device, p.dim);
    lv_obj_set_style_bg_color(s_artBox, lv_color_hex(p.artBg), 0);
    lv_obj_set_style_border_color(s_artBox, lv_color_hex(p.frame), 0);
    lv_obj_set_style_border_width(s_artBox, p.frameOn ? (p.glow >= 2 ? 2 : 1) : 0, 0);
    lv_obj_set_style_border_post(s_artBox, true, 0);
    glow(s_artBox, p.frame, p.glow * 12, (lv_opa_t)(75 + p.glow * 60), p.glow > 1 ? p.glow - 1 : 0);
    text(s_artPh, p.faint);
    lv_obj_set_style_bg_color(s_scrim, lv_color_hex(p.scrim), 0);
    lv_obj_set_style_bg_opa(s_scrim, p.scrimCtl, 0);
    for (int i = 0; i < C_COUNT; ++i) lv_obj_set_style_border_color(s_ctl[i], lv_color_hex(p.text), 0);
    lv_obj_set_style_bg_color(s_volBox, lv_color_hex(p.scrim), 0);
    lv_obj_set_style_bg_opa(s_volBox, p.scrimVol, 0);
    lv_obj_set_style_arc_color(s_volArc, lv_color_hex(p.volTrack), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_volNum, face(orb_style::TITLE, 40, "0123456789-"), 0);
    text(s_volIcon, p.dim);

    text(s_title, p.text);
    text(s_artist, p.dim);
    // The line is rewritten four times a second, so its face is settled once, on words that
    // cover everything it ever says.
    s_fLine = face(orb_style::BODY, 16, "0123456789:/% PreviousNextPauseayingd Volume is fixed on this device Spotify did not take that");
    s_lineSplit = s_fLine != montserrat(16);
    lv_obj_set_style_text_font(s_line, s_fLine, 0);
    lv_obj_set_y(s_line, y_for(LINE_Y, 16, s_fLine));
    show(s_lineSym, false);

    text(s_emIcon, p.faint);
    text(s_emHead, p.text);
    text(s_emBody, p.dim);
    text(s_emFoot, p.dim);
    lv_obj_invalidate(s_scr);
}

}  // namespace

namespace musicview {

void init() {
    if (s_scr) return;
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Under the ring, for themes that glow: its filled part again, wider and faint.
    s_aura = lv_arc_create(s_scr);
    lv_obj_remove_style_all(s_aura);
    lv_obj_clear_flag(s_aura, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_aura, RING_D + 10, RING_D + 10);
    lv_obj_center(s_aura);
    lv_arc_set_rotation(s_aura, 270);
    lv_arc_set_bg_angles(s_aura, 0, 360);
    lv_arc_set_range(s_aura, 0, 1000);
    lv_arc_set_value(s_aura, 0);
    lv_obj_set_style_arc_opa(s_aura, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_aura, RING_W + 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_aura, true, LV_PART_INDICATOR);
    lv_obj_add_flag(s_aura, LV_OBJ_FLAG_HIDDEN);

    // The ring: a full-circle track with the elapsed part laid over it from twelve o'clock.
    s_ring = lv_arc_create(s_scr);
    lv_obj_remove_style_all(s_ring);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_ring, RING_D, RING_D);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, 1000);
    lv_arc_set_value(s_ring, 0);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(s_pal.track), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(s_pal.live), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_ring, true, LV_PART_INDICATOR);

    s_head = blank(s_scr);
    lv_obj_set_size(s_head, RING_W, RING_W);
    lv_obj_set_style_radius(s_head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_head, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_head, LV_OBJ_FLAG_HIDDEN);

    s_body = blank(s_scr);
    lv_obj_set_size(s_body, SCREEN, SCREEN);

    s_device = label(s_body, &lv_font_montserrat_14, s_pal.dim, MID, DEVICE_Y, 260);
    lv_obj_set_style_text_letter_space(s_device, 2, 0);
    lv_label_set_long_mode(s_device, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_device, lv_font_montserrat_14.line_height);

    s_artBox = blank(s_body);
    lv_obj_set_size(s_artBox, ART, ART);
    lv_obj_set_pos(s_artBox, MID - ART / 2, ART_Y);
    lv_obj_set_style_radius(s_artBox, 16, 0);
    lv_obj_set_style_clip_corner(s_artBox, true, 0);
    lv_obj_set_style_bg_color(s_artBox, lv_color_hex(s_pal.artBg), 0);
    lv_obj_set_style_bg_opa(s_artBox, LV_OPA_COVER, 0);

    s_artPh = lv_label_create(s_artBox);
    lv_obj_set_style_text_font(s_artPh, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_artPh, lv_color_hex(s_pal.faint), 0);
    lv_label_set_text(s_artPh, LV_SYMBOL_AUDIO);
    lv_obj_center(s_artPh);

    s_artImg = lv_img_create(s_artBox);
    lv_obj_add_flag(s_artImg, LV_OBJ_FLAG_HIDDEN);

    s_scrim = blank(s_artBox);
    lv_obj_set_size(s_scrim, ART, ART);
    lv_obj_set_style_bg_color(s_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scrim, 165, 0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);

    static const char *icons[C_COUNT] = { LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT };
    for (int i = 0; i < C_COUNT; ++i) {
        lv_obj_t *c = blank(s_artBox);
        lv_obj_set_size(c, CTL_D, CTL_D);
        lv_obj_set_pos(c, ART / 2 + (i - 1) * CTL_PITCH - CTL_D / 2, ART / 2 - CTL_D / 2);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(s_pal.text), 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        s_ctl[i] = c;
        lv_obj_t *ic = lv_label_create(c);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
        lv_label_set_text(ic, icons[i]);
        lv_obj_center(ic);
        s_ctlIcon[i] = ic;
    }

    s_volBox = blank(s_artBox);
    lv_obj_set_size(s_volBox, ART, ART);
    lv_obj_set_style_bg_color(s_volBox, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_volBox, 215, 0);   // the number needs a quieter ground
    lv_obj_add_flag(s_volBox, LV_OBJ_FLAG_HIDDEN);

    s_volArc = lv_arc_create(s_volBox);
    lv_obj_remove_style_all(s_volArc);
    lv_obj_clear_flag(s_volArc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_volArc, 150, 150);
    lv_obj_center(s_volArc);
    lv_arc_set_rotation(s_volArc, 135);
    lv_arc_set_bg_angles(s_volArc, 0, 270);
    lv_arc_set_range(s_volArc, 0, 100);
    lv_obj_set_style_arc_width(s_volArc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_volArc, lv_color_hex(s_pal.volTrack), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_volArc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_volArc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_volArc, lv_color_hex(s_pal.live), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_volArc, true, LV_PART_INDICATOR);

    s_volNum = lv_label_create(s_volBox);
    lv_obj_set_style_text_font(s_volNum, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_volNum, lv_color_hex(s_pal.text), 0);
    lv_label_set_text(s_volNum, "");
    lv_obj_align(s_volNum, LV_ALIGN_CENTER, 0, -4);

    s_volIcon = lv_label_create(s_volBox);
    lv_obj_set_style_text_font(s_volIcon, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_volIcon, lv_color_hex(s_pal.dim), 0);
    lv_label_set_text(s_volIcon, "");
    lv_obj_align(s_volIcon, LV_ALIGN_CENTER, 0, 58);

    s_title = label(s_body, &lv_font_montserrat_24, s_pal.text, MID, TITLE_Y, TITLE_W);
    lv_obj_set_style_anim_speed(s_title, 30, 0);
    s_artist = label(s_body, &lv_font_montserrat_18, s_pal.dim, MID, ARTIST_Y, 320);
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_artist, lv_font_montserrat_18.line_height);   // one line, then "..."
    s_line = label(s_body, &lv_font_montserrat_16, s_pal.dim, MID, LINE_Y, 300);
    lv_label_set_recolor(s_line, true);
    s_lineSym = lv_label_create(s_body);
    lv_obj_set_style_text_font(s_lineSym, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_lineSym, "");
    lv_obj_set_y(s_lineSym, LINE_Y);
    lv_obj_add_flag(s_lineSym, LV_OBJ_FLAG_HIDDEN);

    s_empty = blank(s_scr);
    lv_obj_set_size(s_empty, SCREEN, SCREEN);
    s_emIcon = label(s_empty, &lv_font_montserrat_40, s_pal.faint, MID, 142, 200);
    s_emHead = label(s_empty, &lv_font_montserrat_24, s_pal.text, MID, 202, 340);
    s_emBody = label(s_empty, &lv_font_montserrat_18, s_pal.dim, MID, 244, 330);
    lv_label_set_long_mode(s_emBody, LV_LABEL_LONG_WRAP);
    s_emFoot = label(s_empty, &lv_font_montserrat_14, s_pal.dim, MID, 332, 260);
    lv_label_set_long_mode(s_emFoot, LV_LABEL_LONG_DOT);

    s_now = music::Now{};
    s_now.volume = -1;
    music::begin(ui_apply);
    s_timer = lv_timer_create(tick_cb, TICK_MS, nullptr);
    render();
    show(s_body, false);
}

lv_obj_t *screen() { return s_scr; }

void onEnter() {
    s_entered = true;
    s_now = music::Now{};
    s_now.volume = -1;
    s_mode = M_REST;
    s_toast = nullptr;
    s_lastLine[0] = 0;
    s_lastRing = -1;
    s_volDirty = false;
    restyle();
    s_backdrop = orb_style::attach(s_scr, "music");
    music::setShowing(true);
    render();
}

void onExit() {
    flush_volume();   // a change still in the debounce is something the person asked for
    s_mode = M_REST;
    vol_hide_now();
    s_entered = false;
    music::setShowing(false);
    drop_art();
    orb_style::release(s_backdrop);
}

void onPress() {
    if (!active()) {   // an empty state: a press asks again, now
        if (ponderer::configured()) music::pollSoon();
        return;
    }
    s_lastInput = now();
    if (s_mode == M_REST) {
        if (s_volShown) { flush_volume(); vol_hide_now(); }
        set_mode(M_CONTROL);
    } else {
        switch (s_sel) {
        case C_PREV: s_localSeq = music::queue(music::CMD_PREV); break;
        case C_NEXT: s_localSeq = music::queue(music::CMD_NEXT); break;
        case C_TOGGLE:
            s_localSeq = music::queue(music::CMD_TOGGLE);
            rebase();
            s_now.playing = !s_now.playing;
            break;
        }
    }
    render();
}

void onTurn(int delta) {
    if (!active()) return;
    if (s_mode == M_CONTROL) {
        s_lastInput = now();
        s_sel += delta;
        if (s_sel < 0) s_sel = 0;
        if (s_sel > C_NEXT) s_sel = C_NEXT;
        render();
        return;
    }
    // At rest: volume. The dial shows either way, so a turn on a device that will not
    // take a volume says so rather than doing nothing.
    const bool fresh = !s_volShown || s_volFading;
    vol_show();
    s_lastLine[0] = 0;
    if (s_now.canVolume && s_now.volume >= 0) {
        if (fresh && !s_volDirty) s_volTarget = s_now.volume;
        s_volTarget += delta * VOL_STEP;
        if (s_volTarget < 0) s_volTarget = 0;
        if (s_volTarget > 100) s_volTarget = 100;
        s_now.volume = s_volTarget;
        s_volDirty = true;
        s_volDirtyAt = now();
    }
    render();
}

}  // namespace musicview

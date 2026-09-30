// Music: Spotify now-playing and control, through orb-ponderer. See music_view.h; the
// network half is music_client.cpp.
//
// Layout, against the round glass rather than the square buffer:
//
//   - the ring IS the progress bar: a thin track just inside the bezel, filled clockwise
//     from twelve o'clock, green while playing and grey while paused;
//   - the playing device's name, small and spaced, where the chord is still 330 px wide;
//   - the cover, 200 px, whose corners sit 172 px from the centre and so well clear of the
//     ring;
//   - title (scrolls when long), artist (truncates), and a line with the play state and
//     the time.
//
// Knob, in the Flight Tracker's grammar. At rest the knob belongs to the shell (a turn opens
// the app switcher) and a press takes it: the cover dims and four controls appear on it,
// play/pause highlighted. Turn moves the highlight, press does it. On the volume control a
// press enters volume mode (turn sets it in steps of five, sent once the knob stops; press
// goes back to the controls). Six seconds without a touch gives the knob back, as does the
// rock gesture from anywhere. The line under the artist names whatever is highlighted, so
// the mode is never a guess.
//
// Memory: one cover, ART_PX^2 RGB565 (80 KB), in PSRAM, owned by this file from the moment
// music::takeArt() hands it over until onExit() or the next cover releases it.
#include "music_view.h"
#include "music_client.h"
#include "ponderer.h"
#include "app_shell.h"
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

constexpr uint32_t COL_TEXT   = 0xF2F2F2;
constexpr uint32_t COL_DIM    = 0x8A8F98;
constexpr uint32_t COL_FAINT  = 0x454A53;
constexpr uint32_t COL_TRACK  = 0x1B1E23;
constexpr uint32_t COL_ART_BG = 0x15171B;
constexpr uint32_t COL_PLAY   = 0x1ED760;   // Spotify's green: the one colour that says "live"
constexpr uint32_t COL_PAUSED = 0x6B7280;
constexpr uint32_t COL_WARN   = 0xFFB23F;

constexpr uint32_t TICK_MS     = 250;
constexpr uint32_t IDLE_MS     = 6000;   // the knob goes back after this much stillness
constexpr uint32_t VOL_SEND_MS = 350;    // a run of detents becomes one command
constexpr uint32_t VOL_HOLD_MS = 2500;   // trust our own volume over polls this long after
constexpr uint32_t TOAST_MS    = 2500;
constexpr int      VOL_STEP    = 5;

enum Mode { M_REST, M_CONTROL, M_VOLUME };
enum Ctl { C_PREV, C_TOGGLE, C_NEXT, C_VOL, C_COUNT };
constexpr int CTL_D     = 46;
constexpr int CTL_PITCH = 50;

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_ring = nullptr;
lv_obj_t *s_body = nullptr;
lv_obj_t *s_device = nullptr;
lv_obj_t *s_artBox = nullptr;
lv_obj_t *s_artImg = nullptr;
lv_obj_t *s_artPh = nullptr;
lv_obj_t *s_scrim = nullptr;
lv_obj_t *s_ctl[C_COUNT] = {};
lv_obj_t *s_ctlIcon[C_COUNT] = {};
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
const char *s_toast = nullptr;
uint32_t   s_toastUntil = 0;
char       s_lastLine[96] = "";
int        s_lastRing = -1;

uint8_t     *s_artBody = nullptr;   // the ORB5 allocation s_artDsc points into
lv_img_dsc_t s_artDsc;
char         s_artId[sizeof(music::Now::artId)] = "";

uint32_t now() { return lv_tick_get(); }
bool before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

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

void take_art() {
    uint8_t *body = nullptr; const uint16_t *px = nullptr; int w = 0, h = 0;
    char id[sizeof(s_artId)];
    if (!music::takeArt(&body, &px, &w, &h, id, sizeof(id))) return;
    drop_art();
    s_artBody = body;
    memcpy(s_artId, id, sizeof(s_artId));
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

int max_sel() { return s_now.canVolume ? C_VOL : C_NEXT; }

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
    if (m == M_REST) flush_volume();
    if (m != M_REST && s_mode == M_REST) s_sel = C_TOGGLE;
    s_mode = m;
    app_shell::setCaptured(m != M_REST);
    s_lastInput = now();
}

// ---- drawing -------------------------------------------------------------------------

void render_ring() {
    int v = 0;
    if (active() && s_now.durationMs > 0)
        v = (int)((int64_t)progress_now() * 1000 / s_now.durationMs);
    if (v != s_lastRing) { lv_arc_set_value(s_ring, v); s_lastRing = v; }
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(s_now.playing ? COL_PLAY : COL_PAUSED),
                               LV_PART_INDICATOR);
}

// The line under the artist: time and state at rest, the highlighted control's name while
// the knob is taken, a short failure notice when a command bounced.
void render_line() {
    char buf[96], a[16], b[16];
    uint32_t col = COL_DIM;
    if (s_toast && before(now(), s_toastUntil)) {
        snprintf(buf, sizeof(buf), "%s", s_toast);
        col = COL_WARN;
    } else if (s_mode == M_VOLUME) {
        snprintf(buf, sizeof(buf), "Turn to set, press when done");
        col = COL_TEXT;
    } else if (s_mode == M_CONTROL) {
        static const char *names[C_COUNT] = { "Previous", "", "Next", "Volume" };
        if (s_sel == C_TOGGLE) snprintf(buf, sizeof(buf), "%s", s_now.playing ? "Pause" : "Play");
        else if (s_sel == C_VOL && s_now.volume >= 0) snprintf(buf, sizeof(buf), "Volume %d%%", s_now.volume);
        else snprintf(buf, sizeof(buf), "%s", names[s_sel]);
        col = COL_TEXT;
    } else {
        fmt_time(a, sizeof(a), progress_now());
        fmt_time(b, sizeof(b), s_now.durationMs);
        const char *sym = s_now.playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE;
        const uint32_t sc = s_now.playing ? COL_PLAY : COL_TEXT;
        if (s_now.durationMs > 0)
            snprintf(buf, sizeof(buf), "#%06X %s#   %s / %s", (unsigned)sc, sym, a, b);
        else
            snprintf(buf, sizeof(buf), "#%06X %s#   %s", (unsigned)sc, sym, s_now.playing ? "Playing" : "Paused");
    }
    if (strcmp(buf, s_lastLine) != 0) {
        snprintf(s_lastLine, sizeof(s_lastLine), "%s", buf);
        lv_obj_set_style_text_color(s_line, lv_color_hex(col), 0);
        lv_label_set_text(s_line, buf);
    }
}

void render_controls() {
    const bool ctl = s_mode == M_CONTROL, vol = s_mode == M_VOLUME;
    show(s_scrim, ctl || vol);
    lv_obj_set_style_bg_opa(s_scrim, vol ? 215 : 165, 0);   // the number needs a quieter ground
    for (int i = 0; i < C_COUNT; ++i) {
        show(s_ctl[i], ctl);
        const bool on = ctl && i == s_sel;
        const bool dead = i == C_VOL && !s_now.canVolume;
        lv_obj_set_style_bg_color(s_ctl[i], lv_color_hex(on ? COL_TEXT : 0x000000), 0);
        lv_obj_set_style_bg_opa(s_ctl[i], on ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_set_style_border_opa(s_ctl[i], on ? LV_OPA_TRANSP : LV_OPA_40, 0);
        lv_obj_set_style_text_color(s_ctlIcon[i],
            lv_color_hex(on ? 0x000000 : dead ? COL_FAINT : COL_TEXT), 0);
    }
    set_text(s_ctlIcon[C_TOGGLE], s_now.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    show(s_volArc, vol);
    show(s_volNum, vol);
    show(s_volIcon, vol);
    if (vol) {
        const int v = s_now.volume < 0 ? 0 : s_now.volume;
        lv_arc_set_value(s_volArc, v);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", v);
        set_text(s_volNum, buf);
        set_text(s_volIcon, v == 0 ? LV_SYMBOL_MUTE : v < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
    }
}

// The title steps down a size before it resorts to moving: most long titles fit at 20 px,
// and a line that sits still is easier to read from across a room than one that scrolls.
// Only what still does not fit scrolls, circularly and slowly.
void set_title(const char *t) {
    if (strcmp(lv_label_get_text(s_title), t) == 0) return;
    const lv_font_t *f = &lv_font_montserrat_24;
    int box = TITLE_W;
    lv_coord_t w = lv_txt_get_width(t, strlen(t), f, 0, LV_TEXT_FLAG_NONE);
    if (w > box) {
        f = &lv_font_montserrat_20;
        box = TITLE_W_SMALL;
        w = lv_txt_get_width(t, strlen(t), f, 0, LV_TEXT_FLAG_NONE);
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
    set_text(s_artist, s_now.artist);
    char dev[64];
    snprintf(dev, sizeof(dev), "%s", s_now.device[0] ? s_now.device : "Spotify");
    for (char *c = dev; *c; ++c) if (*c >= 'a' && *c <= 'z') *c -= 32;
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
            if (s_mode == M_VOLUME || before(now(), s_volHoldUntil)) n.volume = s_now.volume;
            s_now = n;
            s_rxTick = now();
            if (s_now.status != music::ST_ACTIVE && s_mode != M_REST) set_mode(M_REST);
            if (s_sel > max_sel()) s_sel = max_sel();
        }
    }
    take_art();
    if (music::takeCmdFailed()) toast("Spotify did not take that");
    if (s_entered) render();
}

void tick_cb(lv_timer_t *) {
    if (!s_entered) return;
    if (s_volDirty && !before(now(), s_volDirtyAt + VOL_SEND_MS)) flush_volume();
    if (s_mode != M_REST && !before(now(), s_lastInput + IDLE_MS)) {
        set_mode(M_REST);
        render();
        return;
    }
    if (!active()) return;
    render_ring();
    render_line();
}

}  // namespace

namespace musicview {

void init() {
    if (s_scr) return;
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

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
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(COL_PLAY), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_ring, true, LV_PART_INDICATOR);

    s_body = blank(s_scr);
    lv_obj_set_size(s_body, SCREEN, SCREEN);

    s_device = label(s_body, &lv_font_montserrat_14, COL_DIM, MID, DEVICE_Y, 260);
    lv_obj_set_style_text_letter_space(s_device, 2, 0);
    lv_label_set_long_mode(s_device, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_device, lv_font_montserrat_14.line_height);

    s_artBox = blank(s_body);
    lv_obj_set_size(s_artBox, ART, ART);
    lv_obj_set_pos(s_artBox, MID - ART / 2, ART_Y);
    lv_obj_set_style_radius(s_artBox, 16, 0);
    lv_obj_set_style_clip_corner(s_artBox, true, 0);
    lv_obj_set_style_bg_color(s_artBox, lv_color_hex(COL_ART_BG), 0);
    lv_obj_set_style_bg_opa(s_artBox, LV_OPA_COVER, 0);

    s_artPh = lv_label_create(s_artBox);
    lv_obj_set_style_text_font(s_artPh, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_artPh, lv_color_hex(COL_FAINT), 0);
    lv_label_set_text(s_artPh, LV_SYMBOL_AUDIO);
    lv_obj_center(s_artPh);

    s_artImg = lv_img_create(s_artBox);
    lv_obj_add_flag(s_artImg, LV_OBJ_FLAG_HIDDEN);

    s_scrim = blank(s_artBox);
    lv_obj_set_size(s_scrim, ART, ART);
    lv_obj_set_style_bg_color(s_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scrim, 165, 0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);

    static const char *icons[C_COUNT] = { LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT, LV_SYMBOL_VOLUME_MAX };
    for (int i = 0; i < C_COUNT; ++i) {
        lv_obj_t *c = blank(s_artBox);
        lv_obj_set_size(c, CTL_D, CTL_D);
        lv_obj_set_pos(c, ART / 2 + (int)((i - 1.5f) * CTL_PITCH) - CTL_D / 2, ART / 2 - CTL_D / 2);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        s_ctl[i] = c;
        lv_obj_t *ic = lv_label_create(c);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_18, 0);
        lv_label_set_text(ic, icons[i]);
        lv_obj_center(ic);
        s_ctlIcon[i] = ic;
    }

    s_volArc = lv_arc_create(s_artBox);
    lv_obj_remove_style_all(s_volArc);
    lv_obj_clear_flag(s_volArc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_volArc, 150, 150);
    lv_obj_center(s_volArc);
    lv_arc_set_rotation(s_volArc, 135);
    lv_arc_set_bg_angles(s_volArc, 0, 270);
    lv_arc_set_range(s_volArc, 0, 100);
    lv_obj_set_style_arc_width(s_volArc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_volArc, lv_color_hex(0x3A3F47), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_volArc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_volArc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_volArc, lv_color_hex(COL_PLAY), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_volArc, true, LV_PART_INDICATOR);
    lv_obj_add_flag(s_volArc, LV_OBJ_FLAG_HIDDEN);

    s_volNum = lv_label_create(s_artBox);
    lv_obj_set_style_text_font(s_volNum, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_volNum, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(s_volNum, "");
    lv_obj_align(s_volNum, LV_ALIGN_CENTER, 0, -4);
    lv_obj_add_flag(s_volNum, LV_OBJ_FLAG_HIDDEN);

    s_volIcon = lv_label_create(s_artBox);
    lv_obj_set_style_text_font(s_volIcon, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_volIcon, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(s_volIcon, "");
    lv_obj_align(s_volIcon, LV_ALIGN_CENTER, 0, 58);
    lv_obj_add_flag(s_volIcon, LV_OBJ_FLAG_HIDDEN);

    s_title = label(s_body, &lv_font_montserrat_24, COL_TEXT, MID, TITLE_Y, TITLE_W);
    lv_obj_set_style_anim_speed(s_title, 30, 0);
    s_artist = label(s_body, &lv_font_montserrat_18, COL_DIM, MID, ARTIST_Y, 320);
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_artist, lv_font_montserrat_18.line_height);   // one line, then "..."
    s_line = label(s_body, &lv_font_montserrat_16, COL_DIM, MID, LINE_Y, 300);
    lv_label_set_recolor(s_line, true);

    s_empty = blank(s_scr);
    lv_obj_set_size(s_empty, SCREEN, SCREEN);
    s_emIcon = label(s_empty, &lv_font_montserrat_40, COL_FAINT, MID, 142, 200);
    s_emHead = label(s_empty, &lv_font_montserrat_24, COL_TEXT, MID, 202, 340);
    s_emBody = label(s_empty, &lv_font_montserrat_18, COL_DIM, MID, 244, 330);
    lv_label_set_long_mode(s_emBody, LV_LABEL_LONG_WRAP);
    s_emFoot = label(s_empty, &lv_font_montserrat_14, COL_DIM, MID, 332, 260);
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
    music::setShowing(true);
    render();
}

void onExit() {
    if (s_mode != M_REST) set_mode(M_REST);   // sends a volume change still in the debounce
    s_entered = false;
    music::setShowing(false);
    drop_art();
}

void onPress() {
    if (!active()) {   // an empty state: a press asks again, now
        if (ponderer::configured()) music::pollSoon();
        return;
    }
    s_lastInput = now();
    switch (s_mode) {
    case M_REST:
        set_mode(M_CONTROL);
        break;
    case M_CONTROL:
        switch (s_sel) {
        case C_PREV: s_localSeq = music::queue(music::CMD_PREV); break;
        case C_NEXT: s_localSeq = music::queue(music::CMD_NEXT); break;
        case C_TOGGLE:
            s_localSeq = music::queue(music::CMD_TOGGLE);
            rebase();
            s_now.playing = !s_now.playing;
            break;
        case C_VOL:
            if (!s_now.canVolume) break;
            if (s_now.volume < 0) s_now.volume = 50;
            s_volTarget = s_now.volume;
            set_mode(M_VOLUME);
            break;
        }
        break;
    case M_VOLUME:
        set_mode(M_CONTROL);
        break;
    }
    render();
}

void onTurn(int delta) {
    if (!active() || s_mode == M_REST) return;
    s_lastInput = now();
    if (s_mode == M_CONTROL) {
        s_sel += delta;
        if (s_sel < 0) s_sel = 0;
        if (s_sel > max_sel()) s_sel = max_sel();
    } else {
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

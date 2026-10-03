// Photos: a random photo, full-bleed on the round glass, changing every minute.
//
// Two sources, chosen from a small menu:
//   Pixel album  the relay (orb-ponderer) picks one, scales it to 466 x 466 and hands back
//                raw ORB5 pixels, so the device does no decoding at all.
//   SD card      a random file from /photos: .jpg/.jpeg (baseline), .png, or .orb5.
//
// The knob. Every detent reaches this screen (the app switcher is the rock gesture, which
// input_router handles before a turn gets here), so nothing needs capturing:
//   turn right     the next photo now: the prefetched one, so it is instant.
//   turn left      the photo before, kept in memory; if there is none, another one.
//                  Either restarts the one-minute timer.
//   press          opens the menu: Next photo / Pixel album / SD card, with "Next photo"
//                  highlighted. While it is open turns move the highlight, a press does
//                  it and closes the menu, and 5 s without input closes it untouched.
// The chosen source survives a reboot: NVS on the device, a file under /tmp in the
// simulator (the same arrangement theme_select.cpp uses for the theme).
//
// Threads, which is most of the design:
//   network side (ponderer netStep, core 0 / the simulator's net thread) fetches from the
//     relay and DECODES. It never touches LVGL and never touches the card.
//   UI side (LVGL timer + uiApply) reads files off the card in 16 KB chunks, ~12 ms per 20 ms tick,
//     and hands the bytes across. docs/memory.md rule 1 is "only the UI thread touches the
//     SD card", and main.cpp's upload handler relies on it with no lock at all, so the card
//     is read where every other screen reads it. The slow part, decoding a camera JPEG,
//     is pure CPU on bytes already in PSRAM, so it moves to the network side instead of
//     freezing the dial. Each chunk read also takes theme_sd::lock(), for the chime stream.
//
// Dress. Colours and faces come from orb_style::look("photos") on every enter (restyle()).
// A photograph stays in its own colours on every theme but a one-colour one (Cold War's
// phosphor), where it is redrawn in the theme's colour as it is made, on the network side,
// so it sits behind the CRT glass like everything else. The theme's glass and sparks lie
// over the photo; its plate is only ever seen behind the empty states and the first fade.
// On ink-on-paper themes the caption's scrim, the badge and the menu are paper, with ink on
// them. With no theme everything is exactly as it was first drawn.
//
// Memory (see docs/memory.md). A frame is 466 x 466 RGB565, 434 KB, always PSRAM. At most
// three exist (1.3 MB; two when the theme's backdrop would take the screen past ~2.2 MB of
// PSRAM, see frames_for_look(): then there is no "previous" and a left turn fetches another): the one on the glass, the one before it (so a left turn is instant)
// and the next one, prefetched so a right turn is. During a crossfade the outgoing photo
// holds the prefetch's place, and the network side will not make a fourth. Frames are taken on enter and given back on exit; one the network
// side is still holding is freed by that side on its next pass, never by this one (rule 2).
// While a card file is being read its raw bytes are a third, transient buffer, capped at
// SD_MAX_FILE and freed as soon as it is decoded.
#include "photo_view.h"
#include "photos_decode.h"
#include "ponderer.h"
#include "app_shell.h"
#include "curved_text.h"
#include "theme_sd.h"
#include "orb_style.h"
#include "theme_art.h"
#include "theme_select.h"
#include "theme_style.h"
#include <ArduinoJson.h>
#include <math.h>
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <Preferences.h>
#include <esp_random.h>
#include "sdcard.h"
#else
#include <chrono>
#include <cstdarg>
#include <dirent.h>
#include <random>
#include <string>
#include <sys/stat.h>
static struct {
    void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
} Serial;
#endif

namespace {

using photos_decode::SIDE;
using photos_decode::FRAME_BYTES;

constexpr int      MID            = SIDE / 2;
constexpr uint32_t CHANGE_MS      = 60000;    // a new photo this often
constexpr uint32_t RELAY_RETRY_MS = 30000;    // after the relay failed to answer
constexpr uint32_t NOCONF_RETRY_MS = 60000;   // nothing will change without a rebuild, but check
constexpr uint32_t SD_RESCAN_MS   = 10000;    // no card / no folder / nothing in it: look again
constexpr uint32_t SD_BADRUN_MS   = 30000;    // every file failed: pause before trying again
constexpr uint32_t MENU_IDLE_MS   = 5000;     // the menu lets go of the knob after this
constexpr uint32_t BADGE_MS       = 3000;     // how long the source name stays up
constexpr uint32_t FADE_MS        = 700;      // crossfade between photos
constexpr size_t   SD_CHUNK       = 16 * 1024;           // one card read: ~10 ms at 20 MHz
constexpr uint32_t SD_TICK_BUDGET_MS = 12;               // card time per 20 ms UI tick
constexpr size_t   SD_MAX_FILE    = 6u * 1024 * 1024;    // a 12 MP phone JPEG is 3-6 MB
#define SD_MAX_FILE_TEXT "6 MB"
constexpr const char *SD_DIR      = "/photos";

// Every colour the screen uses, worked out from the theme's look in restyle(). The values
// here are the unthemed screen's.
struct Pal {
    uint32_t text   = 0xF2F2F2;
    uint32_t soft   = 0xC9CDD4;   // the date under a caption, the menu's heading
    uint32_t dim    = 0x8A8F98;
    uint32_t accent = 0xFFB23F;   // trouble, and the tick beside the source in use
    uint32_t ground = 0x000000;   // the caption's scrim and shadow, the badge, the menu's veil
    uint32_t sel    = 0xF2F2F2;   // the highlighted menu row...
    uint32_t onSel  = 0x111111;   // ...and what is written on it
    lv_opa_t scrimMax = 200, veil = 170, badge = 150;
    int      glow   = 0;
};
const Pal PLAIN;
Pal s_pal;

enum Source : uint8_t { SRC_PIXEL = 0, SRC_SD = 1 };

const char *source_name(uint8_t s) { return s == SRC_SD ? "SD card" : "Pixel album"; }

// What stands between the screen and a photo, most specific first. Each gets its own
// sentence: "no card" and "card with nothing on it" are different fixes.
enum Status : uint8_t {
    ST_WORKING = 0,       // asked, nothing back yet
    ST_OK,
    ST_NOT_CONFIGURED,    // no relay URL/key in this build
    ST_NO_WIFI,
    ST_RELAY_DOWN,        // the relay did not answer at all
    ST_RELAY_NO_SOURCE,   // it answered: no photo source is set up (503, confirmed by hello)
    ST_RELAY_EMPTY,       // it says it has a source, but photo.next still failed
    ST_RELAY_BAD_IMAGE,   // photo.next answered, photo.img did not
    ST_NO_CARD,
    ST_NO_FOLDER,
    ST_NO_IMAGES,
    ST_UNREADABLE,        // there are files, and none of the last few would decode
};

uint32_t now_ms() {
#ifdef ARDUINO
    return millis();
#else
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

uint32_t rnd() {
#ifdef ARDUINO
    return esp_random();
#else
    static std::mt19937 g{ std::random_device{}() };
    return g();
#endif
}

// One picture, ready to draw.
struct Frame {
    uint8_t        *mem = nullptr;     // what to free
    bool            relayBody = false; // mem came from ponderer::get()
    const uint16_t *px = nullptr;
    int             w = 0, h = 0;
    char            caption[64] = "";
    char            date[24] = "";
    uint8_t         src = 0;
    uint32_t        gen = 0;
};

// One-colour themes: the ramp a photo's brightness is mapped through, ground to text colour.
// Written by the UI side in onEnter() before the screen goes active; read by the network
// side, which does nothing for this screen until it is.
std::atomic<bool> s_tint{ false };
uint16_t          s_tintRamp[64];

void tint_frame(Frame &f) {
    if (!s_tint.load() || !f.px) return;
    uint16_t *px = (uint16_t *)f.px;   // inside f.mem, which this frame owns
    const int n = f.w * f.h;
    for (int i = 0; i < n; ++i) {
        const uint16_t v = px[i];
        // 5-6-5 to a 6-bit luminance: (2R + 5G + B) / 8, each brought to 6 bits first.
        const int y = (((v >> 11) << 1) * 2 + ((v >> 5) & 63) * 5 + ((v & 31) << 1)) >> 3;
        px[i] = s_tintRamp[y > 63 ? 63 : y];
    }
}

void frame_free(Frame &f) {
    if (f.relayBody) ponderer::release(f.mem); else photos_decode::release(f.mem);
    f = Frame();
}

// ---- state shared by the two threads (s_mx) -------------------------------------------------

std::mutex            s_mx;
std::atomic<bool>     s_active{ false };   // the screen is on the glass
std::atomic<uint32_t> s_gen{ 1 };          // bumped on every enter and source change
std::atomic<uint8_t>  s_source{ SRC_PIXEL };

Frame    s_back;                  // the prefetched next photo (network side fills it)
bool     s_backReady = false;
int      s_uiFrames = 0;          // frames the UI side holds: on the glass, fading out, previous
int      s_maxFrames = 3;         // those, plus the prefetched next one, never more than this
                                  // (2 under a heavy backdrop; set in onEnter, under s_mx)
uint8_t  s_relayStatus = ST_WORKING;

// Card bytes on their way from the UI side to the decoder.
uint8_t *s_raw = nullptr;
size_t   s_rawLen = 0;
uint32_t s_rawGen = 0;
bool     s_rawReady = false;
char     s_rawName[96] = "";
// ...and what came of them when they would not decode.
bool     s_sdFailed = false;
char     s_sdFailWhy[40] = "";

bool may_make_frame_locked() { return s_uiFrames + (s_backReady ? 1 : 0) < s_maxFrames; }

// ---- network side ---------------------------------------------------------------------------

uint32_t s_workGen = 0;           // the generation the current decode/fetch belongs to
uint32_t s_relayRetryAt = 0;
bool     s_relayRetryArmed = false;
char     s_lastRelayId[64] = "";

bool keep_going() { return s_active.load() && s_gen.load() == s_workGen; }

void set_relay_status(uint8_t st) { std::lock_guard<std::mutex> g(s_mx); s_relayStatus = st; }

void relay_retry_in(uint32_t ms) { s_relayRetryAt = now_ms() + ms; s_relayRetryArmed = true; }

// Anything URL-unsafe in an id, percent-encoded. Ids are the relay's own ("p-3f2a9c"), but
// the relay is a different program and this costs nothing.
void url_encode(const char *in, char *out, size_t n) {
    static const char *HEXD = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < n; ++in) {
        const uint8_t c = (uint8_t)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = HEXD[c >> 4]; out[o++] = HEXD[c & 15]; }
    }
    out[o] = 0;
}

// The fonts have no fallback glyph, and the relay promises ASCII; hold it to that.
void copy_ascii(char *dst, size_t n, const char *src) {
    size_t o = 0;
    for (; src && *src && o + 1 < n; ++src)
        if ((uint8_t)*src >= 0x20 && (uint8_t)*src < 0x7F) dst[o++] = *src;
    dst[o] = 0;
}

// photo.next failed. ponderer::get() does not say how, so ask `hello`, which always works on
// a healthy relay and says whether a photo source is set up. That separates "the relay is
// not answering" from "it answered: nothing to show" (the 503) without guessing.
uint8_t classify_next_failure() {
    uint8_t *body = nullptr; size_t len = 0;
    if (!ponderer::get("hello", "", &body, &len, 4096, 6000)) return ST_RELAY_DOWN;
    JsonDocument doc;
    const bool parsed = !deserializeJson(doc, (const char *)body, len);
    ponderer::release(body);
    if (!parsed) return ST_RELAY_DOWN;
    return doc["photos"].as<bool>() ? ST_RELAY_EMPTY : ST_RELAY_NO_SOURCE;
}

bool relay_step() {
    if (s_relayRetryArmed && (int32_t)(now_ms() - s_relayRetryAt) < 0) return false;
    s_relayRetryArmed = false;
    if (!ponderer::configured()) {
        set_relay_status(ST_NOT_CONFIGURED);
        relay_retry_in(NOCONF_RETRY_MS);
        return true;
    }
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) {
        set_relay_status(ST_NO_WIFI);
        relay_retry_in(5000);
        return true;
    }
#endif
    { std::lock_guard<std::mutex> g(s_mx); if (!may_make_frame_locked()) return false; }

    char extra[200], enc[140];
    url_encode(s_lastRelayId, enc, sizeof(enc));
    snprintf(extra, sizeof(extra), "w=%d&h=%d%s%s", SIDE, SIDE, enc[0] ? "&not=" : "", enc);
    uint8_t *body = nullptr; size_t len = 0;
    if (!ponderer::get("photo.next", extra, &body, &len, 4096)) {
        const uint8_t st = keep_going() ? classify_next_failure() : ST_WORKING;
        Serial.printf("[photos] photo.next failed (%s)\n",
                      st == ST_RELAY_NO_SOURCE ? "no source on the relay" :
                      st == ST_RELAY_EMPTY ? "relay has a source but sent nothing" : "relay not answering");
        set_relay_status(st);
        relay_retry_in(RELAY_RETRY_MS);
        return true;
    }
    JsonDocument doc;
    const bool parsed = !deserializeJson(doc, (const char *)body, len);
    ponderer::release(body);
    Frame f;
    char id[64] = "";
    if (parsed) {
        copy_ascii(id, sizeof(id), doc["id"] | "");
        copy_ascii(f.caption, sizeof(f.caption), doc["caption"] | "");
        copy_ascii(f.date, sizeof(f.date), doc["date"] | "");
    }
    if (!id[0]) {
        Serial.printf("[photos] photo.next answered without an id\n");
        set_relay_status(ST_RELAY_EMPTY);
        relay_retry_in(RELAY_RETRY_MS);
        return true;
    }
    if (!keep_going()) return false;

    url_encode(id, enc, sizeof(enc));
    snprintf(extra, sizeof(extra), "id=%s&w=%d&h=%d", enc, SIDE, SIDE);
    uint8_t *img = nullptr; size_t ilen = 0;
    int w = 0, h = 0;
    const uint16_t *px = nullptr;
    if (ponderer::get("photo.img", extra, &img, &ilen, FRAME_BYTES + 64, 15000))
        px = ponderer::orb5_pixels(img, ilen, &w, &h);
    if (!px || w > SIDE || h > SIDE) {
        Serial.printf("[photos] photo.img %s: %s\n", id, img ? "not a usable ORB5 image" : "failed");
        ponderer::release(img);
        set_relay_status(ST_RELAY_BAD_IMAGE);
        relay_retry_in(RELAY_RETRY_MS);
        return true;
    }
    f.mem = img; f.relayBody = true; f.px = px; f.w = w; f.h = h;
    f.src = SRC_PIXEL; f.gen = s_workGen;
    tint_frame(f);
    strncpy(s_lastRelayId, id, sizeof(s_lastRelayId) - 1);
    std::lock_guard<std::mutex> g(s_mx);
    if (!keep_going() || s_backReady) { frame_free(f); return false; }
    s_back = f;
    s_backReady = true;
    s_relayStatus = ST_OK;
    return true;
}

// Decode the bytes the UI side read off the card.
bool sd_step() {
    uint8_t *raw; size_t len; char name[96];
    {
        std::lock_guard<std::mutex> g(s_mx);
        if (!s_rawReady || !may_make_frame_locked()) return false;
        raw = s_raw; len = s_rawLen;
        strncpy(name, s_rawName, sizeof(name));
        s_raw = nullptr; s_rawReady = false;
    }
    Frame f;
    f.src = SRC_SD; f.gen = s_workGen;
    char why[64] = "";
    bool ok = false;
    const uint32_t t0 = now_ms();
    int rw = 0, rh = 0;
    if (ponderer::orb5_pixels(raw, len, &rw, &rh) && rw == SIDE && rh == SIDE) {
        // Already exactly a frame: the file's own bytes are the picture, no copy.
        f.mem = raw; f.px = (const uint16_t *)(raw + 8); raw = nullptr;
        ok = true;
    } else if (uint8_t *dst = (uint8_t *)photos_decode::alloc(FRAME_BYTES)) {
        photos_decode::Meta meta;
        ok = photos_decode::decode(raw, len, (uint16_t *)dst, meta, why, sizeof(why), keep_going);
        if (ok) { f.mem = dst; f.px = (const uint16_t *)dst; strncpy(f.date, meta.date, sizeof(f.date) - 1); }
        else photos_decode::release(dst);
    } else {
        strncpy(why, "not enough memory", sizeof(why) - 1);
    }
    photos_decode::release(raw);
    if (ok) {
        f.w = f.h = SIDE;
        tint_frame(f);
        photos_decode::caption_from_name(name, f.caption, sizeof(f.caption));
        if (!f.date[0]) photos_decode::date_from_name(name, f.date, sizeof(f.date));
        Serial.printf("[photos] %s: decoded in %u ms\n", name, (unsigned)(now_ms() - t0));
    } else if (keep_going()) {
        Serial.printf("[photos] %s: skipped, %s\n", name, why);
    }
    std::lock_guard<std::mutex> g(s_mx);
    if (!keep_going()) { frame_free(f); return false; }
    if (ok) { s_back = f; s_backReady = true; }
    else    { s_sdFailed = true; snprintf(s_sdFailWhy, sizeof(s_sdFailWhy), "%s", why); }
    return true;
}

bool net_step() {
    if (!s_active.load()) {
        // Left the screen: whatever this side still holds goes back now, from this side.
        std::lock_guard<std::mutex> g(s_mx);
        if (s_backReady) { frame_free(s_back); s_backReady = false; }
        if (s_raw) { photos_decode::release(s_raw); s_raw = nullptr; s_rawReady = false; }
        s_relayRetryArmed = false;
        return false;
    }
    const uint32_t gen = s_gen.load();
    {
        std::lock_guard<std::mutex> g(s_mx);
        if (s_backReady && s_back.gen != gen) { frame_free(s_back); s_backReady = false; }
        if (s_raw && s_rawGen != gen) { photos_decode::release(s_raw); s_raw = nullptr; s_rawReady = false; }
        if (s_backReady) return false;
    }
    if (s_workGen != gen) { s_workGen = gen; s_relayRetryArmed = false; }
    return s_source.load() == SRC_SD ? sd_step() : relay_step();
}

// ---- UI side: widgets -----------------------------------------------------------------------

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_img[2] = {};           // two, so one can fade in over the other
lv_img_dsc_t s_dsc[2];
int       s_top = 0;               // which of the two is the current photo
lv_obj_t *s_scrim = nullptr;       // darkens the bottom of the photo under the caption
lv_obj_t *s_capCanvas = nullptr;
uint8_t  *s_capBuf = nullptr;
lv_obj_t *s_badge = nullptr;       // "SD CARD", briefly, at the top
lv_obj_t *s_msg = nullptr;         // empty states
lv_obj_t *s_msgTitle = nullptr;
lv_obj_t *s_msgHint = nullptr;
lv_obj_t *s_msgIcon = nullptr;
lv_obj_t *s_menu = nullptr;
lv_obj_t *s_menuRow[3] = {};
lv_obj_t *s_menuLbl[3] = {};
lv_obj_t *s_menuMark[3] = {};
lv_obj_t *s_menuTitle = nullptr;
lv_timer_t *s_timer = nullptr;
orb_style::Backdrop *s_backdrop = nullptr;

constexpr int CAP_Y = 340;                     // the caption band: y 340..466
constexpr int CAP_H = SIDE - CAP_Y;
constexpr int SCRIM_Y = 306;
constexpr int SCRIM_H = SIDE - SCRIM_Y;
uint8_t s_scrimPx[SCRIM_H * 3];                // 1 px wide, tiled across by lv_img
lv_img_dsc_t s_scrimDsc;

Frame    s_front, s_old;           // on the glass, and the one fading out under it
Frame    s_prev;                   // the one shown before, kept so a left turn is instant
uint32_t s_shownAt = 0;
bool     s_wantNow = false;        // "Next photo", or a new source: swap as soon as one is ready
bool     s_announce = false;       // show the source badge with the next photo
bool     s_menuOpen = false;
int      s_menuSel = 0;
uint32_t s_menuTouched = 0;

// Card reading (UI side only).
struct SdRead {
    bool     busy = false;
#ifdef ARDUINO
    File     f;
#else
    FILE    *f = nullptr;
#endif
    uint8_t *buf = nullptr;
    size_t   len = 0, got = 0;
    char     name[96] = "";
    uint32_t gen = 0;
} s_rd;
uint8_t  s_sdStatus = ST_WORKING;
bool     s_sdInFlight = false;     // bytes handed over, answer not back yet
uint32_t s_sdRetryAt = 0;
int      s_sdCount = 0;            // eligible files at the last look
int      s_sdFailRun = 0;          // consecutive files that would not decode
int      s_sdTried = 0;            // how many that run was, when it gave up
char     s_sdWhy[40] = "";         // why the last one would not, in words for the dial
char     s_lastPicked[96] = "";     // the last file tried...
char     s_lastGood[96] = "";       // ...and the last one that worked; neither is picked next
char     s_handed[96] = "";         // the file whose bytes are with the decoder now

void show(lv_obj_t *o, bool on) {
    if (!o) return;
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

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

// ---- dress ----------------------------------------------------------------------------------

uint32_t mix(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int sh) { return (uint32_t)lroundf(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t) << sh; };
    return ch(16) | ch(8) | ch(0);
}

const lv_font_t *montserrat(int px) {
    switch (px) {
        case 12: return &lv_font_montserrat_12; case 14: return &lv_font_montserrat_14;
        case 16: return &lv_font_montserrat_16; case 20: return &lv_font_montserrat_20;
        case 22: return &lv_font_montserrat_22; case 28: return &lv_font_montserrat_28;
    }
    return &lv_font_montserrat_16;
}

// A theme's face may carry only the glyphs its own screens use, and a missing one draws as
// nothing at all; so a face is only worn for words it can spell.
bool covers(const lv_font_t *f, const char *t) {
    lv_font_glyph_dsc_t g;
    for (uint32_t i = 0; t[i];) {
        const uint32_t c = _lv_txt_encoded_next(t, &i);
        if (c == '\n' || c == '\r') continue;
        if (!lv_font_get_glyph_dsc(f, &g, c, 0)) return false;
    }
    return true;
}

// The theme's face for these words at about this size (when they also fit in maxW, if one
// is given), else Montserrat at exactly it.
const lv_font_t *face(orb_style::Role role, int px, const char *words, int maxW = 0) {
    if (orb_style::themed_font(role, px)) {
        const lv_font_t *f = orb_style::font(role, px);
        if (covers(f, words)) {
            if (!maxW) return f;
            lv_point_t sz;
            lv_txt_get_size(&sz, words, f, 0, 0, LV_COORD_MAX, 0);
            if (sz.x <= maxW) return f;
        }
    }
    return montserrat(px);
}

// A headline: the theme's headline face, its text face failing that.
const lv_font_t *headline(int px, const char *words, int maxW = 0) {
    const lv_font_t *f = face(orb_style::TITLE, px, words, maxW);
    return f == montserrat(px) ? face(orb_style::BODY, px, words, maxW) : f;
}

void set_font(lv_obj_t *l, const lv_font_t *f) {
    if (lv_obj_get_style_text_font(l, 0) != f) lv_obj_set_style_text_font(l, f, 0);
}

Pal palette(const orb_style::Look &l) {
    const orb_style::Look none;
    const bool plain = l.text == none.text && l.dim == none.dim && l.accent == none.accent &&
                       l.accent2 == none.accent2 && l.rule == none.rule && l.bg == none.bg &&
                       l.dark && !l.mono && !l.glow && !l.sparks && !l.plate[0] && !l.overlay[0];
    if (plain) return PLAIN;
    Pal p;
    p.text   = l.text;
    p.soft   = mix(l.text, l.dim, 0.4f);
    p.dim    = l.dim;
    p.accent = l.accent;
    p.ground = l.bg;
    p.sel    = l.accent;
    p.onSel  = l.bg;
    p.glow   = l.glow;
    // Ink on paper: the veils are paper, and heavier, so ink reads on them over any photo.
    if (!l.dark) { p.scrimMax = 235; p.veil = 215; p.badge = 215; }
    return p;
}

// How many frames this theme leaves room for. The backdrop's plate (424 KB) and glass
// (636 KB) are PSRAM unless the theme is baked into flash, where they cost nothing.
int frames_for_look(const orb_style::Look &l) {
    size_t total = 3 * FRAME_BYTES + (size_t)LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SIDE, CAP_H);
    int w = 0, h = 0;
    const char *slug = theme_select::activeSlug();
    if (!slug || !slug[0]) return 3;   // no theme folder: nothing for a backdrop to load
    if (l.plate[0] && theme_style::hasAsset(l.plate) &&
        !theme_art::find_active(l.plate, theme_art::FMT_RGB565, w, h)) total += (size_t)SIDE * SIDE * 2;
    if (l.overlay[0] && theme_style::hasAsset(l.overlay) &&
        !theme_art::find_active(l.overlay, theme_art::FMT_RGB565_ALPHA, w, h)) total += (size_t)SIDE * SIDE * 3;
    return total > 2200u * 1024u ? 2 : 3;
}

void ui_frames_update() {
    std::lock_guard<std::mutex> g(s_mx);
    s_uiFrames = (s_front.mem ? 1 : 0) + (s_old.mem ? 1 : 0) + (s_prev.mem ? 1 : 0);
}

// ---- persistence ----------------------------------------------------------------------------

#ifndef ARDUINO
constexpr const char *SIM_SOURCE_FILE = "/tmp/orb_sim_photo_source";
#endif

uint8_t load_source() {
#ifdef ARDUINO
    Preferences p;
    p.begin("capsuleradar", true);
    const uint8_t v = p.getUChar("photo_src", SRC_PIXEL);
    p.end();
    return v == SRC_SD ? SRC_SD : SRC_PIXEL;
#else
    FILE *f = fopen(SIM_SOURCE_FILE, "r");
    if (!f) return SRC_PIXEL;
    const int c = fgetc(f);
    fclose(f);
    return c == '1' ? SRC_SD : SRC_PIXEL;
#endif
}

void save_source(uint8_t v) {
#ifdef ARDUINO
    Preferences p;
    p.begin("capsuleradar", false);
    p.putUChar("photo_src", v);
    p.end();
#else
    if (FILE *f = fopen(SIM_SOURCE_FILE, "w")) { fputc(v == SRC_SD ? '1' : '0', f); fclose(f); }
#endif
}

// ---- caption --------------------------------------------------------------------------------

// Shorten to fit `maxW` px: whole words where possible, then "...". The arc has room for
// about thirty-five characters of caption.
void fit(const lv_font_t *font, const char *in, char *out, size_t n, int maxW) {
    snprintf(out, n, "%s", in);
    lv_point_t sz;
    lv_txt_get_size(&sz, out, font, 0, 0, LV_COORD_MAX, 0);
    if (sz.x <= maxW) return;
    char base[80];
    snprintf(base, sizeof(base), "%s", in);
    size_t len = strlen(base);
    bool words = true;                       // first try cutting at spaces, then anywhere
    while (len > 1) {
        if (words) {
            while (len > 0 && base[len - 1] != ' ') --len;
            if (len == 0) { words = false; len = strlen(base); continue; }
        } else {
            --len;
        }
        while (len > 0 && (base[len - 1] == ' ' || base[len - 1] == ',')) --len;
        if (len == 0) break;
        snprintf(out, n, "%.*s...", (int)len, base);
        lv_txt_get_size(&sz, out, font, 0, 0, LV_COORD_MAX, 0);
        if (sz.x <= maxW) return;
    }
}

void draw_caption(const Frame &f) {
    if (!s_capCanvas || !s_capBuf) return;
    lv_canvas_fill_bg(s_capCanvas, lv_color_black(), LV_OPA_TRANSP);
    const bool cap = f.caption[0] != 0, date = f.date[0] != 0;
    show(s_capCanvas, cap || date);
    show(s_scrim, cap || date);
    if (!cap && !date) return;
    const curved_text::Target dst = { s_capBuf, SIDE, CAP_H };
    const float cx = MID, cy = MID - CAP_Y;
    const lv_color_t shadow = lv_color_hex(s_pal.ground);
    char buf[80];
    // Caption inside, date outside it. Along the bottom the arc's outer line is the lower
    // one, and the smaller type belongs nearer the rim, where the chord is shortest.
    if (cap) {
        const lv_font_t *font = headline(20, f.caption);
        const float R = date ? 174.0f : 186.0f;
        fit(font, f.caption, buf, sizeof(buf), (int)(R * 1.9f));
        curved_text::draw_arc(dst, font, buf, cx, cy, R, 180.0f, lv_color_hex(s_pal.text), 3, shadow);
    }
    if (date) {
        const lv_font_t *font = face(orb_style::SMALL, cap ? 14 : 16, f.date);
        const float R = cap ? 199.0f : 190.0f;
        fit(font, f.date, buf, sizeof(buf), (int)(R * 1.2f));
        curved_text::draw_arc(dst, font, buf, cx, cy, R, 180.0f, lv_color_hex(s_pal.soft), 3, shadow);
    }
    lv_obj_invalidate(s_capCanvas);
}

void badge(uint8_t src) {
    if (!s_badge) return;
    lv_anim_del(s_badge, nullptr);
    const char *words = src == SRC_SD ? "SD CARD" : "PIXEL ALBUM";
    set_font(s_badge, face(orb_style::SMALL, 12, words));
    lv_label_set_text(s_badge, words);
    lv_obj_set_style_opa(s_badge, LV_OPA_COVER, 0);
    show(s_badge, true);
    lv_obj_fade_out(s_badge, 500, BADGE_MS);
}

// ---- photos on the glass --------------------------------------------------------------------

void fade_cb(void *obj, int32_t v) { lv_obj_set_style_img_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }

void drop_old() {
    const int under = s_top ^ 1;
    if (s_img[under]) {
        show(s_img[under], false);
        lv_img_set_src(s_img[under], nullptr);
        lv_img_cache_invalidate_src(&s_dsc[under]);
    }
    // The photo that just left becomes "previous", so turning left brings it straight back.
    // (Under a heavy backdrop there is room for two frames, and the other is the next one.)
    frame_free(s_prev);
    if (s_maxFrames >= 3) s_prev = s_old; else frame_free(s_old);
    s_old = Frame();
    ui_frames_update();
}

void fade_done(lv_anim_t *) { drop_old(); }

void put_on_glass(Frame &f) {
    if (s_old.mem) {                             // a fade still running: finish it now
        lv_anim_del(s_img[s_top], fade_cb);
        lv_obj_set_style_img_opa(s_img[s_top], LV_OPA_COVER, 0);
        drop_old();
    }
    const int next = s_front.mem ? (s_top ^ 1) : s_top;
    if (s_front.mem) s_old = s_front;
    s_front = f;
    f = Frame();
    ui_frames_update();

    lv_img_dsc_t &d = s_dsc[next];
    memset(&d, 0, sizeof(d));
    d.header.always_zero = 0;
    d.header.cf = LV_IMG_CF_TRUE_COLOR;
    d.header.w = (uint32_t)s_front.w;
    d.header.h = (uint32_t)s_front.h;
    d.data_size = (uint32_t)s_front.w * s_front.h * 2;
    d.data = (const uint8_t *)s_front.px;
    lv_img_cache_invalidate_src(&d);
    lv_img_set_src(s_img[next], &d);
    lv_obj_center(s_img[next]);
    lv_obj_move_foreground(s_img[next]);
    // Everything that belongs over the photo, back on top of it.
    lv_obj_move_foreground(s_scrim);
    lv_obj_move_foreground(s_capCanvas);
    lv_obj_move_foreground(s_badge);
    lv_obj_move_foreground(s_menu);
    orb_style::raise(s_backdrop);                // and the theme's glass over all of it
    show(s_img[next], true);
    s_top = next;

    lv_obj_set_style_img_opa(s_img[next], LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_img[next]);
    lv_anim_set_exec_cb(&a, fade_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&a, fade_done);
    lv_anim_start(&a);

    draw_caption(s_front);
    s_shownAt = now_ms();
    s_wantNow = false;
    if (s_announce) { badge(s_front.src); s_announce = false; }
}

// ---- what to say when there is no photo -----------------------------------------------------

void message(const char *icon, bool trouble, const char *title, const char *detail) {
    lv_label_set_text(s_msgIcon, icon);
    lv_obj_set_style_text_color(s_msgIcon, lv_color_hex(trouble ? s_pal.accent : s_pal.dim), 0);
    set_font(s_msgTitle, headline(22, title, 320));
    set_font(s_msg, face(orb_style::BODY, 16, detail, 330));
    lv_label_set_text(s_msgTitle, title);
    lv_label_set_text(s_msg, detail);
}

void render() {
    if (!s_scr) return;
    const uint8_t src = s_source.load();
    uint8_t st;
    { std::lock_guard<std::mutex> g(s_mx); st = src == SRC_SD ? s_sdStatus : s_relayStatus; }
    const bool current = s_front.mem && s_front.src == src;
    const bool failing = st != ST_WORKING && st != ST_OK;
    // A photo from this source always wins. One from the other source stays up while the
    // new one is fetched, and gives way the moment the new source turns out to be unwell.
    const bool photo = current || (s_front.mem && !failing);
    for (int i = 0; i < 2; ++i) show(s_img[i], photo && (i == s_top || s_old.mem));
    const bool words = photo && !s_menuOpen && (s_front.caption[0] || s_front.date[0]);
    show(s_capCanvas, words);
    show(s_scrim, words);
    const bool msg = !photo && !s_menuOpen;
    show(s_msgIcon, msg); show(s_msgTitle, msg); show(s_msg, msg); show(s_msgHint, msg && st != ST_WORKING);
    if (!msg) return;
    switch (st) {
        case ST_NOT_CONFIGURED:  message(LV_SYMBOL_SETTINGS, true, "Pixel album not set up", "This Orb has no relay key.\nAdd one in ponderer_config.h."); break;
        case ST_NO_WIFI:         message(LV_SYMBOL_WIFI, true, "No WiFi", "The Pixel album arrives over WiFi.\nYour Orb is fine."); break;
        case ST_RELAY_DOWN:      message(LV_SYMBOL_WARNING, true, "Relay not answering", "orb-ponderer did not reply.\nTrying again shortly."); break;
        case ST_RELAY_NO_SOURCE: message(LV_SYMBOL_IMAGE, true, "No album chosen", "The relay has no photo source.\nPick one on its setup page."); break;
        case ST_RELAY_EMPTY:     message(LV_SYMBOL_IMAGE, true, "No photos came back", "The relay has a source but\nsent nothing. Is the album empty?"); break;
        case ST_RELAY_BAD_IMAGE: message(LV_SYMBOL_WARNING, true, "Photo did not arrive", "The relay chose one but could\nnot send it. Trying again shortly."); break;
        case ST_NO_CARD:         message(LV_SYMBOL_SD_CARD, true, "No SD card", "Put a card with a /photos\nfolder in the Orb."); break;
        case ST_NO_FOLDER:       message(LV_SYMBOL_DIRECTORY, true, "No /photos folder", "Make a folder called photos\nat the top of the SD card."); break;
        case ST_NO_IMAGES:       message(LV_SYMBOL_DIRECTORY, true, "No photos on the card", "/photos has no .jpg, .png\nor .orb5 files in it."); break;
        case ST_UNREADABLE: {
            static char detail[96];
            if (s_sdTried > 1)
                snprintf(detail, sizeof(detail), "The last %d tried would not open.\nLast one: %s.", s_sdTried, s_sdWhy);
            else
                snprintf(detail, sizeof(detail), "The only photo would not open:\n%s.", s_sdWhy);
            message(LV_SYMBOL_WARNING, true, s_sdTried > 1 ? "Can't read these photos" : "Can't read this photo", detail);
            break;
        }
        default:
            message(src == SRC_SD ? LV_SYMBOL_SD_CARD : LV_SYMBOL_IMAGE, false,
                    src == SRC_SD ? "SD card" : "Pixel album", "Finding a photo...");
            break;
    }
}

// Take the prefetched photo if it is time, or if one was asked for.
void consider_swap() {
    if (s_old.mem) return;                        // mid-crossfade: the second slot is busy
    const uint8_t src = s_source.load();
    const bool due = !s_front.mem || s_front.src != src || s_wantNow ||
                     now_ms() - s_shownAt >= CHANGE_MS;
    if (!due) return;
    Frame f;
    {
        std::lock_guard<std::mutex> g(s_mx);
        if (!s_backReady || s_back.gen != s_gen.load() || s_back.src != src) return;
        f = s_back;
        s_back = Frame();
        s_backReady = false;
        // Counted before the lock drops, so the network side cannot squeeze a third frame
        // in between taking this one and putting it on the glass.
        s_uiFrames = (s_front.mem ? 1 : 0) + (s_prev.mem ? 1 : 0) + 1;
    }
    put_on_glass(f);
}

// ---- the card, read from this side ----------------------------------------------------------

enum Pick { PICK_OK, PICK_NO_CARD, PICK_NO_FOLDER, PICK_EMPTY };

// Uniformly random among the eligible files, never the one picked last time when there is
// any other. One pass over the directory and nothing stored: /photos can hold thousands.
struct Chooser {
    int count = 0, others = 0;
    char pick[96] = "";
    char fallback[96] = "";          // an excluded one, for a folder with nothing else in it
    void spare(const char *leaf) { if (!fallback[0]) snprintf(fallback, sizeof(fallback), "%s", leaf); }
    void offer(const char *leaf) {
        if (photos_decode::kind_of_name(leaf) == photos_decode::K_NONE) return;
        ++count;
        if (s_lastPicked[0] && !strcmp(leaf, s_lastPicked)) { spare(leaf); return; }
        if (s_lastGood[0] && !strcmp(leaf, s_lastGood))     { spare(leaf); return; }
        ++others;
        if (rnd() % (uint32_t)others == 0) snprintf(pick, sizeof(pick), "%s", leaf);
    }
};

Pick sd_pick(char *path, size_t n) {
    Chooser ch;
#ifdef ARDUINO
    if (!sdcard::mounted()) return PICK_NO_CARD;
    theme_sd::lock();
    File dir = SD.open(SD_DIR);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); theme_sd::unlock(); return PICK_NO_FOLDER; }
    bool isDir = false;
    for (;;) {
        String nm = dir.getNextFileName(&isDir);
        if (!nm.length()) break;
        if (isDir) continue;
        const char *leaf = strrchr(nm.c_str(), '/');
        ch.offer(leaf ? leaf + 1 : nm.c_str());
    }
    dir.close();
    theme_sd::unlock();
#else
    // The simulator's card is sim/sdcard (as theme_sd and roads_sd have it). A missing
    // sim/sdcard is a missing card; ORB_SIM_PHOTOS_NO_CARD=1 says the same for testing.
    const char *nocard = getenv("ORB_SIM_PHOTOS_NO_CARD");
    struct stat st;
    if ((nocard && nocard[0] == '1') || stat("sim/sdcard", &st) != 0) return PICK_NO_CARD;
    DIR *dir = opendir((std::string("sim/sdcard") + SD_DIR).c_str());
    if (!dir) return PICK_NO_FOLDER;
    while (dirent *e = readdir(dir)) {
        if (e->d_type == DT_DIR) continue;
        ch.offer(e->d_name);
    }
    closedir(dir);
#endif
    s_sdCount = ch.count;
    if (!ch.count) return PICK_EMPTY;
    if (!ch.pick[0]) snprintf(ch.pick, sizeof(ch.pick), "%s", ch.fallback);   // only repeats left
    snprintf(path, n, "%s/%s", SD_DIR, ch.pick);
    snprintf(s_lastPicked, sizeof(s_lastPicked), "%s", ch.pick);
    return PICK_OK;
}

void sd_close() {
#ifdef ARDUINO
    if (s_rd.f) { theme_sd::lock(); s_rd.f.close(); theme_sd::unlock(); }
#else
    if (s_rd.f) { fclose(s_rd.f); s_rd.f = nullptr; }
#endif
}

void sd_abort() {
    if (!s_rd.busy) return;
    sd_close();
    photos_decode::release(s_rd.buf);
    s_rd = SdRead();
}

void sd_set_status(uint8_t st) { std::lock_guard<std::mutex> g(s_mx); s_sdStatus = st; }

void sd_failed_file(const char *why) {
    snprintf(s_sdWhy, sizeof(s_sdWhy), "%s", why);
    ++s_sdFailRun;
    const int limit = s_sdCount < 5 ? s_sdCount : 5;
    if (s_sdFailRun >= (limit > 0 ? limit : 1)) {
        sd_set_status(ST_UNREADABLE);
        s_sdRetryAt = now_ms() + SD_BADRUN_MS;
        s_sdTried = s_sdFailRun;
        s_sdFailRun = 0;
    }
}

bool sd_open(const char *path) {
#ifdef ARDUINO
    theme_sd::lock();
    s_rd.f = SD.open(path, FILE_READ);
    const bool ok = s_rd.f && !s_rd.f.isDirectory();
    s_rd.len = ok ? s_rd.f.size() : 0;
    theme_sd::unlock();
    return ok;
#else
    s_rd.f = fopen((std::string("sim/sdcard") + path).c_str(), "rb");
    if (!s_rd.f) return false;
    fseek(s_rd.f, 0, SEEK_END);
    const long sz = ftell(s_rd.f);
    fseek(s_rd.f, 0, SEEK_SET);
    s_rd.len = sz > 0 ? (size_t)sz : 0;
    return true;
#endif
}

size_t sd_read(uint8_t *into, size_t n) {
#ifdef ARDUINO
    theme_sd::lock();
    const size_t got = s_rd.f.read(into, n);
    theme_sd::unlock();
    return got;
#else
    return fread(into, 1, n, s_rd.f);
#endif
}

// One step of card work per UI tick: look for a file, or read the next chunk of one.
void sd_tick() {
    if (s_source.load() != SRC_SD) { sd_abort(); return; }
    const uint32_t gen = s_gen.load();
    if (s_rd.busy && s_rd.gen != gen) sd_abort();

    if (s_rd.busy) {
        // Chunks until this tick's budget is spent, so a fast card is not held to one chunk
        // per tick and a slow one never holds the dial for more than about a frame.
        const uint32_t t0 = now_ms();
        size_t got = 1;
        while (s_rd.got < s_rd.len && got && now_ms() - t0 < SD_TICK_BUDGET_MS) {
            size_t want = s_rd.len - s_rd.got;
            if (want > SD_CHUNK) want = SD_CHUNK;
            got = sd_read(s_rd.buf + s_rd.got, want);
            s_rd.got += got;
        }
        if (got == 0 && s_rd.got < s_rd.len) {
            Serial.printf("[photos] %s: read failed at %u of %u bytes\n", s_rd.name,
                          (unsigned)s_rd.got, (unsigned)s_rd.len);
            sd_abort();
            sd_failed_file("card read error");
            return;
        }
        if (s_rd.got < s_rd.len) return;
        sd_close();
        {
            std::lock_guard<std::mutex> g(s_mx);
            s_raw = s_rd.buf; s_rawLen = s_rd.len; s_rawGen = s_rd.gen;
            snprintf(s_rawName, sizeof(s_rawName), "%s", s_rd.name);
            s_rawReady = true;
        }
        snprintf(s_handed, sizeof(s_handed), "%s", s_lastPicked);
        s_rd.buf = nullptr;
        s_rd = SdRead();
        s_sdInFlight = true;
        return;
    }

    if (s_sdInFlight || (int32_t)(now_ms() - s_sdRetryAt) < 0) return;
    {
        std::lock_guard<std::mutex> g(s_mx);
        if (s_backReady || s_rawReady || s_uiFrames >= s_maxFrames) return;
    }
    char path[112];
    switch (sd_pick(path, sizeof(path))) {
        case PICK_NO_CARD:   sd_set_status(ST_NO_CARD);   s_sdRetryAt = now_ms() + SD_RESCAN_MS; return;
        case PICK_NO_FOLDER: sd_set_status(ST_NO_FOLDER); s_sdRetryAt = now_ms() + SD_RESCAN_MS; return;
        case PICK_EMPTY:     sd_set_status(ST_NO_IMAGES); s_sdRetryAt = now_ms() + SD_RESCAN_MS; return;
        case PICK_OK: break;
    }
    if (s_sdStatus == ST_NO_CARD || s_sdStatus == ST_NO_FOLDER || s_sdStatus == ST_NO_IMAGES ||
        s_sdStatus == ST_UNREADABLE)
        sd_set_status(ST_WORKING);
    snprintf(s_rd.name, sizeof(s_rd.name), "%s", path);
    if (!sd_open(path)) {
        Serial.printf("[photos] %s: would not open\n", path);
        sd_close();
        s_rd = SdRead();
        sd_failed_file("file would not open");
        return;
    }
    if (s_rd.len < 8 || s_rd.len > SD_MAX_FILE) {
        Serial.printf("[photos] %s: skipped, %u bytes is %s\n", path, (unsigned)s_rd.len,
                      s_rd.len < 8 ? "too small to be a picture" : "over the size limit");
        const bool small = s_rd.len < 8;
        sd_close();
        s_rd = SdRead();
        sd_failed_file(small ? "not a picture" : "file over " SD_MAX_FILE_TEXT);
        return;
    }
    s_rd.buf = (uint8_t *)photos_decode::alloc(s_rd.len);
    if (!s_rd.buf) {
        Serial.printf("[photos] %s: no %u KB block free to read it into\n", path, (unsigned)(s_rd.len / 1024));
        sd_close();
        s_rd = SdRead();
        sd_failed_file("not enough memory");
        return;
    }
    s_rd.busy = true;
    s_rd.got = 0;
    s_rd.gen = gen;
}

// The network side finished something: a photo, or a card file that would not decode.
void ui_apply() {
    if (!s_scr || !s_active.load()) return;
    bool failed;
    bool ready;
    char why[40];
    {
        std::lock_guard<std::mutex> g(s_mx);
        failed = s_sdFailed; s_sdFailed = false;
        ready = s_backReady && s_back.src == SRC_SD;
        snprintf(why, sizeof(why), "%s", s_sdFailWhy);
    }
    if (failed) { s_sdInFlight = false; sd_failed_file(why); }
    if (ready) {
        s_sdInFlight = false;
        s_sdFailRun = 0;
        snprintf(s_lastGood, sizeof(s_lastGood), "%s", s_handed);
        sd_set_status(ST_OK);
    }
    consider_swap();
    render();
}

// ---- stepping by hand -----------------------------------------------------------------------

// The prefetched photo, now. If it is still on its way it goes up the moment it lands.
void next_now() { s_wantNow = true; consider_swap(); }

// The photo before this one, from memory. Gone (never shown, or a source change): another.
void go_back() {
    if (s_old.mem) {                             // mid-fade: settle it, which fills s_prev
        lv_anim_del(s_img[s_top], fade_cb);
        lv_obj_set_style_img_opa(s_img[s_top], LV_OPA_COVER, 0);
        drop_old();
    }
    if (!s_prev.mem || s_prev.src != s_source.load()) { next_now(); return; }
    Frame f = s_prev;
    s_prev = Frame();
    ui_frames_update();
    put_on_glass(f);
}

// ---- the menu -------------------------------------------------------------------------------

void menu_paint() {
    const uint8_t src = s_source.load();
    for (int i = 0; i < 3; ++i) {
        const bool sel = i == s_menuSel;
        lv_obj_set_style_bg_opa(s_menuRow[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_menuLbl[i], lv_color_hex(sel ? s_pal.onSel : s_pal.text), 0);
        lv_obj_set_style_shadow_width(s_menuRow[i], sel ? s_pal.glow * 8 : 0, 0);
        const bool mark = (i == 1 && src == SRC_PIXEL) || (i == 2 && src == SRC_SD);
        show(s_menuMark[i], mark);
        lv_obj_set_style_text_color(s_menuMark[i], lv_color_hex(sel ? s_pal.onSel : s_pal.accent), 0);
    }
}

void menu_open() {
    s_menuOpen = true;
    s_menuSel = 0;
    s_menuTouched = now_ms();
    menu_paint();
    show(s_menu, true);
    lv_anim_del(s_badge, nullptr);
    show(s_badge, false);
    render();
}

void menu_close() {
    if (!s_menuOpen) return;
    s_menuOpen = false;
    show(s_menu, false);
    render();
}

void set_source(uint8_t src) {
    if (src == s_source.load()) { s_wantNow = true; consider_swap(); return; }
    s_source.store(src);
    save_source(src);
    s_gen.fetch_add(1);          // anything in flight for the old source is now stale
    sd_abort();
    s_sdInFlight = false;
    s_sdRetryAt = 0;
    s_sdFailRun = 0;
    sd_set_status(ST_WORKING);
    set_relay_status(ST_WORKING);
    frame_free(s_prev);          // "previous" belongs to the old source now
    ui_frames_update();
    s_wantNow = true;
    s_announce = true;
    badge(src);                  // straight away, so the choice shows before the photo does
    Serial.printf("[photos] source: %s\n", source_name(src));
}

void tick_cb(lv_timer_t *) {
    if (!s_active.load()) return;
    sd_tick();
    consider_swap();
    if (s_menuOpen && now_ms() - s_menuTouched >= MENU_IDLE_MS) menu_close();
    render();
}

// The scrim's pixels: the ground colour (black, or paper), clear at the top of the band and
// most of the way to solid at the rim.
void paint_scrim() {
    const uint32_t c = s_pal.ground;
    const uint16_t v = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
    for (int y = 0; y < SCRIM_H; ++y) {
        const float t = (float)y / (SCRIM_H - 1);
        const float e = t * t * (3.0f - 2.0f * t);
        s_scrimPx[y * 3] = v & 0xFF; s_scrimPx[y * 3 + 1] = v >> 8;
        s_scrimPx[y * 3 + 2] = (uint8_t)((float)s_pal.scrimMax * e);
    }
}

void build() {
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; ++i) {
        s_img[i] = lv_img_create(s_scr);
        lv_obj_clear_flag(s_img[i], LV_OBJ_FLAG_CLICKABLE);
        show(s_img[i], false);
    }

    // The scrim: black, clear at the top of the band and about two-thirds dark at the rim,
    // eased so there is no visible edge where it starts. One pixel wide; lv_img tiles it.
    paint_scrim();
    memset(&s_scrimDsc, 0, sizeof(s_scrimDsc));
    s_scrimDsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_scrimDsc.header.w = 1;
    s_scrimDsc.header.h = SCRIM_H;
    s_scrimDsc.data_size = sizeof(s_scrimPx);
    s_scrimDsc.data = s_scrimPx;
    s_scrim = lv_img_create(s_scr);
    lv_img_set_src(s_scrim, &s_scrimDsc);
    lv_obj_set_size(s_scrim, SIDE, SCRIM_H);
    lv_obj_set_pos(s_scrim, 0, SCRIM_Y);
    show(s_scrim, false);

    // The caption's canvas is attached on enter (it is 180 KB); this is its place holder.
    s_capCanvas = lv_canvas_create(s_scr);
    lv_obj_set_pos(s_capCanvas, 0, CAP_Y);
    show(s_capCanvas, false);

    s_badge = label(s_scr, &lv_font_montserrat_12, s_pal.text);
    lv_obj_set_style_text_letter_space(s_badge, 2, 0);
    lv_obj_set_style_bg_color(s_badge, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_badge, 150, 0);
    lv_obj_set_style_radius(s_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_badge, 12, 0);
    lv_obj_set_style_pad_ver(s_badge, 5, 0);
    lv_obj_align(s_badge, LV_ALIGN_TOP_MID, 0, 34);
    show(s_badge, false);

    // Empty states: a line saying what is wrong, two saying what to do, and where the menu is.
    s_msgIcon = label(s_scr, &lv_font_montserrat_28, s_pal.dim);
    lv_obj_align(s_msgIcon, LV_ALIGN_CENTER, 0, -86);
    s_msgTitle = label(s_scr, &lv_font_montserrat_22, s_pal.text);
    lv_obj_set_width(s_msgTitle, 320);
    lv_obj_align(s_msgTitle, LV_ALIGN_CENTER, 0, -38);
    s_msg = label(s_scr, &lv_font_montserrat_16, s_pal.dim);
    lv_obj_set_width(s_msg, 330);
    lv_obj_set_style_text_line_space(s_msg, 4, 0);
    lv_obj_align(s_msg, LV_ALIGN_CENTER, 0, 16);
    s_msgHint = label(s_scr, &lv_font_montserrat_14, s_pal.dim);
    lv_label_set_text(s_msgHint, "Press for options");
    lv_obj_align(s_msgHint, LV_ALIGN_BOTTOM_MID, 0, -62);

    // The menu: the photo dimmed behind three rows.
    s_menu = blank(s_scr);
    lv_obj_set_size(s_menu, SIDE, SIDE);
    lv_obj_set_style_bg_color(s_menu, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_menu, 170, 0);
    lv_obj_t *title = label(s_menu, &lv_font_montserrat_12, s_pal.soft);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_label_set_text(title, "PHOTOS");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -92);
    s_menuTitle = title;
    static const char *ROWS[3] = { "Next photo", "Pixel album", "SD card" };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *r = blank(s_menu);
        lv_obj_set_size(r, 250, 46);
        lv_obj_align(r, LV_ALIGN_CENTER, 0, -44 + i * 54);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(s_pal.sel), 0);
        s_menuRow[i] = r;
        s_menuLbl[i] = label(r, &lv_font_montserrat_20, s_pal.text);
        lv_label_set_text(s_menuLbl[i], ROWS[i]);
        lv_obj_center(s_menuLbl[i]);
        s_menuMark[i] = label(r, &lv_font_montserrat_16, s_pal.accent);
        lv_label_set_text(s_menuMark[i], LV_SYMBOL_OK);
        lv_obj_align(s_menuMark[i], LV_ALIGN_RIGHT_MID, -18, 0);
    }
    show(s_menu, false);
}

// Put the theme's colours and faces on everything, and set the network side up to match.
// On every enter: the look is not known when init() builds the screen.
void restyle() {
    const orb_style::Look &l = orb_style::look("photos");
    s_pal = palette(l);
    const Pal &p = s_pal;
    auto text = [](lv_obj_t *o, uint32_t c) { lv_obj_set_style_text_color(o, lv_color_hex(c), 0); };

    paint_scrim();
    lv_img_cache_invalidate_src(&s_scrimDsc);
    lv_obj_invalidate(s_scrim);

    text(s_badge, p.text);
    lv_obj_set_style_bg_color(s_badge, lv_color_hex(p.ground), 0);
    lv_obj_set_style_bg_opa(s_badge, p.badge, 0);

    text(s_msgTitle, p.text);
    text(s_msg, p.dim);
    text(s_msgHint, p.dim);
    set_font(s_msgHint, face(orb_style::SMALL, 14, lv_label_get_text(s_msgHint)));

    lv_obj_set_style_bg_color(s_menu, lv_color_hex(p.ground), 0);
    lv_obj_set_style_bg_opa(s_menu, p.veil, 0);
    text(s_menuTitle, p.soft);
    set_font(s_menuTitle, face(orb_style::SMALL, 12, lv_label_get_text(s_menuTitle)));
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_style_bg_color(s_menuRow[i], lv_color_hex(p.sel), 0);
        lv_obj_set_style_shadow_color(s_menuRow[i], lv_color_hex(p.sel), 0);
        lv_obj_set_style_shadow_opa(s_menuRow[i], p.glow ? LV_OPA_80 : LV_OPA_TRANSP, 0);
        // The tick sits 18 px in from the right; the words must clear it on both sides.
        set_font(s_menuLbl[i], face(orb_style::BODY, 20, lv_label_get_text(s_menuLbl[i]), 250 - 2 * 44));
    }

    if (l.mono) {
        for (int i = 0; i < 64; ++i) {
            const uint32_t c = mix(l.bg, l.text, i / 63.0f);
            s_tintRamp[i] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
        }
    }
    s_tint.store(l.mono);
    const int frames = frames_for_look(l);
    std::lock_guard<std::mutex> g(s_mx);
    s_maxFrames = frames;
}

void attach_caption_canvas() {
    if (s_capBuf) return;
    s_capBuf = (uint8_t *)photos_decode::alloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SIDE, CAP_H));
    if (!s_capBuf) { Serial.printf("[photos] caption canvas alloc failed: photos without captions\n"); return; }
    lv_canvas_set_buffer(s_capCanvas, s_capBuf, SIDE, CAP_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(s_capCanvas, lv_color_black(), LV_OPA_TRANSP);
}

void release_caption_canvas() {
    if (!s_capBuf) return;
    // LVGL will not take a null canvas buffer (it wedges on the next switch, docs/memory.md),
    // so the canvas is rebuilt rather than emptied.
    lv_obj_del(s_capCanvas);
    s_capCanvas = lv_canvas_create(s_scr);
    lv_obj_set_pos(s_capCanvas, 0, CAP_Y);
    show(s_capCanvas, false);
    photos_decode::release(s_capBuf);
    s_capBuf = nullptr;
}

}  // namespace

namespace photoview {

void init() {
    if (s_scr) return;
    s_source.store(load_source());
    build();
    ponderer::add({ "photos", net_step, ui_apply });
    s_timer = lv_timer_create(tick_cb, 20, nullptr);
    lv_timer_pause(s_timer);
}

lv_obj_t *screen() { return s_scr; }

void onEnter() {
    restyle();                       // before the network side is let loose: it reads the tint
    attach_caption_canvas();
    s_backdrop = orb_style::attach(s_scr, "photos");
    s_gen.fetch_add(1);
    s_wantNow = true;
    s_announce = true;
    s_sdRetryAt = 0;
    s_sdFailRun = 0;
    s_sdInFlight = false;
    {
        std::lock_guard<std::mutex> g(s_mx);
        s_sdStatus = ST_WORKING;
        s_relayStatus = ST_WORKING;
    }
    s_active.store(true);
    lv_timer_resume(s_timer);
    render();
}

void onExit() {
    s_active.store(false);
    lv_timer_pause(s_timer);
    menu_close();
    sd_abort();
    s_sdInFlight = false;
    lv_anim_del(s_badge, nullptr);
    show(s_badge, false);
    for (int i = 0; i < 2; ++i) {
        lv_anim_del(s_img[i], nullptr);
        lv_img_set_src(s_img[i], nullptr);
        lv_img_cache_invalidate_src(&s_dsc[i]);
        show(s_img[i], false);
    }
    frame_free(s_old);
    frame_free(s_front);
    frame_free(s_prev);
    ui_frames_update();
    release_caption_canvas();
    show(s_scrim, false);
    orb_style::release(s_backdrop);
    // The prefetched frame and any card bytes in flight belong to the network side, which
    // frees them on its next pass now that s_active is false.
}

void onPress() {
    if (!s_menuOpen) { menu_open(); return; }
    const int sel = s_menuSel;
    menu_close();
    if (sel == 0) { s_wantNow = true; consider_swap(); }
    else set_source(sel == 1 ? SRC_PIXEL : SRC_SD);
    render();
}

void onTurn(int delta) {
    if (!s_menuOpen) {
        // At rest a detent is a step through the photos; the timer restarts either way.
        if (delta > 0) next_now(); else if (delta < 0) go_back();
        render();
        return;
    }
    s_menuSel += delta;
    if (s_menuSel < 0) s_menuSel = 0;
    if (s_menuSel > 2) s_menuSel = 2;
    s_menuTouched = now_ms();
    menu_paint();
}

}  // namespace photoview

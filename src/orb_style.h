#pragma once
#include <lvgl.h>
#include <stdint.h>
// How this fork's own screens (Forecast, Music, Photos, Facts, Plasma, Globe) dress for
// the theme being worn. Orb Studio cannot style them (its source is not ours), so they
// borrow from what every theme already carries, and a theme that knows about them can say
// more.
//
// Where a look comes from, later winning over earlier:
//   1. Derived from the theme itself: the plain backdrop every theme ships for its menu
//      (menu_plate.png), the menu's own text colours (made to be read on that backdrop),
//      and the News and Settings typefaces. This is what an unknown theme gets, and it
//      already matches.
//   2. A built-in preset for the stock themes, by theme name (Aviator, Cold War 1983,
//      Modern, Steam Punk), tuned by eye.
//   3. extras_style.json in the theme's folder, for a theme made with these screens in
//      mind (the fork's own "Plasma" theme). Same keys as Look below; colours are
//      "#rrggbb" strings or numbers; "screens": {"forecast": {...}} overrides per screen.
//   With no theme at all: black, white and grey, exactly as these screens were first drawn.
//
// Simulator: ORB_STYLE=<preset name> forces a preset (aviator, coldwar, modern, steampunk,
// plasma, plain) without needing that theme installed.
namespace orb_style {

struct Look {
    uint32_t bg      = 0x000000;   // behind everything, and what shows with no plate
    uint32_t text    = 0xF2F2F2;   // primary text
    uint32_t dim     = 0x8A8F98;   // secondary text, captions
    uint32_t accent  = 0xFFB23F;   // the one emphatic colour: rings, highlights, live things
    uint32_t accent2 = 0x4FA3FF;   // a second, cooler one
    uint32_t rule    = 0x2A2E36;   // hairlines, tracks, quiet fills
    bool     dark    = true;       // light text on a dark ground (false: ink on paper)
    bool     mono    = false;      // one-colour instrument: draw pictures in text/accent only
    uint8_t  glow    = 0;          // 0 none .. 3 heavy: how much shapes and key text may glow
    bool     sparks  = false;      // drifting sparks over the backdrop
    uint8_t  plateDim = 0;         // 0..255 of black laid over the plate, for legibility
    char     plate[41]   = "";     // backdrop image in the theme folder, or ""
    char     overlay[41] = "";     // glass / CRT layer over everything, or ""
};

// The look for one screen: "forecast", "music", "photos", "facts", "plasma", "globe".
// Cheap; loads the theme's look on first use. The reference stays valid until reload().
const Look &look(const char *screen);
void reload();   // forget what was loaded (the theme changed; in practice a restart does this)

inline lv_color_t color(uint32_t rgb) { return lv_color_hex(rgb); }

// Typefaces. A theme's fonts are baked at ONE size each, so a screen asks for a role and
// gets the theme's face when it has one near that size, and the compiled Montserrat of
// `px` otherwise. `px` must be one of the compiled sizes (12..28 even, 32..48 by 4).
enum Role { TITLE, BODY, SMALL };
const lv_font_t *font(Role role, int px);
// True when font(role, px) would return the theme's own face rather than Montserrat.
bool themed_font(Role role, int px);

// The backdrop for a screen while it is showing: the plate behind everything the screen
// has, and the glass and sparks in front of it. attach() in onEnter (after the screen's
// own objects exist; it reorders itself), release() in onExit. Costs up to ~424 KB of
// PSRAM for a plate and ~636 KB for a glass layer, both freed on release; on the device a
// plate the theme has baked into flash costs nothing.
struct Backdrop;
Backdrop *attach(lv_obj_t *screen, const char *screenName);
void release(Backdrop *&b);
// Bring the front layers (glass, sparks) back above anything a screen created since.
void raise(Backdrop *b);

}  // namespace orb_style

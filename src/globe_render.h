#pragma once
// The Globe screen's renderer: the Earth as seen from the Sun, at half the panel's
// resolution (233 x 233 RGB565), for globe_view to double into LVGL's draw buffer the
// way Plasma does.
//
// Why it is cheap. The camera is locked to the Sun, so the light falls on the same screen
// pixels whatever the time: shading, the ocean glint and the limb haze are per-pixel
// constants, built once. What the time changes is the globe's orientation, and that is two
// angles: the tilt (the subsolar latitude, which drifts by under half a degree a day) and
// the spin (the subsolar longitude, a quarter of a degree a minute, plus the knob). The
// tilt goes into a per-pixel table of texture row and "base longitude", rebuilt only when
// the tilt has moved; the spin is then just a number added to every pixel's longitude.
// A frame is one table walk: add, sample the map (bilinear, four texels), multiply by the
// shade, add the glint and the haze. About 29,000 pixels, no trigonometry.
//
// Projection. Orthographic, north up. The globe is turned exactly as Doug's homepage turns
// its three.js globe (Euler XYZ: rotation.y = -subsolarLon - 90 deg, then rotation.x =
// subsolarLat), so the subsolar point is dead centre; the light is the homepage's
// directional light at (1.2, 0.6, 5) plus its 0.45 ambient, which leaves a thin
// terminator at the lower-left limb.
//
// Styles. The Earth is drawn one of two ways. The photograph (above) is what the screen
// shows with no theme, and under a theme that is itself photographic. A theme with a hand
// of its own gets a drawn globe instead, from the baked coast map (a signed distance to
// the shore, see tools/bake_globe_texture.py): the same table walk, but each pixel is one
// bilinear sample of that distance and one lookup into a 256-entry colour ramp that
// already holds the sea, the shoreline, its glow or its water-lining and the land's fill;
// then a graticule every 30 degrees (parallels from a per-pixel table built with the
// orientation; meridians from the pixel's longitude and a per-pixel "pixels per degree"),
// and the same shade and limb tables, which here carry the limb's drawn ring. It costs a
// little less per frame than the photograph: one channel to interpolate, not three.
// What the ramp, the lines and the ring look like is the Style, made from a theme's
// colours by make_style(): a phosphor outline, an engraved atlas, an antique brass globe,
// a neon one.
#include <stdint.h>

namespace globe {

constexpr int RW = 233;              // frame, half the 466 px panel
constexpr int R = 96;                // globe radius, render px (192 on the panel)
constexpr int X0 = 20;               // the table box: render px [X0, X0 + BOX)
constexpr int BOX = 193;

struct Style {
    bool    chart = false;           // drawn from the coast map rather than the photograph
    bool    stars = true;            // the sky has stars (only with no theme)
    bool    sky = true;              // the limb glows into the sky; false: nothing is drawn there
    uint8_t hz[3] = { 96, 160, 255 };// the atmosphere, or a drawn globe's limb ring
    float   glow = 1.0f;             // the sky glow's strength
    float   haze = 1.0f;             // the limb haze's strength, inside the disc
    // drawn globes only
    uint8_t ramp[256][3];            // colour by coast distance (128 = the shore, more = land)
    uint8_t grat[3] = { 0, 0, 0 };   // the graticule's colour...
    uint8_t gratA = 0;               // ...its opacity, 0..255...
    float   gratW = 0.5f;            // ...and its lines' half-width, render px
    float   shading = 0;             // 0 = flat, 1 = lit like the photograph
    float   ringW = 0;               // the limb ring's width, render px (3 and over: a metal band)
    float   gloss = 0;               // a window's reflection in the glass, 0..1
    float   relief = 0;              // how much the land takes light and dark from the photograph
};

// The looks a theme can ask for. PLAIN is the screen with no theme.
enum Kind { PLAIN, PHOTO, PHOSPHOR, ATLAS, ANTIQUE, NEON };
struct Colours { uint32_t bg, text, dim, accent, accent2, rule; int glow; };   // a theme's, 0xRRGGBB; glow 0..3
void make_style(Style &s, Kind kind, const Colours &c);

struct Render {
    uint16_t *out;                   // RW*RW, the frame: static sky + the globe on top
    uint16_t *lon;                   // BOX*BOX, base longitude, 65536 = 360 deg (tilt-dependent)
    uint16_t *row;                   // BOX*BOX, texture row, 8.8 fixed point (tilt-dependent)
    uint8_t  *shade;                 // BOX*BOX, texture multiplier /256
    uint8_t  *spec;                  // BOX*BOX, the ocean glint, added over water
    uint8_t  *haze;                  // BOX*BOX, the atmosphere at the limb, added everywhere
    uint8_t  *latLine;               // BOX*BOX, drawn globes: the parallels' coverage (tilt-dependent)
    uint8_t  *lonQ;                  // BOX*BOX, drawn globes: render px per unit of longitude (tilt-dependent)
    Style    *st;                    // a copy of the style in use (0.8 KB)
    int16_t  *spanL, *spanR;          // BOX each: the disc, per table row (box columns); L > R = none
    float     tilt;                  // the subsolar latitude the tables were built for
    bool      tablesOk;
};

// Allocate (PSRAM on the device), build the static tables and paint the sky into out.
// False if anything failed; the caller then frees whatever arrived with render_free().
// With a style whose sky is off, out is the limb's colour outside the disc and the caller
// is expected not to draw it.
bool render_alloc(Render &r, const Style &st);
void render_free(Render &r);

// Rebuild the orientation tables for a subsolar latitude (degrees). ~50k asin/atan2.
void build_tables(Render &r, float tiltDeg);

// Draw the globe into r.out with the subsolar longitude (plus any knob spin) at `lonDeg`
// facing the viewer.
void draw(Render &r, float lonDeg);

// Where a point on the Earth lands: screen-space unit coordinates (x right, y up, z towards
// the viewer, all -1..1 of the globe radius) for the given orientation.
void project(float latDeg, float lonDeg, float tiltDeg, float faceLonDeg,
             float *x, float *y, float *z);

} // namespace globe

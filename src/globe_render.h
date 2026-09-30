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
#include <stdint.h>

namespace globe {

constexpr int RW = 233;              // frame, half the 466 px panel
constexpr int R = 96;                // globe radius, render px (192 on the panel)
constexpr int X0 = 20;               // the table box: render px [X0, X0 + BOX)
constexpr int BOX = 193;

struct Render {
    uint16_t *out;                   // RW*RW, the frame: static sky + the globe on top
    uint16_t *lon;                   // BOX*BOX, base longitude, 65536 = 360 deg (tilt-dependent)
    uint16_t *row;                   // BOX*BOX, texture row, 8.8 fixed point (tilt-dependent)
    uint8_t  *shade;                 // BOX*BOX, texture multiplier /256
    uint8_t  *spec;                  // BOX*BOX, the ocean glint, added over water
    uint8_t  *haze;                  // BOX*BOX, the atmosphere at the limb, added everywhere
    int16_t  *spanL, *spanR;          // BOX each: the disc, per table row (box columns); L > R = none
    float     tilt;                  // the subsolar latitude the tables were built for
    bool      tablesOk;
};

// Allocate (PSRAM on the device), build the static tables and paint the sky into out.
// False if anything failed; the caller then frees whatever arrived with render_free().
bool render_alloc(Render &r);
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

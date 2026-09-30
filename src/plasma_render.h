#pragma once
// The Plasma screen's renderer: turns plasma_engine's channels into a picture of light,
// at half the panel's resolution, into an RGB565 buffer the view scales up.
//
// Why it is built this way, briefly:
//   - Half resolution (233 x 233). The web page's thinnest stroke is under two panel pixels
//     and everything else is glow, so the loss is small, and every full-frame pass costs a
//     quarter of what it would. The view doubles it with a cheap bilinear step as LVGL asks
//     for each band.
//   - Additive, into an 8-bit RGB accumulator, the same as the page's 'lighter' blending.
//     Strokes are soft capsules drawn a scanline at a time: each pixel is one distance to a
//     segment and one lookup into a small per-segment table that already holds the colour
//     of every pass (glow, body, core) at that distance.
//   - Bloom without reading the frame back. The page blurs a downsampled copy of the
//     finished frame; here every stroke also drops its light into a 60 x 60 grid as it is
//     drawn, the grid is blurred, and the final pass adds it back. The widest, faintest
//     passes (the haze round a channel, the haze of a brush) only ever go to the grid.
//   - One full-frame pass. It adds the glass, the gas and the bloom, clamps, dithers,
//     packs to RGB565, and clears the accumulator for the next frame on its way past.
// Everything outside the circle is left alone, and stays black.
#include <stdint.h>
#include "plasma_engine.h"

namespace plasma {

constexpr int RW = 233;          // render grid, half the 466 px panel
constexpr int LG = 60;           // bloom grid: cells of 4 render px
constexpr int LCELL = 4;
constexpr int FIBMAX = 10;       // brush fibres per foot
constexpr int FIB = 7;           // numbers per fibre

struct Render {
    uint8_t  *acc;               // RW*RW*3, additive light
    uint16_t *out;               // RW*RW, the finished frame, RGB565
    float    *glow;              // LG*LG*3, light for the bloom
    float    *tmp;               // LG*LG*3, blur scratch
    uint8_t  *base;              // 1024*3: the gas and the glass, by radius squared
    uint8_t  *refl;              // reflection sprite (alpha), reflW*reflH
    uint8_t  *elec;              // electrode sprite: RGB + coverage per px, elecS*elecS*4
    uint8_t  *elecGlow;          // electrode's added light, elecS*elecS*3
    float    *fib;               // MAXF*FIBMAX*FIB brush fibre state
    int reflX, reflY, reflW, reflH;
    int elecS, elecX, elecY;
    float elecE[3];              // the electrode's added light, summed, for the bloom
    int16_t chordL[RW], chordR[RW];
    float hue;
    uint32_t rng;
    // what the last frame cost, for the log
    int segs;
    // the Power readout, drawn as light on the inside of the glass when showing
    float ring;                  // 0..1 how much of the arc is lit; < 0 hides it
    float ringA;                 // its brightness, 0..1, for fading it out
};

// Allocate (PSRAM on the device) and build the tables. False if anything failed; the
// caller then frees whatever did arrive with render_free().
bool render_alloc(Render &r, float hue, float r0);
void render_free(Render &r);

// Draw the engine's current state into r.out.
void render_frame(Render &r, const Engine &e);

} // namespace plasma

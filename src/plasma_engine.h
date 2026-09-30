#pragma once
// The discharge behind the Plasma screen: a C++ port of the engine in Doug's plasma
// project (~/Claude/plasma/index.html, between its `=== engine ===` markers).
//
// It is a model of mechanisms, not an animation. Channels are chains of nodes from the
// electrode to the glass; each carries a current found by solving the circuit (a driver
// with a finite output impedance, every channel a resistor set by how ionised its gas is,
// in series with the patch of glass under its foot). Charge piles up under a foot and
// pushes it off onto fresh glass; channels zip together into trunks, unzip, fork when a
// foot is choked, and go out when starved. How many there are is not a setting: Power is
// the drive voltage and the count is wherever breakdown and burning settle.
//
// What changed in the port, and why:
//   - No fingers. The Orb's touch is compiled out, so everything to do with a fingertip's
//     capacitance, groups and captured channels is gone. Nothing else depended on it.
//   - Fixed pools. MAXF channels of NN nodes, CELLS patches of glass, all in one struct
//     the view allocates in PSRAM on enter and frees on exit.
//   - Fewer nodes (16, was 20). Tension is left at the original's value: scaling it for
//     the shorter chain was tried and cost a third of the branching.
//   - A semi-implicit step. The original takes 5 ms explicit substeps because hot
//     channels are stiff; here the tension on each node is divided by (1 + h * its own
//     stiffness) while every other force stays explicit, which is stable at a
//     thirtieth of a second, so a frame needs two substeps rather than seven, and a
//     channel drifting as a whole still drifts at full speed.
//   - Cheap noise and exp: a sum of uniforms for the Gaussian, a table for exp(-x).
// Single precision throughout, no doubles, no per-node transcendental calls.
#include <stdint.h>

namespace plasma {

constexpr int NN     = 16;   // nodes along a channel, electrode to glass
constexpr int MAXF   = 48;   // channel ceiling: the frame budget, not physics (web: 56)
constexpr int CELLS  = 400;  // patches of glass that can hold charge (web: 480)
constexpr int TRAILN = 8;    // past foot positions remembered for the tail
constexpr int FPMAX  = 40;   // max glass patches under one foot's footprint

struct Fil {
    float d[NN * 3];         // direction of each node (unit vectors); node j sits on shell radii[j]
    float ou[NN * 3];        // turbulence, per node
    float rep[NN * 3];       // repulsion, held for a step
    float ex[NN * 3];        // pull of branches on this trunk
    float exW[NN];
    float seg[NN];           // ionisation of segment j (node j-1 -> j)
    float segI[NN];          // current through segment j
    float trail[TRAILN * 3];
    float I, life, low, cool, R, z, a, phi, gx, gy, gz, hue, trailAt;
    int16_t par, k;          // nodes 0..k belong to channel `par` (-1: a trunk of its own)
    uint8_t used, dying, trailN;
    // footprint on the glass, found once a step, used for both the potential and deposition
    uint8_t fpN;
    uint16_t fpI[FPMAX];
    float fpW[FPMAX];
};

struct Stats { uint32_t strikes, deaths, forks, zips, unzips, splits; };

struct Engine {
    Fil fil[MAXF];
    float cell[CELLS * 3];
    float q[CELLS];
    float radii[NN];
    float V, time, next;
    float r0;
    Stats st;
    uint32_t rng;
};

// Build tables and put the ball in its switched-off state. r0 is the electrode's radius
// as a fraction of the glass's.
void init(Engine &e, float r0, uint32_t seed);
// Advance by dt seconds. power and restless are 0..1, as the web page's sliders.
void step(Engine &e, float dt, float power, float restless);

int live_count(const Engine &e);          // channels not dying
uint32_t rnd_u32(Engine &e);
float rnd(Engine &e);                      // [0, 1)
float fexpn(float x);                      // exp(x) for x <= 0, by table

inline int owner(const Engine &e, int fi, int j) {
    const Fil &f = e.fil[fi];
    return (f.par >= 0 && j <= f.k) ? f.par : fi;
}

} // namespace plasma

// The discharge. See plasma_engine.h for what changed from the web engine; the comments
// here say what each part is for, briefly. The long form, with the reasoning behind every
// constant and the eight things that were found by measuring, is in the plasma project's
// README and in index.html itself. Constants keep the original's names and values unless
// noted, so the two can be read side by side.
#include "plasma_engine.h"
#include <math.h>
#include <string.h>

namespace plasma {
namespace {

/* The circuit. The electrode sits at V0 ~ 1 with nothing drawing on it. */
constexpr float ZS = 0.36f;        // driver output impedance
constexpr float VSUS = 0.25f;      // burning voltage: a channel needs this to live
constexpr float VSTRIKE = 0.66f;   // breakdown: what it takes to start a new one
constexpr float X0 = 6.0f;         // reactance of bare glass under a foot
constexpr float RHO = 0.6f;        // channel resistance per unit length...
constexpr float N0 = 0.25f;        // ...over (N0 + ionisation squared)
constexpr float TAU_ION = 0.12f;   // how fast ionisation follows current
constexpr float IREF = 0.09f;      // a typical channel's current
constexpr float IMIN = 0.03f;      // below this for LOW_T, a channel goes out
constexpr float LOW_T = 0.25f;
constexpr float TAU_COOL = 0.35f;  // turbulent cooling: how long a gust lasts...
constexpr float COOL = 0.55f;      // ...and how far it swings ionisation

/* The glass. */
constexpr float SW = 0.14f;        // width of one patch of deposited charge
constexpr float DEP = 18.0f;       // charge laid down per unit current per second
constexpr float TAU_Q = 0.6f;      // how long it takes to leak away

/* The gas. */
constexpr float KT = 34.0f;
constexpr float KREP = 0.014f;     // like charges repel
constexpr float KB = 0.32f;        // hot gas rises
constexpr float TAU_N = 0.28f;     // turbulence: how long a gust lasts
constexpr float KPHI = 0.55f;      // how hard a foot is pushed off its own charge
constexpr float KROOT = 20.0f;     // a root leans towards where its channel lands
constexpr float SUB = 1.0f / 50;   // substep; the implicit diagonal makes this stable

/* Merging and splitting. */
constexpr float ZIPD = 0.05f;
constexpr float UNZIPD = 0.12f;
constexpr float ZIP_RATE = 12.0f;
constexpr float UNZIP_RATE = 8.0f;
constexpr float KZIP = 6.0f;
constexpr float FORK_RATE = 1.8f;

constexpr int FOOT = (NN - 1) * 3;
constexpr int MID = (NN >> 1) * 3;
constexpr int PROBE = NN >> 2;
constexpr float PI_F = 3.14159265f;

// exp(-x) for x in [0, 16], 32 steps per unit, linearly interpolated.
constexpr int EXP_N = 16 * 32;
float s_exp[EXP_N + 2];
bool s_expReady = false;

inline float gauss(Engine &e) {
    // A sum of four uniforms, rescaled to unit variance. Close enough to normal for
    // turbulence, and a fraction of the cost of Box-Muller's log and sine.
    return (rnd(e) + rnd(e) + rnd(e) + rnd(e) - 2.0f) * 1.7320508f;
}

inline float d2(const float *a, const float *b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

inline float segOf(const Engine &e, int fi, int j) { return e.fil[owner(e, fi, j)].seg[j]; }

int used_count(const Engine &e) {
    int n = 0;
    for (int i = 0; i < MAXF; ++i) n += e.fil[i].used;
    return n;
}

int alloc_slot(Engine &e) {
    for (int i = 0; i < MAXF; ++i) if (!e.fil[i].used) return i;
    return -1;
}

int make(Engine &e, float x, float y, float z) {
    const int fi = alloc_slot(e);
    if (fi < 0) return -1;
    Fil &f = e.fil[fi];
    memset(&f, 0, sizeof(Fil));
    f.used = 1;
    f.par = -1; f.k = -1;
    f.cool = gauss(e);
    f.R = 1; f.z = 1;
    f.hue = rnd(e) - 0.5f;
    for (int j = 0; j < NN; ++j) {
        f.d[j * 3] = x; f.d[j * 3 + 1] = y; f.d[j * 3 + 2] = z;
        f.ou[j * 3] = gauss(e) * 0.7f; f.ou[j * 3 + 1] = gauss(e) * 0.7f; f.ou[j * 3 + 2] = gauss(e) * 0.7f;
        f.seg[j] = 0.35f;
    }
    return fi;
}

/* Put a channel out. Its strongest branch was carrying current through the same gas,
   so it becomes the trunk, and the others hang off it instead. */
void kill(Engine &e, int fi) {
    Fil &f = e.fil[fi];
    if (f.dying) return;
    f.dying = 1;
    e.st.deaths++;
    int heir = -1;
    for (int g = 0; g < MAXF; ++g) {
        const Fil &G = e.fil[g];
        if (G.used && G.par == fi && !G.dying && (heir < 0 || G.I > e.fil[heir].I)) heir = g;
    }
    if (heir < 0) return;
    Fil &H = e.fil[heir];
    const int hk = H.k;
    for (int j = 1; j <= hk; ++j) H.seg[j] = f.seg[j];
    H.par = -1; H.k = -1;
    for (int g = 0; g < MAXF; ++g) {
        Fil &G = e.fil[g];
        if (!G.used || G.par != fi || g == heir) continue;
        for (int j = hk + 1; j <= G.k; ++j) G.seg[j] = f.seg[j];
        G.par = (int16_t)heir;
        if (G.k > hk) G.k = (int16_t)hk;
        if (G.k < 0) G.par = -1;
    }
    f.par = hk >= 0 ? (int16_t)heir : (int16_t)-1;
    f.k = (int16_t)hk;
}

} // namespace

uint32_t rnd_u32(Engine &e) {
    uint32_t x = e.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return e.rng = x;
}
float rnd(Engine &e) { return (float)(rnd_u32(e) >> 8) * (1.0f / 16777216.0f); }

float fexpn(float x) {
    float t = -x * 32.0f;
    if (t <= 0) return 1.0f;
    if (t >= (float)EXP_N) return 0.0f;
    const int i = (int)t;
    const float fr = t - (float)i;
    return s_exp[i] + (s_exp[i + 1] - s_exp[i]) * fr;
}

int live_count(const Engine &e) {
    int n = 0;
    for (int i = 0; i < MAXF; ++i) n += e.fil[i].used && !e.fil[i].dying;
    return n;
}

void init(Engine &e, float r0, uint32_t seed) {
    if (!s_expReady) {
        for (int i = 0; i <= EXP_N + 1; ++i) s_exp[i] = expf(-(float)i / 32.0f);
        s_expReady = true;
    }
    memset(&e, 0, sizeof(Engine));
    e.rng = seed ? seed : 0x9E3779B9u;
    for (int i = 0; i < MAXF; ++i) { e.fil[i].par = -1; e.fil[i].k = -1; }
    // Patches of glass on a Fibonacci sphere, so they are evenly spread.
    const float ga = PI_F * (3.0f - sqrtf(5.0f));
    for (int i = 0; i < CELLS; ++i) {
        const float y = 1.0f - 2.0f * (i + 0.5f) / CELLS, r = sqrtf(1.0f - y * y), a = i * ga;
        e.cell[i * 3] = cosf(a) * r; e.cell[i * 3 + 1] = y; e.cell[i * 3 + 2] = sinf(a) * r;
    }
    e.r0 = r0;
    for (int j = 0; j < NN; ++j) e.radii[j] = r0 + (1.0f - r0) * powf((float)j / (NN - 1), 0.9f);
}

void step(Engine &e, float dt, float power, float rest) {
    e.time += dt;
    const float V0 = 0.82f + 0.5f * power;
    const float sigma = 0.55f + 1.3f * rest;
    const float dep = DEP * (0.6f + 0.8f * rest);
    const float *radii = e.radii;
    Fil *fil = e.fil;

    /* --- each foot's footprint on the glass, the potential under it, and which way
       is downhill. Found once; deposition below reuses the same footprint. ------ */
    const float cut2 = 9.0f * SW * SW, invSW2 = 1.0f / (SW * SW);
    for (int fi = 0; fi < MAXF; ++fi) {
        Fil &f = fil[fi];
        if (!f.used) continue;
        const float fx = f.d[FOOT], fy = f.d[FOOT + 1], fz = f.d[FOOT + 2];
        float phi = 0, gx = 0, gy = 0, gz = 0;
        int n = 0;
        for (int i = 0; i < CELLS; ++i) {
            const float x = fx - e.cell[i * 3], y = fy - e.cell[i * 3 + 1], z = fz - e.cell[i * 3 + 2];
            const float r2 = x * x + y * y + z * z;
            if (r2 > cut2) continue;
            const float w = fexpn(-r2 * invSW2);
            if (n < FPMAX) { f.fpI[n] = (uint16_t)i; f.fpW[n] = w; ++n; }
            const float qw = e.q[i] * w;
            phi += qw; gx += qw * x; gy += qw * y; gz += qw * z;
        }
        f.fpN = (uint8_t)n;
        f.phi = phi;
        const float k = 2.0f * invSW2;
        f.gx = gx * k; f.gy = gy * k; f.gz = gz * k;
    }

    /* --- the circuit --------------------------------------------------------- */
    for (int fi = 0; fi < MAXF; ++fi) {
        Fil &f = fil[fi];
        if (!f.used) continue;
        float R = 0;
        for (int j = 1; j < NN; ++j) {
            const float a = radii[j - 1], b = radii[j];
            const float *p = &f.d[j * 3], *o = &f.d[j * 3 - 3];
            const float x = p[0] * b - o[0] * a, y = p[1] * b - o[1] * a, z = p[2] * b - o[2] * a;
            const float s = segOf(e, fi, j);
            R += RHO * sqrtf(x * x + y * y + z * z) / (N0 + s * s);
        }
        f.R = R;
        f.z = sqrtf(R * R + X0 * X0);
        // Turbulence acts on the burning voltage: that is what puts marginal channels out.
        f.a = VSUS * expf(-0.25f * (0.3f + rest) * f.cool) + f.phi;
    }
    float lo = 0, hi = V0;
    for (int it = 0; it < 20; ++it) {
        const float mid = 0.5f * (lo + hi);
        float draw = 0;
        for (int fi = 0; fi < MAXF; ++fi) {
            const Fil &f = fil[fi];
            if (f.used && !f.dying && mid > f.a) draw += (mid - f.a) / f.z;
        }
        if (mid - V0 + ZS * draw > 0) hi = mid; else lo = mid;
    }
    const float V = e.V = 0.5f * (lo + hi);
    for (int fi = 0; fi < MAXF; ++fi) {
        Fil &f = fil[fi];
        if (!f.used) continue;
        f.I = f.dying ? 0.0f : fmaxf(0.0f, V - f.a) / f.z;
    }

    /* --- ionisation, piece by piece: a trunk carries all its branches' current -- */
    for (int fi = 0; fi < MAXF; ++fi) if (fil[fi].used) memset(fil[fi].segI, 0, sizeof(fil[fi].segI));
    for (int fi = 0; fi < MAXF; ++fi) {
        const Fil &f = fil[fi];
        if (!f.used || f.I <= 0) continue;
        for (int j = 1; j < NN; ++j) fil[owner(e, fi, j)].segI[j] += f.I;
    }
    const float coolK = expf(-dt / TAU_COOL), coolS = sqrtf(1.0f - coolK * coolK);
    const float follow = fminf(1.0f, dt / TAU_ION);
    for (int fi = 0; fi < MAXF; ++fi) {
        Fil &f = fil[fi];
        if (!f.used) continue;
        f.cool = f.cool * coolK + gauss(e) * coolS;
        const float gust = expf(COOL * (0.3f + rest) * f.cool);
        for (int j = f.par >= 0 ? f.k + 1 : 1; j < NN; ++j)
            f.seg[j] += (fminf(6.0f, f.segI[j] / IREF) * gust - f.seg[j]) * follow;
    }

    /* --- charge on the glass ------------------------------------------------- */
    const float qk = expf(-dt / TAU_Q);
    for (int i = 0; i < CELLS; ++i) e.q[i] *= qk;
    for (int fi = 0; fi < MAXF; ++fi) {
        const Fil &f = fil[fi];
        if (!f.used || f.I <= 0 || !f.fpN) continue;
        float wsum = 0;
        for (int n = 0; n < f.fpN; ++n) wsum += f.fpW[n];
        if (wsum <= 0) continue;
        const float amt = dep * f.I * dt / wsum;
        for (int n = 0; n < f.fpN; ++n) e.q[f.fpI[n]] += amt * f.fpW[n];
    }

    /* --- going out, and merging ---------------------------------------------- */
    for (int fi = 0; fi < MAXF; ++fi) {
        Fil &f = fil[fi];
        if (!f.used) continue;
        if (f.dying) { f.life -= dt / 0.12f; continue; }
        f.life = fminf(1.0f, f.life + dt / 0.05f);
        if (f.I < IMIN) { f.low += dt; if (f.low > LOW_T) kill(e, fi); }
        else f.low = 0;
    }
    for (int a = 0; a < MAXF; ++a) {
        if (!fil[a].used || fil[a].dying) continue;
        for (int b = a + 1; b < MAXF; ++b) {
            Fil &A = fil[a], &B = fil[b];
            if (!B.used || B.dying || A.dying) continue;
            if (d2(&A.d[FOOT], &B.d[FOOT]) < 0.004f && d2(&A.d[MID], &B.d[MID]) < 0.012f) {
                const int keep = A.I >= B.I ? a : b, lose = keep == a ? b : a;
                Fil &K = fil[keep];
                for (int j = K.par >= 0 ? K.k + 1 : 1; j < NN; ++j)
                    K.seg[j] = fminf(6.0f, K.seg[j] + 0.5f * segOf(e, lose, j));
                kill(e, lose);
            }
        }
    }

    /* --- zipping up: a node at a time, from the electrode outward ---------------- */
    for (int bi = 0; bi < MAXF; ++bi) {
        Fil &B = fil[bi];
        if (!B.used || B.dying) continue;
        const int n = B.k + 1;
        if (n >= NN - 5) continue;   // not along the last stretch before the glass
        bool kids = false;
        for (int g = 0; g < MAXF; ++g)
            if (fil[g].used && fil[g].par == bi && !fil[g].dying) { kids = true; break; }
        if (kids) continue;          // trees stay one level deep
        int best = -1;
        float bestD = ZIPD * ZIPD;
        for (int ai = 0; ai < MAXF; ++ai) {
            if (ai == bi || !fil[ai].used || fil[ai].dying) continue;
            const int o = owner(e, ai, n);
            if (o == bi || fil[o].dying) continue;
            if (B.par >= 0 ? o != B.par : fil[o].par >= 0) continue;
            const float dd = d2(&B.d[n * 3], &fil[o].d[n * 3]);
            if (dd < bestD) { bestD = dd; best = o; }
        }
        if (best < 0 || rnd(e) >= dt * ZIP_RATE * (1.0f - sqrtf(bestD) / ZIPD)) continue;
        B.par = (int16_t)best;
        B.k = (int16_t)n;
        memcpy(&B.d[n * 3], &fil[best].d[n * 3], 3 * sizeof(float));
        e.st.zips++;
    }

    /* --- coming apart --------------------------------------------------------- */
    for (int bi = 0; bi < MAXF; ++bi) {
        Fil &B = fil[bi];
        if (!B.used || B.dying || B.par < 0) continue;
        const int n = B.k + 1;
        if (n >= NN) continue;
        const float sep = sqrtf(d2(&B.d[n * 3], &fil[B.par].d[n * 3]));
        if (sep <= UNZIPD || rnd(e) >= dt * UNZIP_RATE * fminf(2.0f, sep / UNZIPD - 1.0f)) continue;
        if (B.k >= 1) B.seg[B.k] = fil[B.par].seg[B.k];
        B.k--;
        if (B.k < 0) { B.par = -1; e.st.splits++; }
        else e.st.unzips++;
    }

    /* --- branching: a foot choked by its own charge breaks down sideways --------- */
    if (used_count(e) < MAXF) {
        const float room = fmaxf(1e-3f, V - VSUS);
        for (int fi = 0; fi < MAXF; ++fi) {
            Fil &f = fil[fi];
            if (!f.used || f.dying || f.life < 1) continue;
            const float choke = fminf(1.0f, f.phi / room);
            if (rnd(e) >= dt * FORK_RATE * choke * choke * (0.4f + 1.2f * rest)) continue;

            const int own = f.par >= 0 ? f.par : fi;
            int k = (int)(NN * 0.3f + rnd(e) * NN * 0.3f);
            if (f.par >= 0 && f.k < k) k = f.k;
            if (k < 1 || k > NN - 4) continue;

            const float fx = f.d[FOOT], fy = f.d[FOOT + 1], fz = f.d[FOOT + 2];
            float tx = f.gx, ty = f.gy, tz = f.gz;
            float dot = tx * fx + ty * fy + tz * fz;
            tx -= dot * fx; ty -= dot * fy; tz -= dot * fz;
            float tl = sqrtf(tx * tx + ty * ty + tz * tz);
            if (tl < 1e-4f) {
                tx = gauss(e); ty = gauss(e); tz = gauss(e);
                dot = tx * fx + ty * fy + tz * fz;
                tx -= dot * fx; ty -= dot * fy; tz -= dot * fz;
                tl = sqrtf(tx * tx + ty * ty + tz * tz);
                if (tl <= 0) tl = 1;
            }
            tx /= tl; ty /= tl; tz /= tl;
            const float side = (rnd(e) - 0.5f) * 1.4f, cs = cosf(side), sn = sinf(side);
            const float bx = fy * tz - fz * ty, by = fz * tx - fx * tz, bz = fx * ty - fy * tx;
            const float ang = 0.22f + rnd(e) * 0.25f;
            const float ux = (tx * cs + bx * sn) * ang, uy = (ty * cs + by * sn) * ang, uz = (tz * cs + bz * sn) * ang;

            const int ci = make(e, 0, 0, 0);
            if (ci < 0) break;
            Fil &c = fil[ci];
            c.par = (int16_t)own;
            c.k = (int16_t)k;
            c.hue = f.hue + (rnd(e) - 0.5f) * 0.1f;
            memcpy(c.d, f.d, sizeof(c.d));
            for (int j = k + 1; j < NN; ++j) {
                const float t = powf((float)(j - k) / (float)(NN - 1 - k), 1.5f);
                const float x = f.d[j * 3] + ux * t, y = f.d[j * 3 + 1] + uy * t, z = f.d[j * 3 + 2] + uz * t;
                float L = sqrtf(x * x + y * y + z * z);
                if (L <= 0) L = 1;
                c.d[j * 3] = x / L; c.d[j * 3 + 1] = y / L; c.d[j * 3 + 2] = z / L;
                // a branch starts warm: started cold, most went out before carrying anything
                c.seg[j] = 0.6f * segOf(e, fi, j);
            }
            e.st.forks++;
            if (used_count(e) >= MAXF) break;
        }
    }

    for (int fi = 0; fi < MAXF; ++fi) {
        Fil &f = fil[fi];
        if (!f.used || !(f.dying && f.life <= 0)) continue;
        for (int g = 0; g < MAXF; ++g) if (fil[g].used && fil[g].par == fi) { fil[g].par = -1; fil[g].k = -1; }
        f.used = 0;
    }

    /* --- striking: wherever the field is over breakdown, all together ------------ */
    if (e.time >= e.next) {
        float Vest = V;
        int struck = 0;
        while (used_count(e) < MAXF && struck < 8) {
            float best[3] = {0, 0, 0}, bestM = 0;
            bool have = false;
            for (int c = 0; c < 18; ++c) {
                const float y = rnd(e) * 2 - 1, r = sqrtf(fmaxf(0.0f, 1 - y * y)), a = rnd(e) * 6.2831853f;
                const float cx = r * cosf(a), cz = r * sinf(a);
                // space-charge shielding: short-range, or new strands stop forming at all
                float crowd = 0;
                for (int fi = 0; fi < MAXF; ++fi) {
                    const Fil &f = fil[fi];
                    if (!f.used || f.dying || (f.par >= 0 && PROBE <= f.k)) continue;
                    const float *p = &f.d[PROBE * 3];
                    const float x = cx - p[0], yy = y - p[1], z = cz - p[2];
                    crowd += fexpn(-(x * x + yy * yy + z * z) * (1.0f / 0.06f));
                }
                const float vth = VSTRIKE * (1 + 0.15f * crowd);
                const float m = Vest - vth + rnd(e) * 0.02f;
                if (m > bestM) { bestM = m; best[0] = cx; best[1] = y; best[2] = cz; have = true; }
            }
            if (!have) break;
            float L = sqrtf(best[0] * best[0] + best[1] * best[1] + best[2] * best[2]);
            if (L <= 0) L = 1;
            if (make(e, best[0] / L, best[1] / L, best[2] / L) < 0) break;
            e.st.strikes++;
            struck++;
            Vest -= ZS * fmaxf(0.0f, Vest - VSUS) / X0;
        }
        if (struck) e.next = e.time + 0.045f;
    }

    /* --- moving --------------------------------------------------------------- */
    /* Repulsion once a step and held through the substeps: the most expensive force,
       and a slow one. Within a tree it fades in over the first nodes past a fork. */
    for (int fi = 0; fi < MAXF; ++fi) {
        Fil &f = fil[fi];
        if (!f.used) continue;
        const int tree = f.par >= 0 ? f.par : fi, kf = f.par >= 0 ? f.k : NN;
        for (int j = f.par >= 0 ? f.k + 1 : 0; j < NN; ++j) {
            const int i = j * 3;
            const float x = f.d[i], y = f.d[i + 1], z = f.d[i + 2];
            const float shell = 1.0f / fmaxf(0.09f, radii[j] * radii[j]);
            float Rx = 0, Ry = 0, Rz = 0;
            for (int gi = 0; gi < MAXF; ++gi) {
                const Fil &g = fil[gi];
                if (gi == fi || !g.used || g.life <= 0 || (g.par >= 0 && j <= g.k)) continue;
                float ramp = 1;
                if ((g.par >= 0 ? g.par : gi) == tree) {
                    const int m = kf < (g.par >= 0 ? g.k : NN) ? kf : (g.par >= 0 ? g.k : NN);
                    if (j <= m) continue;
                    ramp = fminf(1.0f, (float)(j - m) / 4.0f);
                }
                const float ex = x - g.d[i], ey = y - g.d[i + 1], ez = z - g.d[i + 2];
                const float k = ramp * KREP * g.life * shell / (ex * ex + ey * ey + ez * ez + 0.003f);
                Rx += k * ex; Ry += k * ey; Rz += k * ez;
            }
            f.rep[i] = Rx; f.rep[i + 1] = Ry; f.rep[i + 2] = Rz;
        }
    }

    const int nsub = dt > SUB ? (int)ceilf(dt / SUB) : 1;
    const float h = dt / nsub;
    const float ouK = expf(-h / TAU_N), ouS = sqrtf(1.0f - ouK * ouK);
    float midW[NN];
    for (int j = 0; j < NN; ++j) midW[j] = sinf(PI_F * j / (NN - 1));
    for (int s = 0; s < nsub; ++s) {
        for (int fi = 0; fi < MAXF; ++fi) if (fil[fi].used) {
            memset(fil[fi].ex, 0, sizeof(fil[fi].ex));
            memset(fil[fi].exW, 0, sizeof(fil[fi].exW));
        }
        // A branch pulls on the trunk node it hangs from, as a neighbour would.
        for (int ci = 0; ci < MAXF; ++ci) {
            const Fil &c = fil[ci];
            if (!c.used || c.par < 0 || c.k + 1 >= NN) continue;
            const int k = c.k, i = (k + 1) * 3;
            Fil &P = fil[c.par];
            const float w = KT * (1 + 0.5f * fminf(4.0f, c.segI[k + 1] / IREF)) * (0.3f + radii[k + 1]);
            P.ex[k * 3] += w * c.d[i]; P.ex[k * 3 + 1] += w * c.d[i + 1]; P.ex[k * 3 + 2] += w * c.d[i + 2];
            P.exW[k] += w;
        }

        for (int fi = 0; fi < MAXF; ++fi) {
            Fil &f = fil[fi];
            if (!f.used) continue;
            float *d = f.d, *ou = f.ou;
            for (int j = f.par >= 0 ? f.k + 1 : 0; j < NN; ++j) {
                const int i = j * 3;
                const float x = d[i], y = d[i + 1], z = d[i + 2];
                // Tension (and a branch's pull on its trunk) is the stiff part, and is
                // stepped implicitly on its own diagonal. Everything else is explicit, so a
                // channel drifting as a whole moves at full speed.
                float Tx = 0, Ty = 0, Tz = 0, W = 0;

                // more current: straighter, and lifted, but only so far
                const float c = fminf(4.0f, f.segI[j > 0 ? j : 1] / IREF);
                const float stiff = 1 + 0.5f * c;
                if (j > 0) {
                    const float w = KT * stiff * (0.3f + radii[j]);
                    Tx += w * (d[i - 3] - x); Ty += w * (d[i - 2] - y); Tz += w * (d[i - 1] - z);
                    W += w;
                }
                if (j < NN - 1) {
                    const float w = KT * stiff * (0.3f + radii[j + 1]);
                    Tx += w * (d[i + 3] - x); Ty += w * (d[i + 4] - y); Tz += w * (d[i + 5] - z);
                    W += w;
                }
                if (f.exW[j] > 0) {
                    Tx += f.ex[i] - f.exW[j] * x; Ty += f.ex[i + 1] - f.exW[j] * y; Tz += f.ex[i + 2] - f.exW[j] * z;
                    W += f.exW[j];
                }
                const float damp = 1.0f / (1.0f + h * W);
                float Fx = f.rep[i] + Tx * damp, Fy = f.rep[i + 1] + Ty * damp, Fz = f.rep[i + 2] + Tz * damp;

                // hot gas rises, not at the held ends
                const float mid = midW[j];
                Fy += KB * mid * fminf(1.2f, c);

                // turbulence: a slowly varying push, strongest mid-channel
                ou[i] = ou[i] * ouK + gauss(e) * ouS;
                ou[i + 1] = ou[i + 1] * ouK + gauss(e) * ouS;
                ou[i + 2] = ou[i + 2] * ouK + gauss(e) * ouS;
                const float t = sigma * (0.3f + 0.7f * mid);
                Fx += t * ou[i]; Fy += t * ou[i + 1]; Fz += t * ou[i + 2];

                // a fork holds its branch in close past the join, up to a point
                if (f.par >= 0 && j == f.k + 1) {
                    const Fil &P = fil[f.par];
                    const float kz = KZIP * (0.3f + fminf(1.5f, P.seg[f.k > 1 ? f.k : 1]));
                    Fx += kz * (P.d[i] - x); Fy += kz * (P.d[i + 1] - y); Fz += kz * (P.d[i + 2] - z);
                }
                if (j == 0) {
                    // a root leaves the electrode facing where its channel lands
                    Fx += KROOT * (d[FOOT] - x); Fy += KROOT * (d[FOOT + 1] - y); Fz += KROOT * (d[FOOT + 2] - z);
                }
                if (j == NN - 1) {
                    // the foot slides off its own charge
                    Fx += KPHI * f.gx; Fy += KPHI * f.gy; Fz += KPHI * f.gz;
                }

                // along the shell only, then back onto it
                const float dot = Fx * x + Fy * y + Fz * z;
                const float nx = x + h * (Fx - dot * x), ny = y + h * (Fy - dot * y), nz = z + h * (Fz - dot * z);
                float L = sqrtf(nx * nx + ny * ny + nz * nz);
                if (L <= 0) L = 1;
                const float il = 1.0f / L;
                d[i] = nx * il; d[i + 1] = ny * il; d[i + 2] = nz * il;
            }
        }

        // the shared nodes are the parent's: copy them back
        for (int ci = 0; ci < MAXF; ++ci) {
            Fil &c = fil[ci];
            if (!c.used || c.par < 0) continue;
            memcpy(c.d, fil[c.par].d, (size_t)(c.k + 1) * 3 * sizeof(float));
        }
    }

    /* --- what the renderer reads ----------------------------------------------- */
    for (int fi = 0; fi < MAXF; ++fi) {
        Fil &f = fil[fi];
        if (!f.used) continue;
        if (e.time - f.trailAt >= 0.045f) {
            f.trailAt = e.time;
            memmove(&f.trail[3], &f.trail[0], (TRAILN - 1) * 3 * sizeof(float));
            f.trail[0] = f.d[FOOT]; f.trail[1] = f.d[FOOT + 1]; f.trail[2] = f.d[FOOT + 2];
            if (f.trailN < TRAILN) f.trailN++;
        }
    }
}

} // namespace plasma

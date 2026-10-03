// Runs the Plasma screen's own discharge (src/plasma_engine.cpp) and its renderer
// (src/plasma_render.cpp) off the device, and writes frames as binary PPM, for the Plasma
// theme's artwork. build.py compiles and runs this; nothing here is used by the firmware.
//
//   render_plasma <out prefix> <seed> <power 0..1> <warm seconds> <frames> <seconds between>
//
// Writes <prefix>NN.ppm, 233 x 233 (the renderer's own grid), one per frame. The same seed
// always gives the same pictures: the engine's only randomness is its own generator.
#include "../../src/plasma_engine.h"
#include "../../src/plasma_render.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: %s <prefix> <seed> <power> <warm s> <frames> <gap s>\n", argv[0]);
        return 2;
    }
    const char *prefix = argv[1];
    const uint32_t seed = (uint32_t)strtoul(argv[2], nullptr, 0);
    const float power = (float)atof(argv[3]);
    const float warm = (float)atof(argv[4]);
    const int frames = atoi(argv[5]);
    const float gap = (float)atof(argv[6]);

    // The view's own constants (plasma_view.cpp): electrode size, hue, restlessness, and
    // the thirtieth-of-a-second step the engine is tuned for.
    const float R0 = 0.17f, HUE = 288.0f, RESTLESS = 0.5f, DT = 1.0f / 30;

    plasma::Engine *eng = new plasma::Engine();
    plasma::Render ren = {};
    if (!plasma::render_alloc(ren, HUE, R0)) { fprintf(stderr, "render_alloc failed\n"); return 1; }
    ren.ring = -1;                       // no Power readout on the glass
    ren.rng = seed ^ 0x9E3779B9u;        // the renderer's dither and fibres, pinned too
    plasma::init(*eng, R0, seed);

    for (float t = 0; t < warm; t += DT) plasma::step(*eng, DT, power, RESTLESS);

    std::vector<unsigned char> rgb((size_t)plasma::RW * plasma::RW * 3);
    for (int f = 0; f < frames; ++f) {
        if (f) for (float t = 0; t < gap - 1e-6f; t += DT) plasma::step(*eng, DT, power, RESTLESS);
        // Twice: the renderer clears its accumulator on the way past, and the brush fibres
        // carry state from the frame before, so the second pass is the settled picture.
        plasma::render_frame(ren, *eng);
        plasma::render_frame(ren, *eng);
        for (int i = 0; i < plasma::RW * plasma::RW; ++i) {
            const uint16_t v = ren.out[i];
            const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            rgb[i * 3]     = (unsigned char)((r << 3) | (r >> 2));
            rgb[i * 3 + 1] = (unsigned char)((g << 2) | (g >> 4));
            rgb[i * 3 + 2] = (unsigned char)((b << 3) | (b >> 2));
        }
        char path[512];
        snprintf(path, sizeof(path), "%s%02d.ppm", prefix, f);
        FILE *fp = fopen(path, "wb");
        if (!fp) { perror(path); return 1; }
        fprintf(fp, "P6\n%d %d\n255\n", plasma::RW, plasma::RW);
        fwrite(rgb.data(), 1, rgb.size(), fp);
        fclose(fp);
    }
    fprintf(stderr, "%d frame(s), %d channels live at the end\n", frames, plasma::live_count(*eng));
    plasma::render_free(ren);
    delete eng;
    return 0;
}

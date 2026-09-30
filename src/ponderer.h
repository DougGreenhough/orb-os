#pragma once
// orb-ponderer: the relay on spiritdemon.net that does what this board cannot (HTTPS,
// OAuth, image resizing) and hands the result back over plain HTTP. The device API is
// documented in the orb-ponderer repo, docs/DEVICE-API.md.
//
// Two halves, the same split every other networked screen here uses:
//   - net side  (core 0 on the device, a background thread in the simulator): may block
//     on the network. Each module's netStep() owns its own timing and returns at once
//     when nothing is due.
//   - UI side   (core 1 / the LVGL loop): uiApply() runs after a netStep() said it had
//     something new. Only this side may touch LVGL objects.
#include <stddef.h>
#include <stdint.h>

namespace ponderer {
    // Base URL (ends in '/') and device key, from ponderer_config.h; in the simulator the
    // ORB_PONDERER_URL / ORB_PONDERER_KEY environment variables win.
    const char *base_url();
    const char *key();
    bool        configured();   // both present

    // GET <base>device/?fn=<fn>&k=<key>[&<extra>]. `extra` must already be URL-safe.
    // On success *body is owned by the caller and must go back through release().
    // Returns false on any transport failure or a non-200 answer.
    bool get(const char *fn, const char *extra, uint8_t **body, size_t *len,
             size_t maxLen, int timeoutMs = 8000);
    void release(uint8_t *body);

    // ORB5 images: "ORB5", uint16 LE width, uint16 LE height, then width*height RGB565
    // pixels, uint16 little-endian, row-major. Returns the pixels inside `body`, or null
    // when the header is wrong or the body is short.
    const uint16_t *orb5_pixels(const uint8_t *body, size_t len, int *w, int *h);

    struct Module {
        const char *name;
        bool (*netStep)();   // net side; true = new data for the UI
        void (*uiApply)();   // UI side; may be null
    };
    void add(const Module &m);   // before either tick runs; up to 12 modules

    void net_tick();   // call every pass of the network loop
    void ui_tick();    // call every pass of the UI loop
}

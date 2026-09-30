// orb-ponderer client. See ponderer.h.
#include "ponderer.h"
#include "ponderer_config.h"
#include "net_fetch.h"
#include "config.h"
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

namespace {

constexpr int MAX_MODULES = 12;
ponderer::Module s_mods[MAX_MODULES];
std::atomic<bool> s_dirty[MAX_MODULES];
int s_count = 0;

const char *env_or(const char *name, const char *fallback) {
#ifndef ARDUINO
    const char *v = getenv(name);
    if (v && v[0]) return v;
#else
    (void)name;
#endif
    return fallback;
}

} // namespace

namespace ponderer {

const char *base_url() { return env_or("ORB_PONDERER_URL", PONDERER_URL); }
const char *key()      { return env_or("ORB_PONDERER_KEY", PONDERER_KEY); }
bool configured()      { return base_url()[0] && key()[0]; }

bool get(const char *fn, const char *extra, uint8_t **body, size_t *len,
         size_t maxLen, int timeoutMs) {
    *body = nullptr; *len = 0;
    if (!configured()) return false;
    char url[512];
    const int n = snprintf(url, sizeof(url), "%sdevice/?fn=%s&k=%s%s%s", base_url(), fn, key(),
                           (extra && extra[0]) ? "&" : "", (extra && extra[0]) ? extra : "");
    if (n <= 0 || n >= (int)sizeof(url)) return false;
    const bool ok = net_fetch_psram(url, ORB_USER_AGENT, body, len, maxLen, 3500, timeoutMs);
    if (!ok) printf("[ponderer] %s failed\n", fn);   // the URL carries the key, so it is not logged
    return ok;
}

void release(uint8_t *body) {
    if (!body) return;
#ifdef ARDUINO
    heap_caps_free(body);
#else
    free(body);
#endif
}

const uint16_t *orb5_pixels(const uint8_t *body, size_t len, int *w, int *h) {
    if (!body || len < 8 || memcmp(body, "ORB5", 4) != 0) return nullptr;
    const int ww = body[4] | (body[5] << 8);
    const int hh = body[6] | (body[7] << 8);
    if (ww <= 0 || hh <= 0 || len < 8 + (size_t)ww * hh * 2) return nullptr;
    *w = ww; *h = hh;
    return (const uint16_t *)(body + 8);   // both targets are little-endian, as is the format
}

void add(const Module &m) {
    if (s_count >= MAX_MODULES) { printf("[ponderer] too many modules, %s dropped\n", m.name); return; }
    s_dirty[s_count] = false;
    s_mods[s_count++] = m;
}

void net_tick() {
    for (int i = 0; i < s_count; ++i)
        if (s_mods[i].netStep && s_mods[i].netStep()) s_dirty[i] = true;
}

void ui_tick() {
    for (int i = 0; i < s_count; ++i)
        if (s_dirty[i].exchange(false) && s_mods[i].uiApply) s_mods[i].uiApply();
}

} // namespace ponderer

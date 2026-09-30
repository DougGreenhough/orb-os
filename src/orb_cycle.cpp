// Screen cycle. See orb_cycle.h.
#include "orb_cycle.h"
#include "config.h"
#include "app_shell.h"
#include "ponderer.h"
#include <ArduinoJson.h>
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl.h>
#ifdef ARDUINO
#include <Preferences.h>
#endif

namespace {

constexpr int MAX_APPS = 16;
constexpr uint32_t REFRESH_MS = 5UL * 60UL * 1000UL;

struct Settings {
    bool     on = ORB_CYCLE_DEFAULT_ON;
    uint32_t secs = ORB_CYCLE_DEFAULT_SECS;
    uint32_t resume = ORB_CYCLE_DEFAULT_RESUME;
    char     apps[MAX_APPS][25] = {};   // app names, as app_shell knows them
    int      nApps = 0;
};

Settings s_cur;                 // UI side only
bool     s_ready = false;       // first tick has run (the roster is complete by then)
bool     s_envOverride = false; // ORB_CYCLE set in the simulator: ignore the relay
uint16_t s_mask = 0;            // chosen apps, as app_shell indices
int      s_lastApp = -1;
uint32_t s_enteredMs = 0;       // when the current app came in
std::atomic<uint32_t> s_lastInputMs{0};
std::atomic<bool>     s_hadInput{false};

// Net side -> UI side.
std::mutex s_lock;
Settings   s_pending;
bool       s_hasPending = false;
uint32_t   s_nextFetchMs = 0;
char       s_appList[400] = "";   // "Clock,Flight%20Tracker,..." for the relay, built on the UI side

// Parse {"on":..,"secs":..,"resume":..,"apps":[..]} into `out`. Returns false on junk.
bool parse_cycle(JsonVariantConst c, Settings &out) {
    if (c.isNull()) return false;
    out = Settings();
    out.on = c["on"] | false;
    out.secs = c["secs"] | (int)ORB_CYCLE_DEFAULT_SECS;
    out.resume = c["resume"] | (int)ORB_CYCLE_DEFAULT_RESUME;
    for (JsonVariantConst a : c["apps"].as<JsonArrayConst>()) {
        if (out.nApps >= MAX_APPS) break;
        snprintf(out.apps[out.nApps++], sizeof(out.apps[0]), "%s", a.as<const char *>() ? a.as<const char *>() : "");
    }
    return true;
}

// The last settings the relay gave, kept so the cycle works after a reboot with no network.
void save_cached(const char *json) {
#ifdef ARDUINO
    Preferences p;
    p.begin("capsuleradar", false);
    p.putString("orb_cycle", json);
    p.end();
#else
    if (FILE *f = fopen("/tmp/orb_sim_cycle.json", "w")) { fputs(json, f); fclose(f); }
#endif
}

bool load_cached(Settings &out) {
    char buf[600] = "";
#ifdef ARDUINO
    Preferences p;
    p.begin("capsuleradar", true);
    String s = p.getString("orb_cycle", "");
    p.end();
    snprintf(buf, sizeof(buf), "%s", s.c_str());
#else
    if (FILE *f = fopen("/tmp/orb_sim_cycle.json", "r")) { size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f); }
#endif
    if (!buf[0]) return false;
    JsonDocument doc;
    return !deserializeJson(doc, buf) && parse_cycle(doc.as<JsonVariantConst>(), out);
}

#ifndef ARDUINO
// ORB_CYCLE="secs=8;resume=20;apps=Clock,Facts,Plasma" (simulator only).
bool load_env(Settings &out) {
    const char *e = getenv("ORB_CYCLE");
    if (!e || !e[0]) return false;
    out = Settings();
    out.on = true;
    char buf[400]; snprintf(buf, sizeof(buf), "%s", e);
    for (char *part = strtok(buf, ";"); part; part = strtok(nullptr, ";")) {
        if (!strncmp(part, "secs=", 5)) out.secs = (uint32_t)atoi(part + 5);
        else if (!strncmp(part, "resume=", 7)) out.resume = (uint32_t)atoi(part + 7);
        else if (!strncmp(part, "apps=", 5)) {
            char *save = nullptr;
            for (char *a = strtok_r(part + 5, ",", &save); a && out.nApps < MAX_APPS; a = strtok_r(nullptr, ",", &save))
                snprintf(out.apps[out.nApps++], sizeof(out.apps[0]), "%s", a);
        }
    }
    return true;
}
#endif

void apply(const Settings &s) {
    s_cur = s;
    s_mask = 0;
    for (int i = 0; i < app_shell::count() && i < 16; ++i)
        for (int j = 0; j < s.nApps; ++j)
            if (!strcmp(app_shell::nameAt(i), s.apps[j]) && !app_shell::hiddenAt(i)) s_mask |= (uint16_t)(1u << i);
    printf("[cycle] %s, %us each, resume after %us, %d app(s)\n",
           s.on && s_mask ? "on" : "off", (unsigned)s.secs, (unsigned)s.resume, __builtin_popcount(s_mask));
}

// ---- relay ----------------------------------------------------------------------

bool net_step() {
    if (s_envOverride || !ponderer::configured()) return false;
    const uint32_t now = lv_tick_get();
    if (s_nextFetchMs && (int32_t)(now - s_nextFetchMs) < 0) return false;
    char extra[420];
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (!s_appList[0]) return false;   // the UI side hasn't listed the apps yet
        snprintf(extra, sizeof(extra), "apps=%s", s_appList);
    }
    uint8_t *body = nullptr; size_t len = 0;
    const bool ok = ponderer::get("config", extra, &body, &len, 4096);
    s_nextFetchMs = now + (ok ? REFRESH_MS : 60000UL);
    if (!ok) return false;
    JsonDocument doc;
    Settings s;
    const bool parsed = !deserializeJson(doc, (const char *)body, len) && parse_cycle(doc["cycle"], s);
    if (parsed) {
        char json[600];
        serializeJson(doc["cycle"], json, sizeof(json));
        save_cached(json);
    }
    ponderer::release(body);
    if (!parsed) return false;
    std::lock_guard<std::mutex> g(s_lock);
    s_pending = s;
    s_hasPending = true;
    return true;
}

void ui_apply() {
    Settings s;
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (!s_hasPending) return;
        s = s_pending;
        s_hasPending = false;
    }
    apply(s);
}

// Percent-encode app names for the query string (they contain spaces).
void build_app_list() {
    std::lock_guard<std::mutex> g(s_lock);
    size_t w = 0;
    for (int i = 0; i < app_shell::count(); ++i) {
        if (app_shell::hiddenAt(i)) continue;
        for (const char *c = (i && w ? "," : ""); *c && w + 1 < sizeof(s_appList); ++c) s_appList[w++] = *c;
        for (const char *c = app_shell::nameAt(i); *c && w + 4 < sizeof(s_appList); ++c) {
            if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.') {
                s_appList[w++] = *c;
            } else {
                w += (size_t)snprintf(s_appList + w, 4, "%%%02X", (unsigned char)*c);
            }
        }
    }
    s_appList[w] = 0;
}

void first_tick(uint32_t nowMs) {
    s_ready = true;
    Settings s;
#ifndef ARDUINO
    s_envOverride = load_env(s);
#endif
    if (!s_envOverride && !load_cached(s)) {
        // Compiled defaults: ORB_CYCLE_DEFAULT_APPS is a comma list of names.
        char buf[200]; snprintf(buf, sizeof(buf), "%s", ORB_CYCLE_DEFAULT_APPS);
        char *save = nullptr;
        for (char *a = strtok_r(buf, ",", &save); a && s.nApps < MAX_APPS; a = strtok_r(nullptr, ",", &save))
            snprintf(s.apps[s.nApps++], sizeof(s.apps[0]), "%s", a);
    }
    apply(s);
    build_app_list();
    ponderer::add({ "cycle", net_step, ui_apply });
    s_lastApp = app_shell::index();
    s_enteredMs = nowMs;
}

// The next chosen app after `from`, or -1.
int next_chosen(int from) {
    const int n = app_shell::count();
    for (int step = 1; step <= n; ++step) {
        const int i = (from + step) % n;
        if (s_mask & (1u << i)) return i;
    }
    return -1;
}

} // namespace

namespace orb_cycle {

void noteInput() {
    s_lastInputMs = lv_tick_get();
    s_hadInput = true;
}

void tick(uint32_t nowMs) {
    if (!s_ready) { if (app_shell::count()) first_tick(nowMs); return; }

    const int cur = app_shell::index();
    if (cur != s_lastApp) { s_lastApp = cur; s_enteredMs = nowMs; }
    if (!s_cur.on || !s_mask || app_shell::browsing()) return;

    // Paused by a person: wait until they've left it alone for `resume` seconds.
    if (s_hadInput) {
        if (nowMs - s_lastInputMs.load() < s_cur.resume * 1000UL) return;
        s_hadInput = false;
        if (!(s_mask & (1u << cur))) {       // they left it somewhere outside the loop
            const int n = next_chosen(cur);
            if (n >= 0) app_shell::goTo(n);
            return;
        }
        s_enteredMs = nowMs;                  // a full turn on the screen they left it on
        return;
    }

    if (nowMs - s_enteredMs < s_cur.secs * 1000UL) return;
    const int n = next_chosen(cur);
    if (n >= 0 && n != cur) app_shell::goTo(n);
    else s_enteredMs = nowMs;                 // only one app chosen: nothing to move to
}

} // namespace orb_cycle

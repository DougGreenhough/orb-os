// Themes over WiFi from orb-ponderer. See orb_themes.h.
#include "orb_themes.h"

#ifdef __EMSCRIPTEN__
namespace orb_themes { void init() {} }   // the browser's card is in memory; see the header
#else

#include "ponderer.h"
#include "theme_select.h"
#include "theme_sd.h"
#include "update_ui.h"
#include <ArduinoJson.h>
#include <lvgl.h>
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include "sdcard.h"
#else
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr uint32_t FIRST_CHECK_MS = 20000;            // let the Orb settle after boot
constexpr uint32_t CHECK_MS       = 10UL * 60 * 1000;
constexpr uint32_t RETRY_MS       = 60000;
constexpr size_t   MANIFEST_MAX   = 96 * 1024;
constexpr size_t   FILE_MAX       = 4 * 1024 * 1024;   // the relay's own ceiling
constexpr int      MAX_JOBS       = 16 * 80;
constexpr const char *RECORD      = "_ponderer.json";   // marks a folder as installed by this module
constexpr const char *REV_PATH    = "/themes/_ponderer_rev";
constexpr const char *PENDING     = "/themes/_ponderer_remove";   // slugs to delete once they are no longer worn

struct Job { char slug[theme_select::MAX_SLUG_LEN]; char name[41]; uint32_t bytes; bool lastOfTheme; };

// IDLE -> (net) manifest -> PLAN (ui) -> FETCH (net) <-> WRITE (ui) -> FINISH (ui) -> IDLE
enum State { IDLE, PLAN, FETCH, WRITE, FINISH };
std::atomic<State> s_state{IDLE};
std::mutex s_lock;                 // guards everything below that both sides touch

char    *s_manifest = nullptr;     // the manifest text, net -> ui
size_t   s_manifestLen = 0;
uint8_t *s_file = nullptr;         // one fetched file, net -> ui (released with ponderer::release)
size_t   s_fileLen = 0;

Job     *s_jobs = nullptr;
int      s_jobN = 0, s_jobAt = 0;
uint32_t s_bytesAll = 0, s_bytesDone = 0;
char     s_rev[20] = "";           // the revision being installed
char     s_active[theme_select::MAX_SLUG_LEN] = "";
char     s_cardRev[20] = "";       // what the card says it holds; read on the UI side at init
bool     s_changed = false;
bool     s_panel = false;          // the update panel is up
uint32_t s_nextCheck = 0;
int      s_tries = 0;

void *big_alloc(size_t n) {
#ifdef ARDUINO
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}
void big_free(void *p) {
#ifdef ARDUINO
    if (p) heap_caps_free(p);
#else
    free(p);
#endif
}

// ---- the card (UI side only) --------------------------------------------------------

#ifndef ARDUINO
std::string host(const char *path) { return std::string("sim/sdcard") + path; }
#endif

bool card_ready() {
#ifdef ARDUINO
    return sdcard::mounted();
#else
    struct stat st;
    return stat("sim/sdcard", &st) == 0;
#endif
}

void card_mkdir(const char *path) {
#ifdef ARDUINO
    if (!SD.exists(path)) SD.mkdir(path);
#else
    mkdir(host(path).c_str(), 0755);
#endif
}

// Written beside the real name and renamed once every byte is down, so a write that dies
// partway leaves the file the card already had.
bool card_write(const char *path, const uint8_t *data, size_t len) {
    char part[160];
    snprintf(part, sizeof(part), "%s.part", path);
#ifdef ARDUINO
    theme_sd::lock();
    SD.remove(part);
    File f = SD.open(part, FILE_WRITE);
    bool ok = false;
    if (f) {
        ok = f.write(data, len) == len;
        f.close();
    }
    if (ok) { SD.remove(path); ok = SD.rename(part, path); }
    if (!ok) SD.remove(part);
    theme_sd::unlock();
    return ok;
#else
    FILE *f = fopen(host(part).c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    if (!ok) { ::remove(host(part).c_str()); return false; }
    return rename(host(part).c_str(), host(path).c_str()) == 0;
#endif
}

void card_remove(const char *path) {
#ifdef ARDUINO
    theme_sd::lock();
    SD.remove(path);
    theme_sd::unlock();
#else
    ::remove(host(path).c_str());
#endif
}

// A small text file from the card into `out`. False when it isn't there.
bool card_read_text(const char *path, char *out, size_t cap) {
    size_t len = 0;
    uint8_t *b = theme_sd::read_whole(path, len, cap - 1);
    if (!b) return false;
    memcpy(out, b, len);
    out[len] = 0;
    theme_sd::free(b);
    return true;
}

// What this module last wrote for a theme: {"files":{"name":"sha",...}}. False when the
// folder isn't one of ours.
bool card_record(const char *slug, JsonDocument &out) {
    char path[96];
    snprintf(path, sizeof(path), "/themes/%s/%s", slug, RECORD);
    size_t len = 0;
    uint8_t *b = theme_sd::read_whole(path, len, 32 * 1024);
    if (!b) return false;
    const DeserializationError err = deserializeJson(out, b, len);
    theme_sd::free(b);
    return !err;
}

// ---- UI side --------------------------------------------------------------------------

void finish_idle(uint32_t after) {
    if (s_panel) { update_ui::bake_done(); s_panel = false; }
    s_nextCheck = lv_tick_get() + after;
    s_state = IDLE;
}

void fail(const char *why) {
    printf("[themes] %s; will try again\n", why);
    finish_idle(RETRY_MS);
}

// Work out what to fetch from the manifest and what the card already holds.
void plan() {
    char *text; size_t len;
    { std::lock_guard<std::mutex> g(s_lock); text = s_manifest; len = s_manifestLen; s_manifest = nullptr; }
    if (!text) { finish_idle(RETRY_MS); return; }
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, text, len);
    big_free(text);
    if (err) { fail("the theme list would not parse"); return; }
    if (!card_ready()) { fail("no SD card"); return; }

    snprintf(s_rev, sizeof(s_rev), "%s", (const char *)(doc["rev"] | ""));
    snprintf(s_active, sizeof(s_active), "%s", (const char *)(doc["active"] | ""));
    if (!s_jobs) s_jobs = (Job *)big_alloc(sizeof(Job) * MAX_JOBS);
    if (!s_jobs) { fail("out of memory"); return; }

    s_jobN = s_jobAt = 0; s_bytesAll = s_bytesDone = 0; s_changed = false;
    for (JsonObjectConst t : doc["themes"].as<JsonArrayConst>()) {
        const char *slug = t["slug"] | "";
        if (!slug[0] || strlen(slug) >= theme_select::MAX_SLUG_LEN) continue;
        JsonDocument have;
        const bool known = card_record(slug, have);
        const int first = s_jobN;
        for (JsonObjectConst f : t["files"].as<JsonArrayConst>()) {
            const char *name = f["n"] | "";
            const char *sha  = f["s"] | "";
            if (!name[0] || strlen(name) > 40) continue;
            if (known && !strcmp(have["files"][name] | "", sha)) continue;   // the card has this exact file
            if (s_jobN >= MAX_JOBS) break;
            Job &j = s_jobs[s_jobN++];
            snprintf(j.slug, sizeof(j.slug), "%s", slug);
            snprintf(j.name, sizeof(j.name), "%s", name);
            j.bytes = f["b"] | 0u;
            j.lastOfTheme = false;
            s_bytesAll += j.bytes;
        }
        if (s_jobN > first) s_jobs[s_jobN - 1].lastOfTheme = true;
    }

    // Keep the manifest for FINISH (the records and the removals are written from it).
    { std::lock_guard<std::mutex> g(s_lock);
      big_free(s_manifest);
      const size_t n = measureJson(doc) + 1;
      s_manifest = (char *)big_alloc(n);
      if (s_manifest) { serializeJson(doc, s_manifest, n); s_manifestLen = n - 1; } }

    printf("[themes] revision %s: %d file(s) to fetch (%lu KB), wear '%s'\n",
           s_rev, s_jobN, (unsigned long)(s_bytesAll / 1024), s_active);
    if (!s_jobN) { s_state = FINISH; return; }
    update_ui::file_received("your themes, from your relay", 0);
    s_panel = true;
    s_tries = 0;
    s_state = FETCH;
}

// Record what a theme folder now holds, from the manifest, so the next pull can skip it.
void write_record(const char *slug, JsonObjectConst theme) {
    JsonDocument rec;
    JsonObject files = rec["files"].to<JsonObject>();
    for (JsonObjectConst f : theme["files"].as<JsonArrayConst>()) files[(const char *)(f["n"] | "")] = (const char *)(f["s"] | "");
    char buf[8192];
    const size_t n = serializeJson(rec, buf, sizeof(buf));
    char path[96];
    snprintf(path, sizeof(path), "/themes/%s/%s", slug, RECORD);
    if (n && n < sizeof(buf)) card_write(path, (const uint8_t *)buf, n);
}

void write_file() {
    uint8_t *data; size_t len;
    { std::lock_guard<std::mutex> g(s_lock); data = s_file; len = s_fileLen; s_file = nullptr; }
    if (!data) { fail("a file went missing on the way to the card"); return; }
    const Job &j = s_jobs[s_jobAt];
    char dir[64], path[128];
    snprintf(dir, sizeof(dir), "/themes/%s", j.slug);
    snprintf(path, sizeof(path), "%s/%s", dir, j.name);
    card_mkdir("/themes");
    card_mkdir(dir);
    const bool ok = card_write(path, data, len);
    ponderer::release(data);
    if (!ok) { fail("the card would not take a file"); return; }
    s_bytesDone += j.bytes;
    s_changed = true;
    ++s_jobAt;
    update_ui::file_received(j.name, s_jobAt);
    s_tries = 0;
    s_state = s_jobAt >= s_jobN ? FINISH : FETCH;
}

void finish() {
    char *text; size_t len;
    { std::lock_guard<std::mutex> g(s_lock); text = s_manifest; len = s_manifestLen; s_manifest = nullptr; }
    JsonDocument doc;
    if (!text || deserializeJson(doc, text, len)) { big_free(text); fail("lost the theme list"); return; }
    big_free(text);
    JsonArrayConst themes = doc["themes"].as<JsonArrayConst>();

    for (JsonObjectConst t : themes) write_record(t["slug"] | "", t);

    // Remove what we installed and the list no longer names. Never a folder without our
    // record (someone put it there another way), never the one being worn right now.
    static char onCard[theme_select::MAX_THEMES][theme_select::MAX_SLUG_LEN];
    const int nCard = theme_select::listInstalled(onCard);
    for (int i = 0; i < nCard; ++i) {
        bool listed = false;
        for (JsonObjectConst t : themes) if (!strcmp(onCard[i], t["slug"] | "")) { listed = true; break; }
        if (listed) continue;
        JsonDocument rec;
        if (!card_record(onCard[i], rec)) continue;
        if (!strcmp(onCard[i], theme_select::activeSlug())) {
            // One of ours, taken off the list while it is being worn. It cannot be deleted
            // from under the screens drawing it, so: back to the stock look (or the theme
            // the page chose) with a restart, and WITHOUT recording this revision, so the
            // pull runs again after the restart and removes the folder then.
            printf("[themes] '%s' was taken off the list while worn; restarting without it\n", onCard[i]);
            // Stop it counting as a theme (an Orb with nothing chosen wears the first
            // installed one, which would be this again), and note it for deletion.
            char path[96];
            snprintf(path, sizeof(path), "/themes/%s/_installed", onCard[i]);
            card_remove(path);
            card_write(PENDING, (const uint8_t *)onCard[i], strlen(onCard[i]));
            s_state = IDLE;
            if (s_panel) { update_ui::bake_done(); s_panel = false; }
            theme_select::set(s_active);   // "" = stock
            return;
        }
        if (theme_select::removeInstalled(onCard[i])) { s_changed = true; printf("[themes] removed '%s'\n", onCard[i]); }
    }

    // A theme marked for deletion on the last pass, now that nothing is drawing from it.
    char pending[theme_select::MAX_SLUG_LEN + 2];
    if (card_read_text(PENDING, pending, sizeof(pending))) {
        for (char *c = pending; *c; ++c) if (*c == '\n' || *c == '\r') { *c = 0; break; }
        if (pending[0] && strcmp(pending, theme_select::activeSlug()) != 0 && theme_select::removeInstalled(pending))
            printf("[themes] removed '%s'\n", pending);
        card_remove(PENDING);
    }

    card_mkdir("/themes");
    card_write(REV_PATH, (const uint8_t *)s_rev, strlen(s_rev));
    snprintf(s_cardRev, sizeof(s_cardRev), "%s", s_rev);

    // Wear what the setup page chose: when it names a theme and either this is not the one
    // being worn, or its files have just changed underneath it (a restart re-bakes them).
    const bool wearing = !strcmp(s_active, theme_select::activeSlug());
    if (s_active[0] && (!wearing || s_changed)) {
        static char slugs[theme_select::MAX_THEMES][theme_select::MAX_SLUG_LEN];
        const int n = theme_select::listInstalled(slugs);
        for (int i = 0; i < n; ++i) {
            if (strcmp(slugs[i], s_active) != 0) continue;
            printf("[themes] done; restarting into '%s'\n", s_active);
            s_state = IDLE;
            theme_select::set(s_active);   // persists, then restarts; does not return on the device
            return;
        }
        printf("[themes] '%s' is not installed after the pull; staying as we are\n", s_active);
    }
    printf("[themes] done, revision %s\n", s_rev);
    finish_idle(CHECK_MS);
}

void ui_apply() {
    switch (s_state.load()) {
        case PLAN:   plan(); break;
        case WRITE:  write_file(); break;
        case FINISH: finish(); break;
        default: break;
    }
}

// ---- network side -----------------------------------------------------------------------

bool net_step() {
    switch (s_state.load()) {
        case IDLE: {
            if (!ponderer::configured()) return false;
            const uint32_t now = lv_tick_get();
            if ((int32_t)(now - s_nextCheck) < 0) return false;
            s_nextCheck = now + RETRY_MS;
            char extra[40];
            snprintf(extra, sizeof(extra), "have=%s", s_cardRev);
            uint8_t *body = nullptr; size_t len = 0;
            if (!ponderer::get("themes.manifest", extra, &body, &len, MANIFEST_MAX)) return false;
            // {"rev":"...","same":true}: nothing has changed since the card's revision.
            if (len < 120 && memmem(body, len, "\"same\":true", 11)) {
                ponderer::release(body);
                s_nextCheck = now + CHECK_MS;
                return false;
            }
            char *copy = (char *)big_alloc(len + 1);
            if (copy) { memcpy(copy, body, len); copy[len] = 0; }
            ponderer::release(body);
            if (!copy) return false;
            { std::lock_guard<std::mutex> g(s_lock); big_free(s_manifest); s_manifest = copy; s_manifestLen = len; }
            s_state = PLAN;
            return true;
        }
        case FETCH: {
            const Job &j = s_jobs[s_jobAt];
            char extra[120];
            snprintf(extra, sizeof(extra), "slug=%s&f=%s", j.slug, j.name);
            uint8_t *body = nullptr; size_t len = 0;
            const bool ok = j.bytes <= FILE_MAX
                && ponderer::get("themes.file", extra, &body, &len, (size_t)j.bytes + 16, 30000)
                && len == j.bytes;
            if (!ok) {
                ponderer::release(body);
                // Two tries a file: a WiFi hiccup is not a reason to abandon the whole pull.
                if (++s_tries < 2) return false;
                { std::lock_guard<std::mutex> g(s_lock); big_free(s_manifest); s_manifest = nullptr; }
                s_state = FINISH;   // the UI side finds no manifest, takes the panel down and retries later
                return true;
            }
            { std::lock_guard<std::mutex> g(s_lock); s_file = body; s_fileLen = len; }
            s_state = WRITE;
            return true;
        }
        // PLAN, WRITE and FINISH belong to the UI side. Keep asking for it until it has run,
        // so a state set from the UI side (WRITE -> FETCH -> WRITE ...) is never left waiting.
        case PLAN: case WRITE: case FINISH: return true;
    }
    return false;
}

} // namespace

namespace orb_themes {

void init() {
    static bool done = false;
    if (done) return;
    done = true;
    // What the card holds, so an unchanged list costs one small request and nothing else.
    // A different card, or a wiped one, has no such file and gets a full pull.
    if (!card_read_text(REV_PATH, s_cardRev, sizeof(s_cardRev))) s_cardRev[0] = 0;
    for (char *c = s_cardRev; *c; ++c) if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) { *c = 0; break; }
    uint32_t first = FIRST_CHECK_MS;
#ifndef ARDUINO
    if (const char *e = getenv("ORB_THEMES_FIRST_MS")) first = (uint32_t)atoi(e);
#endif
    s_nextCheck = lv_tick_get() + first;
    ponderer::add({ "themes", net_step, ui_apply });
}

} // namespace orb_themes

#endif  // !__EMSCRIPTEN__

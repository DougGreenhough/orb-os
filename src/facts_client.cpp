// Facts, the network half. See facts_client.h.
#include "facts_client.h"
#include "ponderer.h"
#include <ArduinoJson.h>
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <string.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#else
#include <chrono>
#endif

namespace {

std::mutex s_mu;
facts::Fact s_next = {};             // the prefetched fact
bool s_haveNext = false;
char s_notId[48] = "";               // the fact on screen, which the next ask must not repeat
facts::Status s_status = facts::ST_WAITING;
std::atomic<uint32_t> s_nextTryMs{0};  // when the net side may ask again
std::atomic<int> s_fails{0};         // consecutive failures, for the backoff
std::atomic<bool> s_showing{false};
void (*s_apply)() = nullptr;

uint32_t now_ms() {
#ifdef ARDUINO
    return millis();
#else
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

bool due(uint32_t at) { return (int32_t)(now_ms() - at) >= 0; }

// The relay folds to ASCII already. This is the belt to its braces: anything the font cannot
// draw is dropped rather than drawn as a box, and runs of whitespace become one space.
void clean_copy(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    bool space = true;   // swallows leading spaces too
    for (const char *p = src ? src : ""; *p && n + 1 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        if (c < 0x20 || c > 0x7E) continue;
        if (c == ' ') { if (space) continue; space = true; }
        else space = false;
        dst[n++] = (char)c;
    }
    while (n && dst[n - 1] == ' ') --n;
    dst[n] = 0;
}

// Only the characters an id can reasonably use pass unescaped.
void url_encode(char *dst, size_t cap, const char *src) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const char *p = src; *p && n + 4 < cap; ++p) {
        const unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[n++] = (char)c;
        } else {
            dst[n++] = '%'; dst[n++] = hex[c >> 4]; dst[n++] = hex[c & 15];
        }
    }
    dst[n] = 0;
}

// Why did fn=fact fail? The contract has no "no facts" answer of its own (any non-200 is
// "try later"), but hello counts them, so one more small request tells "the relay has
// nothing for you" apart from "the relay is not there".
facts::Status diagnose() {
    uint8_t *body = nullptr; size_t len = 0;
    if (!ponderer::get("hello", nullptr, &body, &len, 4096)) return facts::ST_UNREACHABLE;
    JsonDocument doc;
    facts::Status st = facts::ST_RELAY_ERROR;
    if (!deserializeJson(doc, (const char *)body, len) && doc["facts"].is<int>() &&
        doc["facts"].as<int>() == 0)
        st = facts::ST_EMPTY;
    ponderer::release(body);
    return st;
}

// Back off on failure: 5 s, 10 s, 20 s... up to two minutes while showing, ten while not.
void schedule_retry() {
    const int fails = ++s_fails;
    uint32_t wait = 5000u << (fails > 6 ? 6 : fails - 1);
    const uint32_t cap = s_showing.load() ? 120000u : 600000u;
    if (wait > cap) wait = cap;
    s_nextTryMs = now_ms() + wait;
}

bool set_status(facts::Status st) {
    std::lock_guard<std::mutex> lock(s_mu);
    if (s_status == st) return false;
    s_status = st;
    return true;
}

bool net_step() {
    if (!ponderer::configured()) return set_status(facts::ST_UNCONFIGURED);

    char notId[48];
    {
        std::lock_guard<std::mutex> lock(s_mu);
        if (s_haveNext) return false;   // one in hand is all anyone needs
        memcpy(notId, s_notId, sizeof(notId));
    }
    if (!due(s_nextTryMs)) return false;

#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) {
        s_nextTryMs = now_ms() + 5000;   // no backoff: the radio coming back is cheap to notice
        return set_status(facts::ST_OFFLINE);
    }
#endif

    char extra[4 + 3 * sizeof(notId)] = "";
    if (notId[0]) {
        memcpy(extra, "not=", 4);
        url_encode(extra + 4, sizeof(extra) - 4, notId);
    }

    uint8_t *body = nullptr; size_t len = 0;
    if (!ponderer::get("fact", extra, &body, &len, 4096)) {
        const facts::Status st = diagnose();
        schedule_retry();
        return set_status(st);
    }

    // Heap-allocated Fact would do too; this is ~310 bytes of a 7 KB stack, well clear.
    facts::Fact f = {};
    bool ok = false;
    {
        JsonDocument filter;
        filter["id"] = true; filter["text"] = true; filter["topic"] = true; filter["source"] = true;
        JsonDocument doc;
        if (!deserializeJson(doc, (const char *)body, len, DeserializationOption::Filter(filter))) {
            clean_copy(f.id, sizeof(f.id), doc["id"] | "");
            clean_copy(f.text, sizeof(f.text), doc["text"] | "");
            clean_copy(f.topic, sizeof(f.topic), doc["topic"] | "");
            clean_copy(f.source, sizeof(f.source), doc["source"] | "");
            ok = f.text[0] != 0;
        }
    }
    ponderer::release(body);

    if (!ok) {
        // A 200 with no words in it: the relay is up and has nothing it will say.
        schedule_retry();
        return set_status(facts::ST_EMPTY);
    }
    s_fails = 0;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        s_next = f;
        s_haveNext = true;
        s_status = facts::ST_OK;
    }
    return true;
}

void ui_apply() { if (s_apply) s_apply(); }

}  // namespace

namespace facts {

void start(void (*uiApply)()) {
    static bool started = false;
    if (started) return;
    started = true;
    s_apply = uiApply;
    ponderer::add({ "facts", net_step, ui_apply });
}

void setShowing(bool showing) {
    s_showing = showing;
    // Coming back to the screen is a good moment to stop waiting out a long hidden backoff.
    if (showing && s_fails) s_nextTryMs = now_ms();
}

bool take(Fact &out) {
    std::lock_guard<std::mutex> lock(s_mu);
    if (!s_haveNext) return false;
    out = s_next;
    s_haveNext = false;
    memcpy(s_notId, out.id, sizeof(s_notId));
    return true;
}

Status status() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_status;
}

}  // namespace facts

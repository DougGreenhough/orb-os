// Music's network half. See music_client.h.
#include "music_client.h"
#include "ponderer.h"
#include <ArduinoJson.h>
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <string.h>
#ifdef ARDUINO
#include <Arduino.h>
static uint32_t now_ms() { return millis(); }
#else
#include <chrono>
#include <math.h>
#include <stdlib.h>
static uint32_t now_ms() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif

namespace {

constexpr uint32_t POLL_MS       = 3000;   // DEVICE-API: at most every 3 s while showing
constexpr uint32_t AFTER_CMD_MS  = 400;    // Spotify takes a moment to reflect a command
constexpr uint32_t RETRY_MS      = 5000;   // after a failure, a little gentler
constexpr size_t   NOW_MAX       = 8192;   // the contract says under 4 KB
constexpr int      FAILS_TO_SHOW = 2;      // one dropped poll is weather, two is an outage
constexpr int      QUEUE_LEN     = 8;

using music::Now;

std::mutex s_mu;                  // guards everything in this block
Now        s_pub = {};            // the latest answer, for the UI
bool       s_pubNew = false;
uint8_t   *s_artBody = nullptr;   // pending cover, not yet taken by the UI
char       s_artId[sizeof(Now::artId)] = "";
int        s_artW = 0, s_artH = 0;
bool       s_showing = false;     // written under s_mu so the art hand-off can check it
struct QCmd { music::Cmd c; int arg; uint32_t seq; };
QCmd       s_q[QUEUE_LEN];
int        s_qLen = 0;

std::atomic<bool>     s_showingFlag{false};   // lock-free read for the net fast path
std::atomic<bool>     s_pollNow{false};
std::atomic<bool>     s_forgetArt{false};     // re-entry: the UI freed its cover, fetch again
std::atomic<bool>     s_cmdFailed{false};
std::atomic<uint32_t> s_seqQueued{0};
std::atomic<uint32_t> s_seqSent{0};

// Net-private. Static rather than on the task's stack: a Now is ~400 bytes.
Now      s_scratch;
char     s_haveArt[sizeof(Now::artId)] = "";   // the id of the last cover handed over
uint32_t s_nextPoll = 0;
int      s_fails = 0;
music::Status s_lastStatus = music::ST_WAITING;

bool due(uint32_t at) { return (int32_t)(now_ms() - at) >= 0; }

// The relay folds to ASCII already; this is the belt to that promise's braces, because the
// fonts have no fallback glyph and one stray byte draws as an empty box.
void copy_ascii(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (src)
        for (const unsigned char *p = (const unsigned char *)src; *p && n + 1 < cap; ++p)
            if (*p >= 0x20 && *p < 0x7F) dst[n++] = (char)*p;
    dst[n] = 0;
}

// Query-string safe, whatever the relay chooses to use as an art id.
void url_encode(char *dst, size_t cap, const char *src) {
    static const char *hex = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && n + 4 < cap; ++p) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            dst[n++] = (char)*p;
        } else {
            dst[n++] = '%'; dst[n++] = hex[*p >> 4]; dst[n++] = hex[*p & 15];
        }
    }
    dst[n] = 0;
}

void publish(const Now &n) {
    std::lock_guard<std::mutex> lock(s_mu);
    s_pub = n;
    s_pubNew = true;
}

// net: send every queued command, oldest first.
bool send_commands() {
    bool any = false;
    for (;;) {
        QCmd q;
        {
            std::lock_guard<std::mutex> lock(s_mu);
            if (!s_qLen) break;
            q = s_q[0];
            memmove(s_q, s_q + 1, sizeof(QCmd) * (size_t)(--s_qLen));
        }
        char extra[40];
        switch (q.c) {
        case music::CMD_TOGGLE: snprintf(extra, sizeof(extra), "c=toggle"); break;
        case music::CMD_NEXT:   snprintf(extra, sizeof(extra), "c=next"); break;
        case music::CMD_PREV:   snprintf(extra, sizeof(extra), "c=prev"); break;
        case music::CMD_VOL:    snprintf(extra, sizeof(extra), "c=vol&v=%d", q.arg); break;
        }
        uint8_t *body = nullptr; size_t len = 0;
        const bool ok = ponderer::get("spotify.cmd", extra, &body, &len, 1024, 5000);
        ponderer::release(body);   // {"ok":true}; the status code already said so
        if (!ok) s_cmdFailed = true;
        s_seqSent = q.seq;
        any = true;
    }
    if (any) s_nextPoll = now_ms() + AFTER_CMD_MS;
    return any;
}

// net: one spotify.now into s_scratch. False on transport failure or a body that is not
// the JSON the contract promises.
bool poll_now() {
    s_scratch = Now{};
    s_scratch.volume = -1;
    s_scratch.cmdSeq = s_seqSent;
    uint8_t *body = nullptr; size_t len = 0;
    if (!ponderer::get("spotify.now", nullptr, &body, &len, NOW_MAX, 5000)) return false;
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, (const char *)body, len);
    ponderer::release(body);
    if (err) { printf("[music] spotify.now: %s\n", err.c_str()); return false; }

    if (!(doc["linked"] | false)) { s_scratch.status = music::ST_UNLINKED; return true; }
    if (!(doc["active"] | false)) { s_scratch.status = music::ST_IDLE; return true; }
    s_scratch.status     = music::ST_ACTIVE;
    s_scratch.playing    = doc["playing"] | false;
    s_scratch.progressMs = doc["progress_ms"] | 0;
    s_scratch.durationMs = doc["duration_ms"] | 0;
    s_scratch.volume     = doc["volume"] | -1;
    s_scratch.canVolume  = doc["can_volume"] | false;
    copy_ascii(s_scratch.title,  sizeof(s_scratch.title),  doc["title"]  | "");
    copy_ascii(s_scratch.artist, sizeof(s_scratch.artist), doc["artist"] | "");
    copy_ascii(s_scratch.device, sizeof(s_scratch.device), doc["device"] | "");
    copy_ascii(s_scratch.artId,  sizeof(s_scratch.artId),  doc["art"]    | "");
    return true;
}

// net: fetch the cover for s_scratch.artId and park it for the UI. Returns true when a new
// cover was parked.
bool fetch_art() {
    char extra[200], id[160];
    url_encode(id, sizeof(id), s_scratch.artId);
    snprintf(extra, sizeof(extra), "id=%s&s=%d", id, music::ART_PX);
    uint8_t *body = nullptr; size_t len = 0;
    // Room for the largest cover the contract allows (s <= 300), so a relay that ignores s=
    // still gives a usable picture (centred and clipped) instead of a refetch every poll.
    const size_t maxLen = 8 + 300 * 300 * 2;
    if (!ponderer::get("spotify.art", extra, &body, &len, maxLen, 8000)) return false;
    int w = 0, h = 0;
    if (!ponderer::orb5_pixels(body, len, &w, &h) || w > 300 || h > 300) {
        printf("[music] spotify.art: not an ORB5 image (%u bytes)\n", (unsigned)len);
        ponderer::release(body);
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    if (!s_showing) { ponderer::release(body); return false; }   // left while we fetched
    ponderer::release(s_artBody);                                // superseded, never taken
    s_artBody = body;
    s_artW = w; s_artH = h;
    copy_ascii(s_artId, sizeof(s_artId), s_scratch.artId);
    return true;
}

#ifndef ARDUINO
// ---- SIMULATOR ONLY: a canned Spotify, for drawing the screen without one ---------------
// ORB_MUSIC_FAKE=playing|paused|idle skips the relay altogether and feeds the UI a made-up
// track and a cover computed here (a gradient with a few rings), so every state of the
// screen can be captured. Commands from the knob are applied to the canned state. Nothing
// of this exists in a device build.
bool fake_step(const char *mode) {
    static bool inited = false, playing = false;
    static int volume = 45;
    static int32_t progress = 83000;
    static uint32_t at = 0;
    if (!inited) { inited = true; playing = !strcmp(mode, "playing"); at = now_ms(); }
    bool fresh = false;
    for (;;) {
        QCmd q;
        {
            std::lock_guard<std::mutex> lock(s_mu);
            if (!s_qLen) break;
            q = s_q[0];
            memmove(s_q, s_q + 1, sizeof(QCmd) * (size_t)(--s_qLen));
        }
        if (q.c == music::CMD_TOGGLE) playing = !playing;
        if (q.c == music::CMD_VOL) volume = q.arg;
        if (q.c == music::CMD_NEXT || q.c == music::CMD_PREV) progress = 0;
        s_seqSent = q.seq;
        fresh = true;
    }
    if (!s_showingFlag) return false;
    if (s_forgetArt.exchange(false)) s_haveArt[0] = 0;
    if (s_pollNow.exchange(false) || fresh) s_nextPoll = now_ms();
    if (!due(s_nextPoll)) return false;
    s_nextPoll = now_ms() + POLL_MS;
    if (playing) progress += (int32_t)(now_ms() - at);
    at = now_ms();

    s_scratch = Now{};
    s_scratch.volume = -1;
    s_scratch.cmdSeq = s_seqSent;
    if (!strcmp(mode, "idle")) { s_scratch.status = music::ST_IDLE; publish(s_scratch); return true; }
    s_scratch.status = music::ST_ACTIVE;
    s_scratch.playing = playing;
    s_scratch.progressMs = progress % 247000;
    s_scratch.durationMs = 247000;
    s_scratch.volume = volume;
    s_scratch.canVolume = true;
    copy_ascii(s_scratch.title, sizeof(s_scratch.title), "Wichita Lineman");
    copy_ascii(s_scratch.artist, sizeof(s_scratch.artist), "Glen Campbell");
    copy_ascii(s_scratch.device, sizeof(s_scratch.device), "Kitchen speaker");
    copy_ascii(s_scratch.artId, sizeof(s_scratch.artId), "fake-cover");
    publish(s_scratch);

    if (strcmp(s_haveArt, s_scratch.artId) != 0) {
        const int n = music::ART_PX;
        uint8_t *body = (uint8_t *)malloc(8 + (size_t)n * n * 2);   // what ponderer::release() frees
        if (!body) return true;
        memcpy(body, "ORB5", 4);
        body[4] = n & 255; body[5] = n >> 8; body[6] = n & 255; body[7] = n >> 8;
        uint16_t *px = (uint16_t *)(body + 8);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                // A dusk sky over a sun: warm to cool top to bottom, rings round a point.
                const float u = (float)x / n, v = (float)y / n;
                const float d = sqrtf((u - 0.62f) * (u - 0.62f) + (v - 0.4f) * (v - 0.4f));
                const float ring = 0.5f + 0.5f * cosf(d * 34.0f);
                float r = 0.95f - 0.75f * v, g = 0.35f + 0.25f * u - 0.2f * v, b = 0.25f + 0.65f * v;
                const float k = d < 0.16f ? 1.0f : 0.72f + 0.28f * ring * (1.0f - d);
                if (d < 0.16f) { r = 1.0f; g = 0.86f; b = 0.55f; }
                auto c = [&](float f, int max) { f *= k; return (int)((f < 0 ? 0 : f > 1 ? 1 : f) * max); };
                px[y * n + x] = (uint16_t)((c(r, 31) << 11) | (c(g, 63) << 5) | c(b, 31));
            }
        std::lock_guard<std::mutex> lock(s_mu);
        if (!s_showing) { free(body); return true; }
        ponderer::release(s_artBody);
        s_artBody = body;
        s_artW = s_artH = n;
        copy_ascii(s_artId, sizeof(s_artId), s_scratch.artId);
        copy_ascii(s_haveArt, sizeof(s_haveArt), s_scratch.artId);
    }
    return true;
}
#endif

bool net_step() {
#ifndef ARDUINO
    if (const char *fake = getenv("ORB_MUSIC_FAKE")) return fake_step(fake);
#endif
    if (!ponderer::configured()) return false;   // the UI says so without our help
    // Commands go out even after the screen has been left: a volume change still in the
    // debounce when the app switcher was rocked open is something the person asked for.
    bool fresh = send_commands();
    if (!s_showingFlag) return false;
    if (s_forgetArt.exchange(false)) s_haveArt[0] = 0;
    if (s_cmdFailed) fresh = true;

    if (s_pollNow.exchange(false)) s_nextPoll = now_ms();
    if (!due(s_nextPoll)) return fresh;

    if (!poll_now()) {
        ++s_fails;
        s_nextPoll = now_ms() + RETRY_MS;
        // A single failure keeps what is on screen (the progress keeps interpolating);
        // only a run of them, or failing before anything ever arrived, is an outage.
        if (s_fails >= FAILS_TO_SHOW || s_lastStatus == music::ST_WAITING ||
            s_lastStatus == music::ST_UNREACHABLE) {
            s_scratch = Now{};
            s_scratch.status = music::ST_UNREACHABLE;
            s_scratch.volume = -1;
            s_scratch.cmdSeq = s_seqSent;
            s_lastStatus = music::ST_UNREACHABLE;
            publish(s_scratch);
            return true;
        }
        return fresh;
    }
    s_fails = 0;
    s_nextPoll = now_ms() + POLL_MS;
    s_lastStatus = s_scratch.status;
    publish(s_scratch);

    // Art only when the id changes. A failed fetch leaves s_haveArt alone, so the next poll
    // tries again rather than giving up on that cover for the length of the song.
    if (s_scratch.status == music::ST_ACTIVE && s_scratch.artId[0] &&
        strcmp(s_scratch.artId, s_haveArt) != 0) {
        if (fetch_art()) copy_ascii(s_haveArt, sizeof(s_haveArt), s_scratch.artId);
    }
    return true;
}

}  // namespace

namespace music {

void begin(void (*uiApply)()) {
    ponderer::add({ "music", net_step, uiApply });
}

void setShowing(bool on) {
    {
        std::lock_guard<std::mutex> lock(s_mu);
        s_showing = on;
        if (!on) {
            ponderer::release(s_artBody);
            s_artBody = nullptr;
            s_artId[0] = 0;
        } else {
            s_pubNew = false;
            s_pub = Now{};
        }
    }
    if (on) {
        s_forgetArt = true;
        s_pollNow = true;
        s_cmdFailed = false;
    }
    s_showingFlag = on;
}

void pollSoon() { s_pollNow = true; }

bool takeNow(Now &out) {
    std::lock_guard<std::mutex> lock(s_mu);
    if (!s_pubNew) return false;
    out = s_pub;
    s_pubNew = false;
    return true;
}

bool takeArt(uint8_t **body, const uint16_t **px, int *w, int *h, char *id, size_t idLen) {
    std::lock_guard<std::mutex> lock(s_mu);
    if (!s_artBody) return false;
    *body = s_artBody;
    *px = (const uint16_t *)(s_artBody + 8);   // validated by orb5_pixels() on arrival
    *w = s_artW; *h = s_artH;
    snprintf(id, idLen, "%s", s_artId);
    s_artBody = nullptr;
    return true;
}

uint32_t queue(Cmd c, int arg) {
    const uint32_t seq = ++s_seqQueued;
    std::lock_guard<std::mutex> lock(s_mu);
    // A second volume change before the first went out replaces it rather than queueing
    // behind it: only the last value means anything.
    if (c == CMD_VOL)
        for (int i = 0; i < s_qLen; ++i)
            if (s_q[i].c == CMD_VOL) { s_q[i].arg = arg; s_q[i].seq = seq; return seq; }
    if (s_qLen < QUEUE_LEN) s_q[s_qLen++] = { c, arg, seq };
    return seq;
}

bool takeCmdFailed() { return s_cmdFailed.exchange(false); }

}  // namespace music

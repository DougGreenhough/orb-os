// orb-ponderer client. See ponderer.h.
//
// This board cannot do TLS (see intel_client.cpp), so the transport carries its own
// encryption, the "Transport" section of orb-ponderer's docs/DEVICE-API.md:
//
//   key       "op2_<16 hex id>_<43 base64url chars = 32-byte secret>"
//   request   GET <base>device/?v=2&i=<id>&r=<base64url(nonce24 | ciphertext | mac16)>
//             plaintext "fn=<fn>&t=<unix time>&n=<16 hex>[&<extra>]", AD "op2|<id>"
//   response  "ORB2" | nonce24 | ciphertext | mac16, AD "op2|<id>|<request nonce24>"
//             plaintext is one type byte ('J' JSON, 'B' bytes) then the payload
//
// XChaCha20-Poly1305 and keyed BLAKE2b from Monocypher (lib/monocypher), the same
// code on the device and in the simulator. Per-direction keys come from the secret,
// so the secret itself never goes on the wire. Decryption is in place, in the
// buffer net_fetch already filled, so a photo costs no second copy.
#include "ponderer.h"
#include "ponderer_config.h"
#include "net_fetch.h"
#include "config.h"
#include <monocypher.h>
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#include <esp_random.h>
#endif

namespace {

constexpr int MAX_MODULES = 12;
ponderer::Module s_mods[MAX_MODULES];
std::atomic<bool> s_dirty[MAX_MODULES];
int s_count = 0;

constexpr size_t NONCE = 24, MAC = 16, MAGIC = 4;
constexpr size_t OVERHEAD = MAGIC + NONCE + MAC + 1;   // + the type byte

// The parsed key. Filled once, on first use.
bool    s_keyOk = false;
char    s_id[17];
uint8_t s_kreq[32], s_kresp[32];

// get() only ever runs on the network side, but a capture script in the simulator
// can call net_tick() from the UI thread too; one lock makes the static buffers safe.
std::mutex s_lock;
char    s_plain[512];
uint8_t s_sealed[NONCE + sizeof(s_plain) + MAC];
char    s_url[1024];

const char *env_or(const char *name, const char *fallback) {
#ifndef ARDUINO
    const char *v = getenv(name);
    if (v && v[0]) return v;
#else
    (void)name;
#endif
    return fallback;
}

void random_bytes(uint8_t *out, size_t n) {
#ifdef ARDUINO
    esp_fill_random(out, n);   // hardware RNG; true random while the radio is on
#else
    arc4random_buf(out, n);
#endif
}

int b64u_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// Unpadded base64url. Returns bytes written, or -1 on a bad character or overflow.
int b64u_decode(const char *s, size_t n, uint8_t *out, size_t cap) {
    uint32_t acc = 0; int bits = 0; size_t w = 0;
    for (size_t i = 0; i < n; ++i) {
        const int v = b64u_val(s[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (w >= cap) return -1;
            out[w++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)w;
}

size_t b64u_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t w = 0; uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        acc = (acc << 8) | in[i]; bits += 8;
        while (bits >= 6) { bits -= 6; if (w + 1 >= cap) return 0; out[w++] = A[(acc >> bits) & 63]; }
    }
    if (bits > 0) { if (w + 1 >= cap) return 0; out[w++] = A[(acc << (6 - bits)) & 63]; }
    out[w] = 0;
    return w;
}

void kdf(const uint8_t secret[32], const char *label, uint8_t out[32]) {
    char msg[48];
    const int n = snprintf(msg, sizeof(msg), "orb-ponderer v2 %s", label);
    crypto_blake2b_keyed(out, 32, secret, 32, (const uint8_t *)msg, (size_t)n);
}

std::once_flag s_keyOnce;

bool load_key_once() {
    const char *k = env_or("ORB_PONDERER_KEY", PONDERER_KEY);
    if (!k) return false;
    // op2_ + 16 hex + _ + 43 base64url
    if (strlen(k) != 4 + 16 + 1 + 43 || strncmp(k, "op2_", 4) != 0 || k[20] != '_') {
        if (k[0]) printf("[ponderer] key is not in the op2_ format; make a new one on the setup page\n");
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        const char c = k[4 + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    uint8_t secret[33];
    if (b64u_decode(k + 21, 43, secret, sizeof(secret)) != 32) return false;
    memcpy(s_id, k + 4, 16); s_id[16] = 0;
    kdf(secret, "request", s_kreq);
    kdf(secret, "response", s_kresp);
    crypto_wipe(secret, sizeof(secret));
    return s_keyOk = true;
}

// Called from both the UI (configured()) and the network side, so parse exactly once.
bool load_key() {
    std::call_once(s_keyOnce, [] { load_key_once(); });
    return s_keyOk;
}

} // namespace

namespace ponderer {

const char *base_url() { return env_or("ORB_PONDERER_URL", PONDERER_URL); }
const char *key()      { return env_or("ORB_PONDERER_KEY", PONDERER_KEY); }
bool configured()      { return base_url()[0] && load_key(); }

bool get(const char *fn, const char *extra, uint8_t **body, size_t *len,
         size_t maxLen, int timeoutMs) {
    *body = nullptr; *len = 0;
    if (!configured()) return false;

    // The server refuses a request more than five minutes off its clock, which is
    // what stops a recorded request being played back later. Before NTP has run the
    // clock says 1970, and asking would only be refused.
    const time_t now = time(nullptr);
    if (now < 1700000000L) {
        static bool said = false;
        if (!said) { printf("[ponderer] clock not set yet; waiting for NTP\n"); said = true; }
        return false;
    }

    std::lock_guard<std::mutex> g(s_lock);

    uint8_t n8[8]; random_bytes(n8, sizeof(n8));
    const int pn = snprintf(s_plain, sizeof(s_plain),
                            "fn=%s&t=%ld&n=%02x%02x%02x%02x%02x%02x%02x%02x%s%s",
                            fn, (long)now, n8[0], n8[1], n8[2], n8[3], n8[4], n8[5], n8[6], n8[7],
                            (extra && extra[0]) ? "&" : "", (extra && extra[0]) ? extra : "");
    if (pn <= 0 || pn >= (int)sizeof(s_plain)) return false;

    char ad[32];
    const int adn = snprintf(ad, sizeof(ad), "op2|%s", s_id);
    uint8_t *nonce = s_sealed, *ct = s_sealed + NONCE, *mac = ct + pn;
    random_bytes(nonce, NONCE);
    crypto_aead_lock(ct, mac, s_kreq, nonce, (const uint8_t *)ad, (size_t)adn, (const uint8_t *)s_plain, (size_t)pn);
    crypto_wipe(s_plain, sizeof(s_plain));

    const int un = snprintf(s_url, sizeof(s_url), "%sdevice/?v=2&i=%s&r=", base_url(), s_id);
    if (un <= 0 || un >= (int)sizeof(s_url)) return false;
    if (!b64u_encode(s_sealed, NONCE + pn + MAC, s_url + un, sizeof(s_url) - un)) return false;

    uint8_t *b = nullptr; size_t n = 0;
    if (!net_fetch_psram(s_url, ORB_USER_AGENT, &b, &n, maxLen + OVERHEAD, 3500, timeoutMs)) {
        printf("[ponderer] %s failed\n", fn);
        return false;
    }
    if (n < OVERHEAD || memcmp(b, "ORB2", 4) != 0) {
        printf("[ponderer] %s: not an encrypted answer\n", fn);
        release(b);
        return false;
    }

    // The answer is bound to this request: its AD carries our request nonce.
    uint8_t rad[4 + 16 + 1 + NONCE];
    memcpy(rad, "op2|", 4); memcpy(rad + 4, s_id, 16); rad[20] = '|'; memcpy(rad + 21, nonce, NONCE);
    uint8_t *rnonce = b + MAGIC, *rct = b + MAGIC + NONCE;
    const size_t rlen = n - MAGIC - NONCE - MAC;
    if (crypto_aead_unlock(rct, b + n - MAC, s_kresp, rnonce, rad, sizeof(rad), rct, rlen) != 0) {
        printf("[ponderer] %s: answer failed to authenticate\n", fn);
        release(b);
        return false;
    }
    // rct[0] is the type byte ('J' or 'B'); callers already know which they asked for.
    memmove(b, rct + 1, rlen - 1);
    *body = b;
    *len = rlen - 1;
    return true;
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

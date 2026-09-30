// Facts, the typesetting. See facts_layout.h.
#include "facts_layout.h"
#include "font_ladder.h"
#include <math.h>
#include <string.h>

namespace {

// Largest first. Everything the binary contains from 48 down; 14 and 12 are last resorts
// that a 160-character fact never actually needs.
constexpr int SIZES[] = { 48, 44, 40, 36, 32, 28, 26, 24, 22, 20, 18, 16, 14, 12 };
// When a word is wider than any line, breaking it is better than shrinking to a size nobody
// can read across a room, but breaking it at 48 px into five pieces is worse than either.
// Breaking is only considered from this size down.
constexpr int SPLIT_FROM = 32;
constexpr int MAX_WORDS = 120;
constexpr int MAX_LINES = 16;
constexpr int SAFETY = 4;   // px shaved off each chord: kerning and rounding, not design

struct Words {
    const char *text;
    int n;
    int16_t start[MAX_WORDS];
    int16_t len[MAX_WORDS];
    int16_t w[MAX_WORDS];
};

struct Geometry {
    int n;
    int lh, ls;
    int blockH;
    int width[MAX_LINES];
};

int text_w(const char *s, int len, const lv_font_t *f) {
    return (int)lv_txt_get_width(s, (uint32_t)len, f, 0, LV_TEXT_FLAG_NONE);
}

// Leading: Montserrat's own line height is about 1.1 em, which is tight for a paragraph;
// an eighth of the size on top reads as ordinary book leading at every rung.
int line_space_for(int size) { return size / 8; }

void geometry(int n, int radius, const lv_font_t *f, int size, Geometry &g) {
    g.n = n;
    g.lh = lv_font_get_line_height(f);
    g.ls = line_space_for(size);
    g.blockH = n * g.lh + (n - 1) * g.ls;
    const float r2 = (float)radius * radius;
    for (int i = 0; i < n; ++i) {
        // The line's whole box, not just its x-height: the farther of its two edges from the
        // centre is where the circle is narrowest for it.
        const float top = -g.blockH / 2.0f + i * (g.lh + g.ls);
        const float bot = top + g.lh;
        const float y = fmaxf(fabsf(top), fabsf(bot));
        const float h2 = r2 - y * y;
        g.width[i] = h2 > 0 ? (int)(2.0f * sqrtf(h2)) - SAFETY : 0;
    }
}

// Greedy fill of the lines in g, each scaled by `scale`. Returns the number of lines used,
// or -1 when the words do not fit. With `out`, also writes the set text.
int fill(const Words &w, const Geometry &g, const lv_font_t *f, float scale, bool split,
         char *out, int outCap, int *widest) {
    const int space = text_w(" ", 1, f);
    const int hyph = text_w("-", 1, f);
    int line = 0, used = 0, o = 0, maxW = 0;
    auto put = [&](const char *s, int len) {
        if (!out) return;
        for (int i = 0; i < len && o + 1 < outCap; ++i) out[o++] = s[i];
    };
    int i = 0, from = 0;   // word i, starting at character `from` (non-zero after a break)
    while (i < w.n) {
        if (line >= g.n) return -1;
        const int avail = (int)(g.width[line] * scale);
        const char *s = w.text + w.start[i] + from;
        const int len = w.len[i] - from;
        const int ww = from ? text_w(s, len, f) : w.w[i];
        const int need = used ? used + space + ww : ww;
        if (need <= avail) {
            if (used) put(" ", 1);
            put(s, len);
            used = need;
            ++i; from = 0;
            continue;
        }
        if (used) {   // does not fit after something: start the next line
            if (used > maxW) maxW = used;
            put("\n", 1);
            ++line; used = 0;
            continue;
        }
        // Alone on an empty line and still too wide.
        if (!split) return -1;
        int k = len - 1;
        while (k > 1 && text_w(s, k, f) + hyph > avail) --k;
        if (k <= 1 || text_w(s, k, f) + hyph > avail) return -1;
        put(s, k); put("-", 1);
        if (text_w(s, k, f) + hyph > maxW) maxW = text_w(s, k, f) + hyph;
        put("\n", 1);
        from += k;
        ++line;
    }
    if (used > maxW) maxW = used;
    if (out) out[o] = 0;
    if (widest) *widest = maxW;
    return line + 1;
}

}  // namespace

bool facts_layout::fit(const char *text, int radius, Result &out) {
    memset(&out, 0, sizeof(out));
    Words w;
    w.text = text;
    w.n = 0;
    for (int p = 0; text[p] && w.n < MAX_WORDS;) {
        while (text[p] == ' ') ++p;
        if (!text[p]) break;
        const int s = p;
        while (text[p] && text[p] != ' ') ++p;
        w.start[w.n] = (int16_t)s;
        w.len[w.n] = (int16_t)(p - s);
        ++w.n;
    }
    if (!w.n) return false;

    Geometry g;
    for (int pass = 0; pass < 2; ++pass) {
        const bool split = pass == 1;
        for (int size : SIZES) {
            if (split && size > SPLIT_FROM) continue;
            const lv_font_t *f = font_ladder(size);
            for (int i = 0; i < w.n; ++i) w.w[i] = (int16_t)text_w(text + w.start[i], w.len[i], f);
            const int lh = lv_font_get_line_height(f), ls = line_space_for(size);
            const int maxN = (2 * radius + ls) / (lh + ls);
            for (int n = 1; n <= maxN && n <= MAX_LINES; ++n) {
                geometry(n, radius, f, size, g);
                if (fill(w, g, f, 1.0f, split, nullptr, 0, nullptr) < 0) continue;

                // It fits in n lines. Now narrow every line by the same factor for as long
                // as it still fits in n: the greedy fill leaves a long first line and a
                // stub at the end, and this evens them out while keeping the round outline
                // (every line shrinks in proportion to its own chord).
                float lo = 0.3f, hi = 1.0f;
                if (!split && n > 1) {
                    for (int it = 0; it < 12; ++it) {
                        const float mid = (lo + hi) / 2;
                        const int used = fill(w, g, f, mid, false, nullptr, 0, nullptr);
                        if (used == n) hi = mid; else lo = mid;
                    }
                }
                const int used = fill(w, g, f, hi, split, out.text, sizeof(out.text), &out.widest);
                out.font = f;
                out.size = size;
                out.lines = used;
                out.lineSpace = ls;
                out.blockH = used * lh + (used - 1) * ls;
                return true;
            }
        }
    }
    // Unreachable for anything the contract allows (160 characters always fits at 12 px).
    // Say something rather than nothing.
    const lv_font_t *f = font_ladder(12);
    strncpy(out.text, text, sizeof(out.text) - 1);
    out.font = f; out.size = 12; out.lines = 1; out.blockH = lv_font_get_line_height(f);
    return true;
}

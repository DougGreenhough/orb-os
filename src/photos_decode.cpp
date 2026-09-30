// Photos: file bytes -> one cover-cropped 466 x 466 frame. See photos_decode.h.
#include "photos_decode.h"
#include <PNGdec.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <tjpgd.h>            // the TJpg_Decoder library's copy of the core
#else
#include <chrono>
#include "photos_tjpgd.h"     // the simulator's copy of the same core
#endif

namespace photos_decode {

void *alloc(size_t bytes) {
#ifdef ARDUINO
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}

void release(void *p) {
    if (!p) return;
#ifdef ARDUINO
    heap_caps_free(p);
#else
    free(p);
#endif
}

}  // namespace photos_decode

namespace {

using photos_decode::SIDE;

uint32_t now_ms() {
#ifdef ARDUINO
    return millis();
#else
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

void say(char *why, size_t n, const char *msg) {
    if (why && n) { strncpy(why, msg, n - 1); why[n - 1] = 0; }
}

// ---- the mapping from the square on the glass back into the source ------------------------
//
// Worked out once per decode as two lookup tables, so every sampler below agrees on exactly
// which source pixel a screen pixel came from, and the per-pixel work is two table reads.
//
// "Oriented" space is the picture the right way up (EXIF applied); "stored" space is the
// pixels as the file lays them down. For most files they are the same.
struct Cover {
    int sw = 0, sh = 0;          // stored size (after any decoder-side scaling)
    int orient = 1;              // EXIF 1..8
    int ow = 0, oh = 0;          // oriented size
    float s = 1.0f;              // screen px per oriented source px
    float offX = 0, offY = 0;    // how much of the scaled picture is cropped off each side
    int16_t *umap = nullptr;     // screen column -> oriented column
    int16_t *vmap = nullptr;     // screen row    -> oriented row
};

bool cover_setup(Cover &c, int sw, int sh, int orient) {
    c.sw = sw; c.sh = sh;
    c.orient = (orient >= 1 && orient <= 8) ? orient : 1;
    const bool swap = c.orient >= 5;
    c.ow = swap ? sh : sw;
    c.oh = swap ? sw : sh;
    const float sx = (float)SIDE / c.ow, sy = (float)SIDE / c.oh;
    c.s = sx > sy ? sx : sy;
    c.offX = (c.ow * c.s - SIDE) * 0.5f;
    c.offY = (c.oh * c.s - SIDE) * 0.5f;
    c.umap = (int16_t *)photos_decode::alloc(sizeof(int16_t) * SIDE * 2);
    if (!c.umap) return false;
    c.vmap = c.umap + SIDE;
    for (int d = 0; d < SIDE; ++d) {
        int u = (int)((d + 0.5f + c.offX) / c.s);
        int v = (int)((d + 0.5f + c.offY) / c.s);
        c.umap[d] = (int16_t)(u < 0 ? 0 : u >= c.ow ? c.ow - 1 : u);
        c.vmap[d] = (int16_t)(v < 0 ? 0 : v >= c.oh ? c.oh - 1 : v);
    }
    return true;
}

void cover_free(Cover &c) { photos_decode::release(c.umap); c.umap = c.vmap = nullptr; }

// Oriented (u, v) -> stored (x, y). EXIF's eight cases.
inline void to_stored(const Cover &c, int u, int v, int &x, int &y) {
    switch (c.orient) {
        default:
        case 1: x = u;             y = v;             break;
        case 2: x = c.sw - 1 - u;  y = v;             break;
        case 3: x = c.sw - 1 - u;  y = c.sh - 1 - v;  break;
        case 4: x = u;             y = c.sh - 1 - v;  break;
        case 5: x = v;             y = u;             break;
        case 6: x = v;             y = c.sh - 1 - u;  break;
        case 7: x = c.sw - 1 - v;  y = c.sh - 1 - u;  break;
        case 8: x = c.sw - 1 - v;  y = u;             break;
    }
}

// A stored rectangle [x0,x1) x [y0,y1) -> the oriented rectangle it occupies.
void rect_to_oriented(const Cover &c, int x0, int y0, int x1, int y1,
                      int &u0, int &v0, int &u1, int &v1) {
    switch (c.orient) {
        default:
        case 1: u0 = x0;         u1 = x1;         v0 = y0;         v1 = y1;         break;
        case 2: u0 = c.sw - x1;  u1 = c.sw - x0;  v0 = y0;         v1 = y1;         break;
        case 3: u0 = c.sw - x1;  u1 = c.sw - x0;  v0 = c.sh - y1;  v1 = c.sh - y0;  break;
        case 4: u0 = x0;         u1 = x1;         v0 = c.sh - y1;  v1 = c.sh - y0;  break;
        case 5: u0 = y0;         u1 = y1;         v0 = x0;         v1 = x1;         break;
        case 6: u0 = c.sh - y1;  u1 = c.sh - y0;  v0 = x0;         v1 = x1;         break;
        case 7: u0 = c.sh - y1;  u1 = c.sh - y0;  v0 = c.sw - x1;  v1 = c.sw - x0;  break;
        case 8: u0 = y0;         u1 = y1;         v0 = c.sw - x1;  v1 = c.sw - x0;  break;
    }
}

// First screen index whose map value is >= val (the maps never decrease).
int first_at_least(const int16_t *map, int val) {
    int lo = 0, hi = SIDE;
    while (lo < hi) { const int mid = (lo + hi) / 2; if (map[mid] < val) lo = mid + 1; else hi = mid; }
    return lo;
}

// Nearest-neighbour: fill every screen pixel whose source falls inside this stored block.
// Blocks can arrive in any order (JPEG MCUs, PNG rows), and each screen pixel is written by
// exactly one of them.
void sample_block(const Cover &c, uint16_t *dst, const uint16_t *px, int bw,
                  int x0, int y0, int x1, int y1) {
    int u0, v0, u1, v1;
    rect_to_oriented(c, x0, y0, x1, y1, u0, v0, u1, v1);
    const int dx0 = first_at_least(c.umap, u0), dx1 = first_at_least(c.umap, u1);
    const int dy0 = first_at_least(c.vmap, v0), dy1 = first_at_least(c.vmap, v1);
    for (int dy = dy0; dy < dy1; ++dy) {
        uint16_t *row = dst + dy * SIDE;
        const int v = c.vmap[dy];
        for (int dx = dx0; dx < dx1; ++dx) {
            int x, y;
            to_stored(c, c.umap[dx], v, x, y);
            if (x < x0 || x >= x1 || y < y0 || y >= y1) continue;
            row[dx] = px[(y - y0) * bw + (x - x0)];
        }
    }
}

// Box filter for rows arriving top to bottom, upright (orientation 1) and shrinking: every
// source pixel lands in exactly one screen pixel and the screen pixel is their average. This
// is what keeps a 2000 px PNG from sparkling when it is taken down to 466.
struct RowBox {
    const Cover *c = nullptr;
    uint16_t *dst = nullptr;
    uint32_t *acc = nullptr;     // r, g, b, n per screen column
    int16_t  *col = nullptr;     // source column -> screen column, -1 when cropped off
    int cur = -1;                // the screen row being accumulated
};

bool rowbox_begin(RowBox &b, const Cover &c, uint16_t *dst) {
    b.c = &c; b.dst = dst; b.cur = -1;
    b.acc = (uint32_t *)photos_decode::alloc(sizeof(uint32_t) * 4 * SIDE + sizeof(int16_t) * c.sw);
    if (!b.acc) return false;
    b.col = (int16_t *)(b.acc + 4 * SIDE);
    memset(b.acc, 0, sizeof(uint32_t) * 4 * SIDE);
    for (int x = 0; x < c.sw; ++x) {
        const int d = (int)floorf((x + 0.5f) * c.s - c.offX);
        b.col[x] = (int16_t)((d < 0 || d >= SIDE) ? -1 : d);
    }
    return true;
}

void rowbox_flush(RowBox &b) {
    if (b.cur < 0 || b.cur >= SIDE) return;
    uint16_t *row = b.dst + b.cur * SIDE;
    for (int d = 0; d < SIDE; ++d) {
        uint32_t *a = b.acc + 4 * d;
        if (a[3]) {
            const uint32_t n = a[3], h = n / 2;
            row[d] = (uint16_t)((((a[0] + h) / n) << 11) | (((a[1] + h) / n) << 5) | ((a[2] + h) / n));
        }
        a[0] = a[1] = a[2] = a[3] = 0;
    }
}

void rowbox_row(RowBox &b, int y, const uint16_t *px) {
    const Cover &c = *b.c;
    const int r = (int)floorf((y + 0.5f) * c.s - c.offY);
    if (r != b.cur) { rowbox_flush(b); b.cur = r; }
    if (r < 0 || r >= SIDE) return;
    for (int x = 0; x < c.sw; ++x) {
        const int d = b.col[x];
        if (d < 0) continue;
        const uint16_t p = px[x];
        uint32_t *a = b.acc + 4 * d;
        a[0] += p >> 11; a[1] += (p >> 5) & 0x3F; a[2] += p & 0x1F; a[3] += 1;
    }
}

void rowbox_end(RowBox &b) { rowbox_flush(b); photos_decode::release(b.acc); b.acc = nullptr; }

// Rows from any source go through here: box filter when shrinking, nearest when growing.
struct RowSink {
    Cover c;
    RowBox box;
    bool boxed = false;
    uint16_t *dst = nullptr;
};

bool rowsink_begin(RowSink &k, int w, int h, uint16_t *dst) {
    k.dst = dst;
    if (!cover_setup(k.c, w, h, 1)) return false;
    k.boxed = k.c.s < 1.0f;
    if (k.boxed && !rowbox_begin(k.box, k.c, dst)) { cover_free(k.c); return false; }
    return true;
}
void rowsink_row(RowSink &k, int y, const uint16_t *px) {
    if (k.boxed) rowbox_row(k.box, y, px);
    else         sample_block(k.c, k.dst, px, k.c.sw, 0, y, k.c.sw, y + 1);
}
void rowsink_end(RowSink &k) {
    if (k.boxed) rowbox_end(k.box);
    cover_free(k.c);
}

// A long decode hands the CPU back now and then. The network task shares core 0 with the
// WiFi stack and the idle task's watchdog, and a camera JPEG can take a second or two.
struct Pacer {
    uint32_t last = now_ms();
    photos_decode::keep_going_fn keep = nullptr;
    bool stop = false;
    bool tick() {
        const uint32_t t = now_ms();
        if (t - last < 25) return !stop;
        last = t;
#ifdef ARDUINO
        vTaskDelay(1);
#endif
        if (keep && !keep()) stop = true;
        return !stop;
    }
};

// ---- ORB5 ----------------------------------------------------------------------------------

bool decode_orb5(const uint8_t *raw, size_t len, uint16_t *dst, char *why, size_t wn, Pacer &pace) {
    if (len < 8) { say(why, wn, "damaged .orb5"); return false; }
    const int w = raw[4] | (raw[5] << 8), h = raw[6] | (raw[7] << 8);
    if (w <= 0 || h <= 0 || len < 8 + (size_t)w * h * 2) { say(why, wn, "damaged .orb5"); return false; }
    const uint16_t *px = (const uint16_t *)(raw + 8);
    if (w == SIDE && h == SIDE) { memcpy(dst, px, photos_decode::FRAME_BYTES); return true; }
    RowSink k;
    if (!rowsink_begin(k, w, h, dst)) { say(why, wn, "not enough memory"); return false; }
    for (int y = 0; y < h; ++y) {
        rowsink_row(k, y, px + (size_t)y * w);
        if ((y & 31) == 0 && !pace.tick()) break;
    }
    rowsink_end(k);
    if (pace.stop) { say(why, wn, "abandoned"); return false; }
    return true;
}

// ---- PNG -----------------------------------------------------------------------------------

struct PngCtx {
    PNG *png;
    RowSink *sink;
    uint16_t *line;
    Pacer *pace;
};

int png_row(PNGDRAW *d) {
    PngCtx *ctx = (PngCtx *)d->pUser;
    // Transparent parts land on black, which is what the glass is anyway.
    ctx->png->getLineAsRGB565(d, ctx->line, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);
    rowsink_row(*ctx->sink, d->y, ctx->line);
    if ((d->y & 15) == 0 && !ctx->pace->tick()) return 0;
    return 1;
}

int png_bits_per_pixel(PNG *png) {
    const int per = png->getBpp();   // bits per channel
    switch (png->getPixelType()) {
        case PNG_PIXEL_TRUECOLOR:       return 3 * per;
        case PNG_PIXEL_GRAY_ALPHA:      return 2 * per;
        case PNG_PIXEL_TRUECOLOR_ALPHA: return 4 * per;
        default:                        return per;   // grey, palette
    }
}
int png_pitch(PNG *png) { return (png_bits_per_pixel(png) * png->getWidth() + 7) / 8; }

// Where PNGdec's DecodePNG() puts its two lines inside ucPixels: the current line at offset
// 15 (16-byte aligned pixels after the filter byte), the previous one at the next offset
// that is 15 mod 16 after it. Both must end inside the array.
bool png_lines_fit(int pitch) {
    int y = 15 + pitch + 1;
    y += 15 - (y & 15);
    return y + pitch + 1 <= PNG_MAX_BUFFERED_PIXELS;
}

bool decode_png(const uint8_t *raw, size_t len, uint16_t *dst, char *why, size_t wn, Pacer &pace) {
    // PNGdec's state is tens of KB: never on this stack, always PSRAM, and only for as long
    // as one decode takes.
    void *mem = photos_decode::alloc(sizeof(PNG));
    if (!mem) { say(why, wn, "not enough memory"); return false; }
    PNG *png = new (mem) PNG();
    bool ok = false;
    const int rc = png->openRAM((uint8_t *)raw, (int)len, png_row);
    if (rc != PNG_SUCCESS) {
        const int e = png->getLastError();
        say(why, wn, (rc == PNG_TOO_BIG || e == PNG_TOO_BIG) ? "PNG too wide"
                     : e == PNG_UNSUPPORTED_FEATURE ? "interlaced or 16-bit PNG"
                     : "damaged PNG");
    } else if (!png_lines_fit(png_pitch(png))) {
        // PNGdec keeps two scanlines in one PNG_MAX_BUFFERED_PIXELS array but only checks
        // that ONE fits, so a line between about half and all of it decodes a row and then
        // fails on memory it has already overrun. Refused here, before it gets that far.
        // The flag is shared with every other PNG user (platformio.ini); raising it is
        // what would let wider PNGs through.
        int widest = 1;
        while (png_lines_fit((png_bits_per_pixel(png) * (widest + 1) + 7) / 8)) ++widest;
        snprintf(why, wn, "PNG over %d px wide", widest);
    } else {
        const int w = png->getWidth(), h = png->getHeight();
        RowSink k;
        uint16_t *line = (uint16_t *)photos_decode::alloc(sizeof(uint16_t) * (size_t)w);
        if (!line || !rowsink_begin(k, w, h, dst)) {
            say(why, wn, "not enough memory");
        } else {
            PngCtx ctx = { png, &k, line, &pace };
            const int r = png->decode(&ctx, 0);
            rowsink_end(k);
            if (pace.stop) say(why, wn, "abandoned");
            else if (r != PNG_SUCCESS) say(why, wn, "damaged PNG");
            else ok = true;
        }
        photos_decode::release(line);
        png->close();
    }
    png->~PNG();
    photos_decode::release(mem);
    return ok;
}

// ---- JPEG ----------------------------------------------------------------------------------

// EXIF, read straight out of the APP1 segment: orientation (0x0112) and the capture date
// (0x9003 DateTimeOriginal, else 0x0132 DateTime). Bounds-checked at every step, because a
// photo's metadata is the least trustworthy part of it.
struct Exif { int orient = 1; char when[20] = ""; };

struct Tiff {
    const uint8_t *p; size_t n; bool le;
    bool ok(size_t off, size_t len) const { return off <= n && len <= n - off; }
    uint16_t u16(size_t o) const { return le ? (uint16_t)(p[o] | p[o + 1] << 8) : (uint16_t)(p[o] << 8 | p[o + 1]); }
    uint32_t u32(size_t o) const {
        return le ? (uint32_t)p[o] | (uint32_t)p[o + 1] << 8 | (uint32_t)p[o + 2] << 16 | (uint32_t)p[o + 3] << 24
                  : (uint32_t)p[o] << 24 | (uint32_t)p[o + 1] << 16 | (uint32_t)p[o + 2] << 8 | (uint32_t)p[o + 3];
    }
};

void exif_ifd(const Tiff &t, uint32_t ifd, Exif &ex, bool top, int depth) {
    if (depth > 2 || !t.ok(ifd, 2)) return;
    const int count = t.u16(ifd);
    for (int i = 0; i < count && i < 256; ++i) {
        const size_t e = ifd + 2 + (size_t)i * 12;
        if (!t.ok(e, 12)) return;
        const uint16_t tag = t.u16(e), type = t.u16(e + 2);
        const uint32_t cnt = t.u32(e + 4);
        if (top && tag == 0x0112 && type == 3) {
            ex.orient = t.u16(e + 8);
        } else if (top && tag == 0x8769) {
            exif_ifd(t, t.u32(e + 8), ex, false, depth + 1);
        } else if ((tag == 0x9003 || (tag == 0x0132 && !ex.when[0])) && type == 2 && cnt >= 19) {
            const uint32_t off = t.u32(e + 8);
            if (t.ok(off, 19)) { memcpy(ex.when, t.p + off, 19); ex.when[19] = 0; }
        }
    }
}

void read_exif(const uint8_t *raw, size_t len, Exif &ex) {
    if (len < 4 || raw[0] != 0xFF || raw[1] != 0xD8) return;
    size_t i = 2;
    while (i + 4 <= len && raw[i] == 0xFF) {
        const uint8_t m = raw[i + 1];
        if (m == 0xDA || m == 0xD9) return;          // image data begins: no EXIF past here
        const size_t seg = (size_t)(raw[i + 2] << 8 | raw[i + 3]);
        if (seg < 2 || i + 2 + seg > len) return;
        if (m == 0xE1 && seg >= 16 && !memcmp(raw + i + 4, "Exif\0\0", 6)) {
            Tiff t = { raw + i + 10, seg - 8, raw[i + 10] == 'I' };
            if (t.n < 8) return;
            exif_ifd(t, t.u32(4), ex, true, 0);
            return;
        }
        i += 2 + seg;
    }
}

struct JpgCtx {
    const uint8_t *raw; size_t len, pos;
    Cover *c;
    uint16_t *dst;
    Pacer *pace;
};

size_t jpg_in(JDEC *jd, uint8_t *buf, size_t n) {
    JpgCtx *ctx = (JpgCtx *)jd->device;
    const size_t left = ctx->len - ctx->pos;
    if (n > left) n = left;
    if (buf) memcpy(buf, ctx->raw + ctx->pos, n);
    ctx->pos += n;
    return n;
}

int jpg_out(JDEC *jd, void *bitmap, JRECT *r) {
    JpgCtx *ctx = (JpgCtx *)jd->device;
    const int bw = r->right - r->left + 1;
    int x1 = r->right + 1, y1 = r->bottom + 1;
    if (x1 > ctx->c->sw) x1 = ctx->c->sw;
    if (y1 > ctx->c->sh) y1 = ctx->c->sh;
    sample_block(*ctx->c, ctx->dst, (const uint16_t *)bitmap, bw, r->left, r->top, x1, y1);
    return ctx->pace->tick() ? 1 : 0;
}

void format_date(const char *y, const char *m, const char *d, char *out, size_t n) {
    static const char *MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    const int mi = (m[0] - '0') * 10 + (m[1] - '0'), di = (d[0] - '0') * 10 + (d[1] - '0');
    if (mi < 1 || mi > 12 || di < 1 || di > 31) { if (n) out[0] = 0; return; }
    snprintf(out, n, "%d %s %.4s", di, MON[mi - 1], y);
}

bool is_digits(const char *s, int n) {
    for (int i = 0; i < n; ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

bool decode_jpeg(const uint8_t *raw, size_t len, uint16_t *dst, photos_decode::Meta &meta,
                 char *why, size_t wn, Pacer &pace) {
    Exif ex;
    read_exif(raw, len, ex);
    // "2025:08:14 10:11:12". Cameras with no clock set write zeros; those say nothing.
    if (ex.when[0] && is_digits(ex.when, 4) && ex.when[4] == ':' && is_digits(ex.when + 5, 2) &&
        is_digits(ex.when + 8, 2) && strncmp(ex.when, "0000", 4) != 0)
        format_date(ex.when, ex.when + 5, ex.when + 8, meta.date, sizeof(meta.date));

    // JDEC plus its working pool (TJPGD_WORKSPACE_SIZE is 3500 with JD_FASTDECODE 1), in
    // one PSRAM block: 4 KB is under the auto-PSRAM threshold, so it has to be asked for.
    constexpr size_t POOL = 4096;
    uint8_t *mem = (uint8_t *)photos_decode::alloc(sizeof(JDEC) + POOL);
    if (!mem) { say(why, wn, "not enough memory"); return false; }
    JDEC *jd = (JDEC *)mem;
    memset(jd, 0, sizeof(JDEC));
    Cover c;
    JpgCtx ctx = { raw, len, 0, &c, dst, &pace };
    bool ok = false;
    JRESULT r = jd_prepare(jd, jpg_in, mem + sizeof(JDEC), POOL, &ctx);
    if (r != JDR_OK) {
        say(why, wn, r == JDR_FMT3 ? "progressive JPEG"
                     : r == JDR_MEM1 ? "unusual JPEG"
                     : "damaged JPEG");
    } else {
        jd->swap = 0;   // Bodmer's field: LVGL wants little-endian RGB565, as decoded
        const int w = jd->width, h = jd->height;
        const int shortSide = w < h ? w : h;
        // The biggest DCT shrink that still leaves the short side at least a screen wide;
        // the rest of the way is sampled. A 4032 x 3024 photo decodes at 1/4 (1008 x 756).
        int scale = 0;
        while (scale < 3 && (shortSide >> (scale + 1)) >= SIDE) ++scale;
        const int sw = (w + (1 << scale) - 1) >> scale, sh = (h + (1 << scale) - 1) >> scale;
        if (!cover_setup(c, sw, sh, ex.orient)) {
            say(why, wn, "not enough memory");
        } else {
            r = jd_decomp(jd, jpg_out, (uint8_t)scale);
            if (pace.stop) say(why, wn, "abandoned");
            else if (r != JDR_OK) say(why, wn, "damaged JPEG");
            else ok = true;
            cover_free(c);
        }
    }
    photos_decode::release(mem);
    return ok;
}

// ---- names ---------------------------------------------------------------------------------

bool ieq_prefix(const char *s, const char *p) {
    for (; *p; ++s, ++p) {
        char a = *s, b = *p;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

// A date in a name: YYYY MM DD with optional single separators. Returns its start and
// length, or -1.
int find_date(const char *s, int n, int &len, char *y, char *m, char *d) {
    for (int i = 0; i + 8 <= n; ++i) {
        if (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9') continue;
        if (!((s[i] == '1' && s[i + 1] == '9') || (s[i] == '2' && s[i + 1] == '0'))) continue;
        if (!is_digits(s + i, 4)) continue;
        int j = i + 4;
        const bool sep1 = j < n && (s[j] == '-' || s[j] == '_' || s[j] == '.');
        if (sep1) ++j;
        if (j + 2 > n || !is_digits(s + j, 2)) continue;
        const int mm = j; j += 2;
        const bool sep2 = j < n && (s[j] == '-' || s[j] == '_' || s[j] == '.');
        if (sep1 != sep2) continue;
        if (sep2) ++j;
        if (j + 2 > n || !is_digits(s + j, 2)) continue;
        const int dd = j; j += 2;
        const int mi = (s[mm] - '0') * 10 + s[mm + 1] - '0', di = (s[dd] - '0') * 10 + s[dd + 1] - '0';
        if (mi < 1 || mi > 12 || di < 1 || di > 31) continue;
        memcpy(y, s + i, 4); memcpy(m, s + mm, 2); memcpy(d, s + dd, 2);
        len = j - i;
        return i;
    }
    return -1;
}

// The leaf of a path, without its extension.
int stem(const char *name, const char **start) {
    const char *leaf = strrchr(name, '/');
    leaf = leaf ? leaf + 1 : name;
    const char *dot = strrchr(leaf, '.');
    *start = leaf;
    return dot && dot != leaf ? (int)(dot - leaf) : (int)strlen(leaf);
}

// Latin-1 range of UTF-8 (C3 80..C3 BF) folded to the letter underneath the accent.
char fold_c3(uint8_t b) {
    static const char map[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    return (b >= 0x80 && b <= 0xBF) ? map[b - 0x80] : 0;
}

}  // namespace

namespace photos_decode {

Kind kind_of_name(const char *name) {
    const char *leaf = strrchr(name, '/');
    leaf = leaf ? leaf + 1 : name;
    if (leaf[0] == '.') return K_NONE;
    const char *dot = strrchr(leaf, '.');
    if (!dot) return K_NONE;
    char ext[8] = {};
    for (int i = 0; i < 6 && dot[1 + i]; ++i) {
        char ch = dot[1 + i];
        ext[i] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    }
    if (!strcmp(ext, "orb5")) return K_ORB5;
    if (!strcmp(ext, "png"))  return K_PNG;
    if (!strcmp(ext, "jpg") || !strcmp(ext, "jpeg")) return K_JPEG;
    return K_NONE;
}

bool decode(const uint8_t *raw, size_t len, uint16_t *dst, Meta &meta,
            char *why, size_t whyLen, keep_going_fn keepGoing) {
    meta.date[0] = 0;
    say(why, whyLen, "");
    if (!raw || !dst || len < 8) { say(why, whyLen, "empty file"); return false; }
    // Black first: a sampler that misses an edge pixel leaves black, never last photo's.
    memset(dst, 0, FRAME_BYTES);
    Pacer pace;
    pace.keep = keepGoing;
    if (!memcmp(raw, "ORB5", 4)) return decode_orb5(raw, len, dst, why, whyLen, pace);
    if (!memcmp(raw, "\x89PNG", 4)) return decode_png(raw, len, dst, why, whyLen, pace);
    if (raw[0] == 0xFF && raw[1] == 0xD8) return decode_jpeg(raw, len, dst, meta, why, whyLen, pace);
    say(why, whyLen, "not a picture");
    return false;
}

bool date_from_name(const char *name, char *out, size_t n) {
    const char *s;
    const int len = stem(name, &s);
    char y[5] = {}, m[3] = {}, d[3] = {};
    int dl;
    if (find_date(s, len, dl, y, m, d) < 0) { if (n) out[0] = 0; return false; }
    format_date(y, m, d, out, n);
    return out[0] != 0;
}

void caption_from_name(const char *name, char *out, size_t n) {
    if (!n) return;
    out[0] = 0;
    const char *s;
    int len = stem(name, &s);

    // A camera's own counter is not a caption.
    static const char *CAMERA[] = { "IMG", "DSC", "DSCN", "DSCF", "PXL", "MVIMG", "PANO", "VID",
                                    "DJI", "GOPR", "GH0", "SAM", "WP", "P", "_MG", "Screenshot",
                                    "Photo", "image", "Snapchat", "signal", "WhatsApp Image" };
    for (const char *p : CAMERA) {
        if (!ieq_prefix(s, p)) continue;
        const char *q = s + strlen(p);
        if (*q == '_' || *q == '-' || *q == ' ') ++q;
        if (*q >= '0' && *q <= '9') return;
    }

    // Take a leading date off: it is shown as the date.
    char y[5], m[3], d[3];
    int dl;
    if (find_date(s, len, dl, y, m, d) == 0) {
        s += dl; len -= dl;
        while (len > 0 && (*s == ' ' || *s == '_' || *s == '-' || *s == '.')) { ++s; --len; }
    }

    int digits = 0;
    for (int i = 0; i < len; ++i) if (s[i] >= '0' && s[i] <= '9') ++digits;
    if (len == 0 || digits * 2 >= len) return;   // mostly a number: still a counter

    bool spaces = memchr(s, ' ', (size_t)len) != nullptr;
    size_t o = 0;
    for (int i = 0; i < len && o + 1 < n; ++i) {
        const uint8_t ch = (uint8_t)s[i];
        char put;
        if (ch == 0xC3 && i + 1 < len) { put = fold_c3((uint8_t)s[++i]); if (!put) continue; }
        else if (ch >= 0x80) continue;                    // anything else non-ASCII: dropped
        else if (ch == '_') put = ' ';
        else if (ch == '-' && !spaces) put = ' ';         // "saint-malo" keeps its hyphen only in "Saint-Malo harbour"
        else if (ch < 0x20) continue;
        else put = (char)ch;
        if (put == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
        out[o++] = put;
    }
    while (o > 0 && out[o - 1] == ' ') --o;
    out[o] = 0;
}

}  // namespace photos_decode

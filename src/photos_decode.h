#pragma once
// Photos: turning a file's bytes into one full-bleed frame for the round glass.
//
// Pure computation, no LVGL and no card access, so it can run on the network task while
// the UI thread keeps drawing. Every format ends in the same place: SIDE x SIDE RGB565
// (little-endian, LVGL's LV_COLOR_16_SWAP 0 layout), the picture scaled to COVER the square
// and centre-cropped, so a landscape photo loses its sides rather than gaining black bars
// on a dial that has no corners to spare.
//
// Formats, sniffed from the bytes rather than trusted from the name:
//   ORB5  the relay's raw RGB565 (see ponderer.h). Resampled if it is not already SIDE.
//   PNG   PNGdec, both targets. Rows are box-averaged when shrinking, so a big PNG does
//         not shimmer. PNGdec holds two scanlines in PNG_MAX_BUFFERED_PIXELS (8192, a
//         build flag shared with every PNG on the device), which caps the width at 1360 px
//         for RGB and 1020 px for RGBA; wider ones are refused and said so, as are
//         interlaced and 16-bit PNGs, which PNGdec does not read.
//   JPEG  the TJpgDec core, both targets (the simulator compiles its own copy, see
//         photos_tjpgd.h). Baseline only: progressive JPEGs are refused and said so. The
//         decoder's own 1/2, 1/4, 1/8 DCT scaling takes a camera photo most of the way,
//         and the last step (under 2x) is sampled. EXIF orientation is applied, and the
//         EXIF capture date is returned.
//
// Memory: the caller owns `dst`. Decoding borrows a few KB of PSRAM for tables and one
// scanline, plus PNGdec's state (tens of KB) for a PNG, and gives all of it back before
// returning. Nothing large goes on the stack: this runs on a ~7 KB network task.
#include <stddef.h>
#include <stdint.h>

namespace photos_decode {

constexpr int    SIDE        = 466;
constexpr size_t FRAME_BYTES = (size_t)SIDE * SIDE * 2;   // 434,312

enum Kind : uint8_t { K_NONE = 0, K_ORB5, K_PNG, K_JPEG };

// By extension, case-insensitive: .orb5, .png, .jpg, .jpeg. K_NONE for anything else,
// including dot-files (macOS leaves "._IMG_1234.jpg" resource forks on every card it touches).
Kind kind_of_name(const char *name);

struct Meta {
    char date[24];   // "14 Aug 2025", or "" when the file does not say
};

// Called now and then during a long decode. Return false to abandon it (the screen was
// left, or the source changed). May be null.
typedef bool (*keep_going_fn)();

// Decode `raw` into dst (FRAME_BYTES, caller-allocated). On failure returns false and puts
// a short reason in `why` for the log ("progressive JPEG", "PNG too wide").
bool decode(const uint8_t *raw, size_t len, uint16_t *dst, Meta &meta,
            char *why, size_t whyLen, keep_going_fn keepGoing);

// A caption worth drawing from a file name, or "" when the name is only a camera's counter
// (IMG_2031, PXL_20250814_101112345, DSC01234). A leading date is dropped (it is shown as
// the date instead), underscores become spaces, UTF-8 accents fold to plain ASCII because
// the fonts have no fallback glyphs.
void caption_from_name(const char *name, char *out, size_t n);

// "14 Aug 2025" from a date written into a file name (20250814, 2025-08-14, 2025_08_14).
bool date_from_name(const char *name, char *out, size_t n);

// PSRAM on the device (explicitly: internal RAM is the scarce one), plain heap in the
// simulator. Everything the Photos screen allocates goes through these two.
void *alloc(size_t bytes);
void  release(void *p);

}  // namespace photos_decode

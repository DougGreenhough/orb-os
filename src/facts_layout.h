#pragma once
#include <lvgl.h>
// Facts, the typesetting: set one short paragraph as large as it will go inside a circle.
//
// Pure arithmetic over LVGL font metrics (no objects), so it is the same on both targets and
// can be reasoned about on its own. Each line is fitted against the circle's chord at that
// line's own height, the farther of its top and bottom edges, so a block of type takes the
// round shape of the glass instead of a square box's.
namespace facts_layout {

struct Result {
    const lv_font_t *font;
    int  size;          // px, a rung of font_ladder.h
    int  lines;
    int  lineSpace;     // extra px between lines, for the label's text_line_space
    int  blockH;        // height of the whole block, px
    int  widest;        // px, of the widest line as set
    char text[300];     // the lines, joined by '\n' (and '-' where a word had to be broken)
};

// radius: of the circle the block must stay inside, centred on the block's own centre.
// Returns false only for empty text; anything else fits at some size, breaking words that
// are wider than the circle as a last resort.
bool fit(const char *text, int radius, Result &out);

// The same, in one given face (a theme's own, which exists at a single size): true when the
// whole text sets inside the circle in it without breaking a word, false when it does not
// and the ladder should be asked instead. out.size is the face's line height.
bool fit_face(const char *text, int radius, const lv_font_t *face, Result &out);

}  // namespace facts_layout

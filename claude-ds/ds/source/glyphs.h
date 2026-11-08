// Extra 8x8 tiles loaded after the 96 ASCII characters of the console font:
// box drawing, blocks for bars and the mascot, and a few icons.
//
// In a Grid cell, character codes >= GLYPH_FIRST refer to these tiles.

#ifndef CLAUDE_DS_GLYPHS_H
#define CLAUDE_DS_GLYPHS_H

#include <stdint.h>

enum {
    GLYPH_FIRST = 128,
    G_H = GLYPH_FIRST, // horizontal line
    G_V,               // vertical line
    G_TL,              // corner top-left
    G_TR,              // corner top-right
    G_BL,              // corner bottom-left
    G_BR,              // corner bottom-right
    G_LT,              // tee pointing right (left edge)
    G_RT,              // tee pointing left (right edge)
    G_TT,              // tee pointing down (top edge)
    G_BT,              // tee pointing up (bottom edge)
    G_X,               // cross
    G_FULL,            // full block
    G_UPPER,           // upper half block
    G_LOWER,           // lower half block
    G_SHADE,           // 50% shade
    G_DOT,             // bullet
    G_ARROW,           // right-pointing triangle
    G_NOTE,            // eighth note
    G_HEART,           // heart
    G_WIFI,            // wifi waves

    // The buddy: 40x40 pixels = 5x5 tiles (row by row), see tools/gen_buddy.py
    G_BUDDY0,
    // Eye tiles replacing buddy tiles (1,2) and (3,2): mood * 2 + side
    G_EYES0 = G_BUDDY0 + 25,
    G_END = G_EYES0 + 10
};

#define BUDDY_TILES 5

enum {
    EYES_OPEN = 0,
    EYES_CLOSED = 1,
    EYES_HAPPY = 2,
    EYES_SURPRISED = 3,
    EYES_DEAD = 4,
};

#define GLYPH_COUNT (G_END - GLYPH_FIRST)

// One byte per row, most significant bit = leftmost pixel.
extern const uint8_t glyph_bits[GLYPH_COUNT][8];

#endif // CLAUDE_DS_GLYPHS_H

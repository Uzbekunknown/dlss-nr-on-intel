/* The menu's glyphs, rasterised at start-up rather than stored as a table.
 *
 * The obvious design is a compiled-in 8x16 bitmap font: public domain, no dependencies, no
 * runtime work. It is the wrong one here, for a reason that only shows up in the bilingual menu.
 *
 * A bitmap table covers the glyphs somebody typed into it. This menu prints Chinese as well as
 * English, and a table would need a hand-drawn 16x16 bitmap for every character its Chinese
 * strings use - correct, but a kilobyte of data per glyph that nobody can proofread, and it would
 * silently print blanks the first time a string used a character outside the set.
 *
 * `GetGlyphOutlineW` answers the question directly: ask the system font for the glyph, get its
 * bitmap, copy it. Any character Windows can render, the menu can draw, and the set is no longer
 * closed - which is what makes adding a language later a string change and nothing else.
 *
 * The cost is that the atlas is built at start-up rather than at compile time: one pass over the
 * characters the menu can print, about a millisecond, once per process. That is the right trade
 * for removing a class of bug that only appears in the language the author does not read.
 *
 * The layer is Windows-only by construction (Linux drives the same settings through `nr-panel`,
 * which already exists), so this does not need a portable arm.
 */
#ifndef NR_MENU_FONT_H
#define NR_MENU_FONT_H

#include <stdint.h>

/* What the menu can print. ASCII printable covers the English strings and the numbers; the CJK
 * range is the block the characters in `nr_config.LANG` come from. Both are baked into the atlas
 * so a lookup never fails at draw time. */
#define NR_FONT_FIRST_ASCII 0x20
#define NR_FONT_LAST_ASCII  0x7E

/* One cell, in pixels: the em size the menu draws at. The cell is a little wider than the widest
 * Latin glyph so a double-width CJK character has room, and the atlas uses the same cell for both
 * so the draw code never branches. 16 is the smallest that stays readable over a game. */
#define NR_FONT_CELL_W 16
#define NR_FONT_CELL_H 16

/* The CJK characters the menu's own strings use, listed so the atlas holds exactly them plus
 * anything a later string adds. Kept as codepoints rather than a range: the whole block is
 * twenty thousand glyphs and the menu prints eight.
 *
 * If a string is added with a character not listed, the atlas builder extends itself to include
 * it from the string tables - see nr_font_collect_cjk(), which walks the strings rather than
 * trusting this list to stay in step with them. */
/* The most cells the atlas will hold: ASCII printable plus room for the accented Latin and CJK a
 * menu in four languages needs. Bigger than any language set this menu has, and small enough that
 * the atlas is a few hundred kilobytes at worst. */
#define NR_FONT_MAX_CODEPOINTS 512

typedef struct {
    /* The atlas: 8-bit coverage, one byte a texel, row-major. */
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;

    /* Where each glyph is, so the draw code can ask for a codepoint and get a rectangle. */
    struct nr_font_cell {
        int32_t  codepoint;
        uint32_t x, y, w, h;
    } cells[NR_FONT_MAX_CODEPOINTS];
    int cell_count;
} nr_font;

/* Build the atlas from the system font. `pixel_height` is the em size - 16 is what the menu uses,
 * which is the smallest that stays readable over a game.
 *
 * `extra` is a NULL-terminated array of UTF-8 strings whose non-ASCII characters are added to the
 * atlas, or NULL. The caller passes the menu's own string tables, so the glyphs baked are the
 * glyphs printed - and a language added to those tables brings its characters with it.
 *
 * That last part is not a nicety. An earlier version walked a hand-written list of Chinese
 * strings, and the atlas came out with ASCII and Chinese and NOTHING ELSE: Russian and Spanish
 * were baked as blanks, which is a menu in the wrong characters rather than a crash. Passing the
 * tables is what makes "the atlas covers what the menu prints" true by construction.
 *
 * Returns 1 and fills `font` on success, 0 when GDI cannot give a glyph outline for anything
 * asked (a stripped Windows install, a font that failed to load). A zero return means no menu
 * rather than a menu with boxes in it: `nr_menu_create` treats it as "cannot draw" and says so. */
int nr_font_build(nr_font *font, int pixel_height, const char *const *extra);

void nr_font_free(nr_font *font);

/* The cell for a codepoint, or NULL. */
const struct nr_font_cell *nr_font_cell_for(const nr_font *font, int codepoint);

/* Whether the atlas holds it - the check that turns a missing glyph into a skipped character
 * rather than a blank one. */
int nr_font_covers(const nr_font *font, int codepoint);

/* Collect the CJK codepoints a UTF-8 string uses, appending them to `out` up to `max`, so the
 * atlas is built from the strings that exist rather than from a list beside them. Returns how
 * many were added. */
int nr_font_collect_cjk(const char *utf8, int *out, int count, int max);

#endif /* NR_MENU_FONT_H */

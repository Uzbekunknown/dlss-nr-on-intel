/* The menu's glyphs, rasterised at start-up with GDI. See nr_menu_font.h for why not a table.
 *
 * `GetGlyphOutlineW` with `GGO_BITMAP` returns a 1-bit-per-pixel bitmap of one glyph at one size,
 * which is exactly what the menu samples. The one subtlety is that it knows nothing about
 * advance widths: a glyph's bitmap is trimmed to its ink, so the caller has to ask for the advance
 * separately or every letter would sit on top of the last. `GetCharABCWidthsW` answers that, and
 * the two together are the whole font.
 *
 * The atlas is laid out as monospace cells even though the font is not, because the menu's list is
 * a column and a proportional font makes a column of values look ragged. Cells are sized to the
 * widest glyph, and each glyph is centred in its cell - the cheapest way to keep one rectangle a
 * character without carrying per-glyph metrics through the draw code.
 */
#include "nr_menu_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* The cell size comes from the header, so the menu's layout and the atlas cannot disagree about
 * it. Both files need the number; one of them owns it. */

static int cell_compare(const void *a, const void *b)
{
    const struct nr_font_cell *ca = (const struct nr_font_cell *)a;
    const struct nr_font_cell *cb = (const struct nr_font_cell *)b;
    return ca->codepoint - cb->codepoint;
}

#ifdef _WIN32

/* The font the menu uses. Segoe UI is on every Windows since Vista; the fallback is the
 * system's own, which is what a stripped install has. */
static HFONT make_font(int pixel_height)
{
    static const wchar_t *families[] = { L"Segoe UI", L"Microsoft YaHei UI", L"Microsoft YaHei",
                                         L"Tahoma", NULL };
    for (int i = 0; families[i]; ++i) {
        HFONT f = CreateFontW(-pixel_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, families[i]);
        if (!f) continue;
        /* Ask whether it resolved to something that can render a CJK glyph; a font that cannot
         * would build an atlas of boxes for half the menu. */
        HDC dc = CreateCompatibleDC(NULL);
        HGDIOBJ old = SelectObject(dc, f);
        wchar_t name[LF_FACESIZE] = {0};
        GetTextFaceW(dc, LF_FACESIZE, name);
        SelectObject(dc, old);
        DeleteDC(dc);
        if (name[0]) return f;
        DeleteObject(f);
    }
    return NULL;
}

static int glyph_bitmap(HDC dc, UINT cp, int cell_w, int cell_h, uint8_t *cell)
{
    GLYPHMETRICS gm;
    MAT2 mat = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};   /* identity, in FIXED */
    DWORD size = GetGlyphOutlineW(dc, cp, GGO_BITMAP, &gm, 0, NULL, &mat);
    if (size == GDI_ERROR || size == 0) return 0;

    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) return 0;
    if (GetGlyphOutlineW(dc, cp, GGO_BITMAP, &gm, size, buf, &mat) == GDI_ERROR) {
        free(buf);
        return 0;
    }

    /* `gm.gmBlackBoxX` is in pixels and each row is padded to a 4-byte boundary. The glyph's
     * origin is `gm.gmptGlyphOrigin`, measured from the cell's baseline. */
    int bw = (int)gm.gmBlackBoxX;
    int bh = (int)gm.gmBlackBoxY;
    if (bw <= 0 || bh <= 0) { free(buf); return 0; }
    int stride = ((bw + 31) / 32) * 4;

    /* Centre the ink in the cell; the baseline sits at three quarters of the cell height, which
     * is where a 16-pixel em puts it. */
    int baseline = cell_h * 3 / 4;
    int ox = (cell_w - bw) / 2;
    int oy = baseline - (int)gm.gmptGlyphOrigin.y;

    for (int y = 0; y < bh; ++y) {
        int ty = oy + y;
        if (ty < 0 || ty >= cell_h) continue;
        for (int x = 0; x < bw; ++x) {
            int tx = ox + x;
            if (tx < 0 || tx >= cell_w) continue;
            /* GGO_BITMAP is one bit a pixel, most significant bit leftmost. */
            if (buf[y * stride + x / 8] & (0x80u >> (x % 8)))
                cell[ty * cell_w + tx] = 255;
        }
    }
    free(buf);
    return 1;
}

/* Nothing built in: the caller passes the menu's own string tables, so the atlas covers exactly\n * what the menu prints. See the note on 
r_font_build in the header. */

#else

static int glyph_bitmap(void *dc, unsigned cp, int cell_w, int cell_h, uint8_t *cell)
{
    (void)dc; (void)cp; (void)cell_w; (void)cell_h; (void)cell;
    return 0;
}


#endif

int nr_font_collect_cjk(const char *utf8, int *out, int count, int max)
{
    if (!utf8) return count;
    const uint8_t *p = (const uint8_t *)utf8;
    while (*p && count < max) {
        int cp = *p, n = 1;
        if (cp >= 0xF0) { cp = p[0] & 0x07; n = 4; }
        else if (cp >= 0xE0) { cp = p[0] & 0x0F; n = 3; }
        else if (cp >= 0xC0) { cp = p[0] & 0x1F; n = 2; }
        if (n > 1) {
            int ok = 1;
            for (int i = 1; i < n; ++i) {
                if ((p[i] & 0xC0) != 0x80) { ok = 0; break; }
                cp = (cp << 6) | (p[i] & 0x3F);
            }
            if (!ok) { ++p; continue; }
        }
        p += n;
        if (cp < 0x80) continue;
        int seen = 0;
        for (int i = 0; i < count; ++i) if (out[i] == cp) { seen = 1; break; }
        if (!seen) out[count++] = cp;
    }
    return count;
}

int nr_font_build(nr_font *font, int pixel_height, const char *const *extra)
{
    if (!font) return 0;
    memset(font, 0, sizeof *font);

#ifdef _WIN32
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return 0;
    HFONT hf = make_font(pixel_height);
    if (!hf) { DeleteDC(dc); return 0; }
    HGDIOBJ old = SelectObject(dc, hf);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    /* Which codepoints to bake. ASCII printable first, then every non-ASCII character the menu's
     * own strings use - walked from the strings the caller passes, so a language added to them
     * brings its glyphs with it and one removed takes them away.
     *
     * wanted is sized for ASCII plus the most cells the atlas has room for; the collect call is
     * given that bound and stops there rather than writing past it. */
    int wanted[NR_FONT_MAX_CODEPOINTS];
    int want_count = 0;
    for (int cp = NR_FONT_FIRST_ASCII; cp <= NR_FONT_LAST_ASCII; ++cp)
        wanted[want_count++] = cp;
    for (int i = 0; extra && extra[i] && want_count < NR_FONT_MAX_CODEPOINTS; ++i)
        want_count = nr_font_collect_cjk(extra[i], wanted, want_count, NR_FONT_MAX_CODEPOINTS);

    /* The grid: as many cells a row as keep the atlas under the texture limit, which every
     * driver has at 4096 or more. */
    int cell_w = NR_FONT_CELL_W, cell_h = NR_FONT_CELL_H;
    int per_row = 64;
    while ((per_row * cell_w) > 4096) per_row /= 2;
    int rows = (want_count + per_row - 1) / per_row;
    uint32_t aw = (uint32_t)(per_row * cell_w);
    uint32_t ah = (uint32_t)(rows * cell_h);

    uint8_t *pixels = (uint8_t *)calloc((size_t)aw * ah, 1);
    if (!pixels) {
        SelectObject(dc, old); DeleteObject(hf); DeleteDC(dc);
        return 0;
    }

    int made = 0;
    for (int i = 0; i < want_count; ++i) {
        int cx = (i % per_row) * cell_w;
        int cy = (i / per_row) * cell_h;
        uint8_t cell[NR_FONT_CELL_W * NR_FONT_CELL_H];
        memset(cell, 0, sizeof cell);
        if (glyph_bitmap(dc, (UINT)wanted[i], cell_w, cell_h, cell)) {
            for (int y = 0; y < cell_h; ++y)
                memcpy(pixels + (size_t)(cy + y) * aw + cx, cell + y * cell_w, cell_w);
            made++;
        }
        if (font->cell_count < (int)(sizeof font->cells / sizeof font->cells[0])) {
            struct nr_font_cell *c = &font->cells[font->cell_count++];
            c->codepoint = wanted[i];
            c->x = (uint32_t)cx; c->y = (uint32_t)cy;
            c->w = (uint32_t)cell_w; c->h = (uint32_t)cell_h;
        }
    }

    SelectObject(dc, old);
    DeleteObject(hf);
    DeleteDC(dc);

    if (made == 0) { free(pixels); return 0; }

    /* Sorted so the lookup can binary-search; the draw path runs once a glyph a frame and a
     * linear scan over ninety would be the menu's most expensive line. */
    qsort(font->cells, (size_t)font->cell_count, sizeof font->cells[0], cell_compare);

    font->pixels = pixels;
    font->width = aw;
    font->height = ah;
    return 1;
#else
    (void)pixel_height;
    return 0;
#endif
}

void nr_font_free(nr_font *font)
{
    if (!font) return;
    free(font->pixels);
    font->pixels = NULL;
    font->cell_count = 0;
}

const struct nr_font_cell *nr_font_cell_for(const nr_font *font, int codepoint)
{
    if (!font || font->cell_count == 0) return NULL;
    int lo = 0, hi = font->cell_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int here = font->cells[mid].codepoint;
        if (here == codepoint) return &font->cells[mid];
        if (here < codepoint) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

int nr_font_covers(const nr_font *font, int codepoint)
{
    return nr_font_cell_for(font, codepoint) != NULL;
}

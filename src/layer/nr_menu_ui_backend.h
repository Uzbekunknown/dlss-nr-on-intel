/* What a renderer behind nr_menu_ui.h shares with the panel (nr_menu_ui.cpp). C++, private to the
 * UI's own files.
 *
 * The panel's handle is the start of each renderer's own state: nr_menu_ui_vk.cpp's and
 * nr_menu_ui_d3d12.cpp's structs derive from this one, and the plain-C interface hands the same
 * pointer to the panel's functions and to the renderer's. The panel reads only what is here.
 */
#ifndef NR_MENU_UI_BACKEND_H
#define NR_MENU_UI_BACKEND_H

#include <stddef.h>

struct ImDrawData;

struct nrex_ui {
    /* Between `nrex_new_frame` and the renderer's render: a frame ImGui has opened. */
    bool in_frame;
    /* The renderer's own step at the start of each frame, before ImGui's NewFrame, or NULL. */
    void (*backend_new_frame)(nrex_ui *ui);
    /* What the last rendered frame drew into, x0 y0 x1 y1 in image pixels, and whether it drew
     * anything: set by the renderer through `nrex_note_drawn`, read by `nrex_drawn_rect`. */
    int drawn[4];
    bool drawn_any;
};

/* The panel's ImGui context: the keyboard repeat, the look, and the font with the glyphs the four
 * languages need (nr_menu_ui.cpp). A renderer calls it once, before its own setup; it adopts
 * `font_bytes`. */
bool nrex_context_create(const char *font_bytes, size_t font_size, const int *codepoints,
                         int codepoint_count, const char **why);

/* The bounds of `draw`'s vertices over a `width` x `height` image, two pixels wider and inside the
 * image, into `ui->drawn` (nr_menu_ui.cpp). A renderer calls it after ImGui::Render, and draws and
 * blends no more than that. Returns whether anything was drawn. */
bool nrex_note_drawn(nrex_ui *ui, const ImDrawData *draw, unsigned width, unsigned height);

#endif /* NR_MENU_UI_BACKEND_H */

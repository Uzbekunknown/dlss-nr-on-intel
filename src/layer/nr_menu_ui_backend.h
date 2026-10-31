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

struct nrex_ui {
    /* Between `nrex_new_frame` and the renderer's render: a frame ImGui has opened. */
    bool in_frame;
    /* The renderer's own step at the start of each frame, before ImGui's NewFrame, or NULL. */
    void (*backend_new_frame)(nrex_ui *ui);
};

/* The panel's ImGui context: the keyboard repeat, the look, and the font with the glyphs the four
 * languages need (nr_menu_ui.cpp). A renderer calls it once, before its own setup; it adopts
 * `font_bytes`. */
bool nrex_context_create(const char *font_bytes, size_t font_size, const int *codepoints,
                         int codepoint_count, const char **why);

#endif /* NR_MENU_UI_BACKEND_H */

/* The C/C++ boundary for the ImGui-based menu.
 *
 * Dear ImGui is C++ and this layer is C, so the menu's UI lives in one C++ translation unit that
 * exports a plain-C interface prefixed `nrex_`. Everything on the C side of this header is what
 * nr_menu.c already had: the same knob rows, the same four languages, the same open/compare state -
 * only the drawing is different.
 *
 * Rendering model, and why an intermediate image:
 *
 *   ImGui's Vulkan backend draws through a graphics pipeline into a colour attachment. The
 *   swapchain image is already created by the game with only TRANSFER_DST and STORAGE usage - the
 *   layer cannot add COLOR_ATTACHMENT after the fact - so the UI is rendered into an image of the
 *   menu's own and copied onto the swapchain image with the transfer the layer already makes.
 *
 *   The intermediate is alpha-blended over the game's frame at copy time by sampling... it is not:
 *   a plain copy would black out the frame behind the panel. So the copy is done by a small
 *   compute shader that blends src over dst, which needs only the storage usage the image has. The
 *   pipeline that draws the UI still writes the intermediate as a colour attachment, which is the
 *   one usage this file owns outright.
 */
#ifndef NR_MENU_UI_H
#define NR_MENU_UI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The row the C side fills and the C++ side draws. Same shape nr_menu_bridge already produced for
 * the hand-drawn menu: a label, a shown value, and what kind of control it is. */
typedef struct nrex_row {
    const char *label;        /* the C side owns the string and its lifetime */
    const char *value;        /* the current value, shown on the right */
    const char *const *choices; /* for a choice row: the strings, terminated by NULL */
    int         choice_count;
    int         choice_index; /* for a choice row: which is selected */
    float       min, max, step; /* for a float row */
    int         kind;         /* nrex_kind_* below */
    int         hint_id;      /* which hint to show, or -1 */

    /* The one-line explanation, shown when the pointer rests on the row.
     *
     * OptiScaler does this - a tooltip on hover tells you what a knob does without leaving the
     * game - and it is the difference between a settings list you can use and a list of numbers
     * with names beside them. The C side owns the string and its lifetime, like `label`.
     *
     * NULL is fine and not an error: a row with nothing to explain shows no tooltip, which is what
     * the Info rows want. */
    const char *hint;

    /* FILLED BY THE UI when the user moves this row. The C side reads it after `nrex_end_panel`
     * for every row whose `changed` is set, and applies it through the bridge - so the menu owns
     * no setting, it only reports what the user asked for. Without this an edit has nowhere to
     * go: the widget's new value would live and die in the panel's stack frame. */
    int         changed;
    float       value_f;      /* for a float row: the new value */
    int         value_i;      /* for a choice or flag row: the new index */
} nrex_row;

enum {
    NREX_KIND_FLOAT = 0,      /* a slider from min to max, stepped */
    NREX_KIND_CHOICE,         /* a combo of choices */
    NREX_KIND_FLAG,           /* on/off */
    NREX_KIND_INFO,           /* display only: title, saved, hint */
};

enum {
    NREX_ACT_NONE = 0,
    NREX_ACT_CHANGED = 1,     /* some row's value moved; the C side reads back with nrex_get_* */
    NREX_ACT_SAVE = 2,        /* the user asked to save */
    NREX_ACT_RESET = 4,       /* the user asked for defaults */
    NREX_ACT_CLOSE = 8,       /* the user asked to close the panel */
    NREX_ACT_LANGUAGE = 16,   /* the panel's language selector moved */
};

/* Handle. The C side never looks inside. */
typedef struct nrex_ui nrex_ui;

/* Creation. The Vulkan objects are the layer's, already made for its own work; the UI borrows them
 * for the frames it draws and creates nothing on the game's device beyond its own images and
 * pipelines, which it destroys on shutdown.
 *
 *   format            the swapchain's format, so the intermediate image matches
 *   api_version       the instance's declared API version, for the backend's pipeline layout
 *   min_image_count   the swapchain's minimum image count
 *   image_count       the swapchain's image count
 *   load_fn           vkGetDeviceProcAddr, for everything device-level
 *   load_instance_fn  vkGetInstanceProcAddr, for the physical-device queries the device resolver
 *                     deliberately refuses (the loader's contract, learned the hard way)
 *   font_bytes/size   a TTF in memory; NULL uses ImGui's embedded proggy, which has no CJK
 *
 * Returns NULL and sets *why if the backend cannot start. */
nrex_ui *nrex_create(void *device, void *physical, void *instance, void *queue,
                     uint32_t queue_family, uint32_t format, uint32_t api_version,
                     uint32_t min_image_count, uint32_t image_count,
                     void *load_fn, void *load_instance_fn,
                     const char *font_bytes, size_t font_size,
                     const int *codepoints, int codepoint_count,
                     const char **why);

void nrex_destroy(nrex_ui *ui);

/* The frame's window. Call between nrex_new_frame and nrex_render. */
void nrex_begin_panel(nrex_ui *ui, const char *title, int language);
/* The language the panel's selector is on. The caller compares it with the branch's own and, when
 * they differ, asks for the new one through the menu - the same flag the L key sets. */
int nrex_panel_language(nrex_ui *ui);
/* Fills the panel from the rows. The C side keeps the arrays; the UI only reads. */
void nrex_rows(nrex_ui *ui, const nrex_row *rows, int count, int selected);
/* Ends the frame. Returns the OR of nrex_act_* the user performed this frame. */
int nrex_end_panel(nrex_ui *ui);

/* Frame boundaries. nrex_render records into the command buffer the caller submits. */
void nrex_new_frame(nrex_ui *ui, float display_w, float display_h, float delta_seconds);
void nrex_render(nrex_ui *ui, uint64_t cmd_handle);

/* The key state, fed by the C side's GetAsyncKeyState polling. ImGui derives edges itself. */
void nrex_key(nrex_ui *ui, int vk, int down);
void nrex_mouse(nrex_ui *ui, float x, float y, int down);

/* Whether the menu should draw its own pointer.
 *
 * The game owns the OS cursor and may hide it - ShowCursor is a per-process counter this layer
 * cannot reach - so a hidden cursor would leave the player aiming at nothing. With this set, ImGui
 * rasterises an arrow into the frame instead. The caller decides per frame, because the game shows
 * and hides its cursor as it likes.
 *
 * It draws NOTHING without a position, so pair it with `nrex_cursor_hint` unless the pointer is
 * inside the menu's image. See that function. */
void nrex_set_software_cursor(nrex_ui *ui, int on);

/* Where to draw the software cursor, in the UI image's pixels.
 *
 * `nrex_mouse` reports "no position" when the pointer is outside the menu's image, which is
 * deliberate - clamping would hover whatever sits at the edge and a click there would land on a
 * widget the pointer was never over. But ImGui also refuses to draw its own cursor without a
 * position, so the fallback arrow was invisible everywhere except over the panel, which is exactly
 * backwards: the pointer is most useful where there is no panel.
 *
 * This is the position for DRAWING only. Hit-testing keeps using the value from `nrex_mouse`, so a
 * hint never causes a widget to be hovered. */
void nrex_cursor_hint(nrex_ui *ui, float x, float y);

/* The intermediate image the UI rendered into this frame, for the caller's blend-copy. Valid until
 * the next nrex_new_frame. The C side has the storage-image copy it needs. */
void *nrex_image(nrex_ui *ui);
/* The image VIEW: the blend pass samples the UI through it. */
void *nrex_image_view(nrex_ui *ui);
uint32_t nrex_image_width(nrex_ui *ui);
uint32_t nrex_image_height(nrex_ui *ui);

/* Recreated when the swapchain changes size. */
int nrex_resize(nrex_ui *ui, uint32_t width, uint32_t height);

/* Language name for the tab the C side shows. */
const char *nrex_language_name(int language);

#ifdef __cplusplus
}
#endif

#endif /* NR_MENU_UI_H */

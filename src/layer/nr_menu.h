/* The in-game settings menu: what it is, where it draws, and what it will not do.
 *
 * **Where it draws.** At `vkQueuePresentKHR`, after NR has written its result and before the frame
 * reaches the driver. The image is the game's swapchain image, and the draw is a compute pass over
 * the panel's own rectangle - not a render pass. A render pass would need a framebuffer, an image
 * view and a load op against an image the application owns, with a guess at its usage flags and
 * layout; a layer that guesses wrong there breaks the game. A compute pass needs the image as a
 * storage image and nothing else, and the handful of quads a menu is made of are rasterised by a
 * rectangle test in `menu.comp`.
 *
 * **What it must not disturb.**
 *
 *   * *Semaphore ownership.* The layer consumes the game's present waits exactly once, in
 *     `transfer()`, and then presents with no waits at all (`present_now` patches them out when
 *     `waits_consumed` is set). The menu's submit must therefore wait on nothing and signal
 *     nothing - it runs on the same queue through the caller's own command buffer, and the barrier
 *     inside that recording is what orders it against the image. Adding a wait semaphore here
 *     would either consume the game's a second time or hand the presentation engine a semaphore it
 *     does not own; both are the class of bug this file exists to avoid.
 *
 *   * *The network's input and the temporal history.* The menu is drawn after NR's writeback, so
 *     what the network reads is the game's own frame and what is carried into the next frame is
 *     NR's result. No menu pixel reaches either.
 *
 *   * *Host readback.* Nothing here copies the image to host memory. The panel is composited on
 *     the GPU, in the caller's command buffer, whether the menu is open or closed.
 *
 *   * *A surface it cannot draw on.* `nr_menu_create` asks the physical device whether the
 *     swapchain format can be a storage image at all, and refuses when it cannot. A refusal is a
 *     missing menu, never a broken game: every entry point returns early on a NULL menu, and the
 *     caller treats it as "no menu this frame".
 */
#ifndef NR_MENU_H
#define NR_MENU_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque. One per device, created on the first present and kept for the process. */
typedef struct nr_menu nr_menu;

/* ------------------------------------------------------------------ languages */

/* English, Chinese, Russian and Spanish - see `nr_menu_strings.h`, which is generated from the
 * setup wizard's own tables so the two cannot disagree about a label. This order is the order the
 * language key cycles. */
enum {
    NR_MENU_LANG_EN = 0,
    NR_MENU_LANG_ZH,
    NR_MENU_LANG_RU,
    NR_MENU_LANG_ES,
    NR_MENU_LANG_COUNT
};

/* ------------------------------------------------------------------ compare */

/* The picture comparison, taken from the pre-SR project's own feature: the picture shown against
 * itself, so the two can be seen at once rather than toggled and remembered.
 *
 *   off           nothing drawn
 *   wipe          one frame cut at the split, nothing resampled - playable, and the split is a
 *                 stored value that stays where it was put
 *   side_by_side  the whole frame in each half, squeezed horizontally, for looking at rather than
 *                 playing in
 *
 * The wipe does not need a second copy of the frame. NR writes its result into the swapchain image
 * itself, so the image after writeback is the processed picture and the host-side `staging` buffer
 * holds the game's own frame from the same present. Passing that buffer as `reference` is what
 * makes the wipe a comparison of two frames rather than a cut in one.
 */
enum {
    NR_MENU_COMPARE_OFF = 0,
    NR_MENU_COMPARE_WIPE,
    NR_MENU_COMPARE_SIDE_BY_SIDE,
    NR_MENU_COMPARE_COUNT
};

/* ------------------------------------------------------------------ lifecycle */

/* How the menu reaches Vulkan.
 *
 * It must NOT link vulkan-1.lib and must not call `vk*` by name. The layer is handed every
 * function pointer by the loader, precisely so it binds no loader of its own - a layer that linked
 * one would be a second loader in the game's process, and on Windows the two would resolve to
 * whichever came first on the search path. `nr_layer.c` resolves each function it needs through
 * its own loader-provided pointers; the menu takes the same two.
 *
 * Two, not one, because Vulkan splits them: the physical-device queries are reached through the
 * instance, everything else through the device.
 *
 * Either returning NULL means the driver has no such function, which the menu treats as "cannot
 * draw" rather than crashing.
 *
 * **`NR_MENU_CALL` is `VKAPI_PTR`, spelled locally because this header does not include Vulkan.**
 * The functions being wrapped are `vkGetDeviceProcAddr` and `vkGetInstanceProcAddr`, which are
 * `__stdcall` on x86; a plain function-pointer type makes them `__cdecl`, and every call through
 * it leaves the two arguments on the stack - the same unbalanced-stack defect that crashed a
 * 32-bit game through `nrex_gpa_fn`. The signature is `void *` for the handle because these are
 * DISPATCHABLE handles (`VkDevice`, `VkInstance`), which are pointer-width on every architecture,
 * so nothing is truncated here. */
#if defined(_WIN32)
#  define NR_MENU_CALL __stdcall
#else
#  define NR_MENU_CALL
#endif
typedef void *(NR_MENU_CALL *nr_menu_resolve_fn)(void *handle, const char *name);

/* Build the menu for a device, if this swapchain's format can be a storage image.
 *
 * Returns NULL and leaves *why set (a static string, never freed) when it cannot - an unknown
 * format, no storage-image support for that format, no queue, no memory, a driver function
 * missing. The caller treats that as "no menu this frame", which is what makes a failure here a
 * missing feature rather than a broken game.
 *
 * `resolve_device` is `data->get_device_proc(device, name)`; `resolve_instance` is
 * `next_instance_proc(layer_instance, name)`.
 */
nr_menu *nr_menu_create(void *device, void *physical_device, uint32_t queue_family,
                        uint32_t format, nr_menu_resolve_fn resolve_device,
                        nr_menu_resolve_fn resolve_instance, const char **why);

void nr_menu_destroy(nr_menu *menu);

/* ------------------------------------------------------------------ per frame */

/* Read the keyboard and change whatever the user asked for.
 *
 * Called once a frame from the layer, before the composite. It reads only the keys the menu uses,
 * and while it is closed only the three that act without it: the key that opens the menu, the NR
 * toggle and the compare key. Everything else is polled only while it is open, so the game keeps
 * its own input.
 *
 * Returns:
 *   0  closed and nothing was touched - the fast path
 *   1  open and the frame should be composited
 *   2  the open/closed state changed, so the caller can log it once rather than every frame
 */
int nr_menu_update(nr_menu *menu, float frame_seconds);

int nr_menu_is_open(const nr_menu *menu);

/* Whether anything has to be drawn this frame: the panel is open, or a comparison is showing.
 *
 * The caller uses this to skip the whole submit when neither is true, which is every frame of
 * ordinary play. Polling the keys must still happen - the toggle and the compare key act with the
 * menu closed - but that is a few `GetAsyncKeyState` calls and no GPU work, so the two are
 * separate: `nr_menu_bridge_update` (or `nr_menu_update`) always runs, and this decides whether a
 * command buffer is worth opening. */
int nr_menu_needs_composite(const nr_menu *menu);

/* ------------------------------------------------------------------ what the keys asked for */

/* Each returns 1 once and clears itself, so a caller does one action per press. */

/* The toggle key was pressed: NR on or off. The caller flips the trigger. */
int nr_menu_take_toggle_request(nr_menu *menu);

/* Whether NR is on, as the menu last heard. The caller owns the trigger and sets this. */
void nr_menu_set_nr_enabled(nr_menu *menu, int enabled);
int  nr_menu_nr_enabled(const nr_menu *menu);

/* The compare key was pressed, or the menu changed the mode itself. */
int nr_menu_take_compare_change(nr_menu *menu, int *mode);
int nr_menu_compare_mode(const nr_menu *menu);

/* How far to move the split, -1..1, consumed on read. Zero when untouched. */
float nr_menu_take_split_step(nr_menu *menu);
void  nr_menu_set_split(nr_menu *menu, float split);   /* 0..1 across the image */
float nr_menu_split(const nr_menu *menu);

/* The save key. The caller writes the INI once and clears it. */
int nr_menu_take_save_request(nr_menu *menu);

/* The language was switched; the caller re-reads its strings. */
int nr_menu_take_language_change(nr_menu *menu, int *language);

/* The reset row was activated. */
int nr_menu_take_reset(nr_menu *menu);

/* ------------------------------------------------------------------ the rows */

/* One row, as the caller wants it shown. `label`, `value` and `hint` are borrowed for the frame
 * and are already in the current language: the menu does not know what a knob is, it draws rows.
 * That is what keeps the knob list in `nr_knobs.py` and out of C. */
typedef struct {
    const char *label;
    const char *value;
    const char *hint;      /* shown at the bottom for the selected row; may be NULL */
    int         is_choice; /* drawn with < > rather than - + */
    int         is_flag;   /* drawn as on / off */
} nr_menu_row;

void nr_menu_set_rows(nr_menu *menu, const nr_menu_row *rows, int count);
void nr_menu_set_title(nr_menu *menu, const char *title);
void nr_menu_set_hint(nr_menu *menu, const char *hint);
void nr_menu_set_note(nr_menu *menu, const char *note);

/* Which row the arrow keys are on. */
int nr_menu_selected(const nr_menu *menu);

/* The direction the selected row was asked to move: +1, -1 or 0, consumed on read. The menu owns
 * the cursor and the key reading; the caller owns what a change means. */
int nr_menu_take_step(nr_menu *menu);

/* ------------------------------------------------------------------ driving it without a keyboard */

/* Ask for what a key would have asked for.
 *
 * Two callers need this and neither is a keyboard. A test has no way to press INS in another
 * process, so without these the settings path cannot be exercised at all - and a suite that can
 * only check "nothing changed" is satisfied by a save that does nothing, which is the failure it
 * exists to catch. And a headless or scripted run wants to move a knob without a window.
 *
 * These set the same flags the key reading sets, so a test drives exactly the path a keypress
 * does: `nr_menu_take_step` and `nr_menu_take_save_request` are what the caller reads either way.
 * They are not a back door into the menu's state - the values still live in the bridge, and the
 * file is still written by the same code. */
void nr_menu_inject_step(nr_menu *menu, int direction);
void nr_menu_inject_save(nr_menu *menu);
/* The effect toggle, as the Home key raises it.
 *
 * A flag rather than a direct flip of the trigger, because the trigger is the layer's and the
 * bridge is what turns this request into a file write - the same path the button and the key
 * already share. The ImGui build reads Home at the layer's level (see `menu_read_keys`), so
 * this exists to give that read somewhere to land. */
void nr_menu_inject_toggle(nr_menu *menu);
/* Whether this file reads the panel's keys itself. Off when ImGui owns the panel: the two
 * readers would otherwise both act on INS, the arrows and Enter, which is one keypress processed
 * twice. The always-on hotkeys (NR on/off, the comparison) are unaffected - they are read by the
 * bridge and act with the panel closed. */
/* The layer's one call per frame: the level of every virtual key, 0..255, behind one focus check.
 *
 * This file used to read `GetAsyncKeyState` itself, which made it the third place keys were read
 * from. Now `nr_layer.c` is the only reader - it polls once, gates once, and hands the levels in
 * here. A 256-byte array rather than a list of the keys the menu uses: the state machine names its
 * own keys, and a list would be a second place to keep in step with it. NULL means "every key up". */
void nr_menu_set_key_levels(nr_menu *menu, const unsigned char *levels);

void nr_menu_set_key_reading(nr_menu *menu, int on);

/* Forget which keys are held, so the next press is an edge again.
 *
 * `key_pressed` latches the level in `m->key_down[]` on every call, INCLUDING when the caller
 * throws the result away - which is what a focus gate does. So a Home held while the game is in
 * the background is recorded as "was down", and the first press after returning to the game is
 * not an edge and does nothing. The gate would trade "the key acts when it should not" for "the
 * key is dead for one press", which is the worse bug.
 *
 * The layer calls this whenever it stops reading, which is the same place it already re-arms its
 * own latches (`nr_layer.c`'s `menu_ui_*_was_down`) for exactly this reason. */
void nr_menu_release_keys(nr_menu *menu);

void nr_menu_inject_reset(nr_menu *menu);

/* Revert to the SAVED settings, where Reset goes to the code's defaults. The caller polls it
 * with `nr_menu_take_revert`, the same way it polls reset. */
void nr_menu_inject_revert(nr_menu *menu);
int  nr_menu_take_revert(nr_menu *menu);

/* Ask for a language, the way the L key does. It sets the same flag the key sets, so the bridge
 * picks it up on its next update and every row is re-translated - there is no second path that
 * could translate differently. */
void nr_menu_inject_language(nr_menu *menu, int language);

/* Set the language WITHOUT raising the change flag.
 *
 * The bridge owns the setting - it reads `menu_language` from the file and writes it back - so it
 * has to be able to tell the menu which language is showing without that coming back as a fresh
 * request to save it. `nr_menu_inject_language` is the other direction: a user asking for a change,
 * which the bridge then persists.
 *
 * Using the injector for this made every refresh queue a language write, and
 * `nr_menu_bridge_update` acts on that queue - so a refresh followed by an update rewrote the
 * settings file's `menu_language` on a frame where nothing had changed. */
void nr_menu_set_language(nr_menu *menu, int language);
/* The language the menu is showing, so a caller that cycles with L can take the next one. The
 * injector above takes an absolute index and there is no "advance by one" among the menu's own
 * controls, so the value has to be readable for L to mean anything. */
int  nr_menu_language(const nr_menu *menu);
void nr_menu_inject_select(nr_menu *menu, int row);
/* Open or close the panel, as INS would. A knob can only be moved while the panel is open - that
 * is the design, not a limitation - so a caller driving the settings without a keyboard has to
 * open it first, exactly as a person would. */
void nr_menu_inject_open(nr_menu *menu, int open);

/* ------------------------------------------------------------------ drawing */

/* Composite the menu over `image`, which must be in the format the menu was created for and in a
 * layout the caller has already transitioned for writing.
 *
 * The Vulkan handles are `uint64_t` rather than `void *` because that is what they are: a
 * `VkImage` is an opaque non-dispatchable handle, which on a 64-bit build is a 64-bit integer and
 * NOT a pointer. Declaring the parameter `void *` compiles - the two are the same width - and
 * then mismatches the prototype at every call site, which MSVC reports as C4022 "pointer differs
 * in parameter 3" and a stricter compiler rejects. Taking the integer type says what is actually
 * passed.
 *
 * `cmd` is the caller's command buffer, already recording: the menu never begins, ends or submits
 * one, because the submit path is where the semaphore contract lives and the caller owns it. */
void nr_menu_draw(nr_menu *menu, uint64_t cmd, uint64_t image, uint32_t width, uint32_t height);

/* Draw the picture comparison without the menu panel.
 *
 * Called from the same place as `nr_menu_draw` and independently of whether the menu is open - the
 * comparison is a stored setting and keeps working once the menu is closed, which is the point of
 * it.
 *
 * `reference` is a device buffer holding the game's own frame for this present, or 0 for "cut the
 * image against itself". Passing it is what makes the wipe a comparison of two frames. */
/* Blends the ImGui UI's image onto the game's image, whole-frame.
 *
 * `ui_view` is the UI's image view; the shader samples it through the `reference` binding and
 * composites it over the target with straight alpha, so a transparent pixel is the game's own
 * frame and the panel sits on top. This is the ImGui path's equivalent of `nr_menu_draw`, and it
 * is the reason the UI's image exists at all: the swapchain image has no colour-attachment usage,
 * so ImGui cannot draw into it directly. */
void nr_menu_draw_overlay(nr_menu *m, uint64_t cmd_handle, uint64_t target_handle,
                          uint64_t ui_view_handle, uint32_t width, uint32_t height);

void nr_menu_draw_compare(nr_menu *menu, uint64_t cmd, uint64_t image, uint64_t reference,
                          uint32_t width, uint32_t height);

/* ------------------------------------------------------------------ the font */

/* Whether the atlas was built. A menu without one draws nothing and says why. */
int nr_menu_font_ready(const nr_menu *menu);

#ifdef __cplusplus
}
#endif

#endif /* NR_MENU_H */

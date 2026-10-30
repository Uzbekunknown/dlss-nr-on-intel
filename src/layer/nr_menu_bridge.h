/* The seam between the menu and the settings.
 *
 * The menu draws text and reports "row 3 moved up one". Something has to know that row 3 is
 * `render_scale`, that its range is 0.05 to 1.0, and that the answer belongs in the settings file
 * the daemon is polling. That is this file.
 *
 * **Where the knobs are defined.** Once, in `nr_knobs.py`, and mirrored here because the menu draws
 * inside the game's process with no interpreter in it. The mirror is checked: the tests compare
 * this table against `nr_knobs.KNOBS` through the two accessors at the bottom, and fail when one
 * gains a knob the other does not have or a range differs. That check exists because the copies
 * already drifted once - `render_scale` defaulted to 0.55 here and 1.0 in `nr_knobs.py` - and
 * nothing said so.
 *
 * **Why the file and not the daemon.** The daemon is a separate process behind a socket that
 * carries frames. Asking it to move a knob would put settings behind that protocol, so the menu
 * would stop responding exactly when a daemon is busy - which is when somebody opens a settings
 * menu. The settings file is the interface the tools, the daemon and the layer already share.
 */
#ifndef NR_MENU_BRIDGE_H
#define NR_MENU_BRIDGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nr_menu_bridge nr_menu_bridge;
typedef struct nr_menu nr_menu;

/* ------------------------------------------------------------------ lifecycle */

/* `settings_path` is the file `nr_paths.SETTINGS` names - the daemon's own path, which the layer
 * already resolves to spawn the daemon. `language` is one of NR_MENU_LANG_*.
 *
 * Returns NULL only when there is no menu to attach to: a bridge without a menu has nothing to
 * draw and nothing to read keys from. */
nr_menu_bridge *nr_menu_bridge_create(nr_menu *menu, const char *settings_path, int language);

/* A bridge with no menu, for a caller that has no window and no device.
 *
 * The settings logic and the drawing are separable, and this is the seam: a headless run, a
 * scripted run and the settings tests all want to move a knob and write the file without a
 * swapchain. Passing a hand-made stand-in to `nr_menu_bridge_create` instead is what a first
 * attempt at this did, and it crashed - the bridge calls `nr_menu_set_rows` and friends on whatever
 * it is given, and those write through a `nr_menu` whose real size is private to nr_menu.c.
 *
 * A bridge from here has `nr_menu_bridge_update` doing everything except the key reading: the
 * caller drives it with `nr_menu_inject_*` on the menu it does not have, so instead those calls
 * are made through the bridge's own entry points below. */
nr_menu_bridge *nr_menu_bridge_create_headless(const char *settings_path, int language);

/* Move the selected knob, as an arrow key would. `row` is an index into `nr_knobs.py`'s order.
 * Only meaningful on a headless bridge - one with a menu takes its rows from the keyboard. */
int nr_menu_bridge_move(nr_menu_bridge *bridge, int row, int direction);

/* The bridge's own menu, for a caller that has to inject a request into it.
 *
 * A headless bridge has no keyboard and no window, so the only way a test can ask for Reset or for
 * a language change is to reach the menu the bridge is holding and use `nr_menu_inject_*` on it -
 * the same entry points the layer uses when it drives the menu without a keypress. Returns NULL for
 * a bridge with no menu (the ones `nr_menu_bridge_create_headless` builds), so a caller that only
 * wants to read rows does not need this at all. */
nr_menu *nr_menu_bridge_menu(nr_menu_bridge *bridge);

/* Set the language on a bridge with no menu. Applied by the next `nr_menu_bridge_update`.
 *
 * A bridge with a menu takes its language from `nr_menu_inject_language` instead - the key and the
 * combo both go through the menu, and that is what keeps them agreeing. This exists for the
 * headless case, which has no menu to inject into. */
void nr_menu_bridge_set_language(nr_menu_bridge *bridge, int language);

/* Close the panel, keeping what the user changed.
 *
 * The user's rule: close the panel, keep the unsaved changes, do not reset, do not save. The values
 * stay where they are, the preview keeps them so the picture keeps them too, and the settings file
 * is untouched. With a menu present this routes through `nr_menu_inject_open`, so the Close button
 * and the INS key cannot diverge about what closing means. */
void nr_menu_bridge_close(nr_menu_bridge *bridge);

/* Reset the knobs to their defaults, on a bridge with no menu.
 *
 * Preview only, like the Reset button: the values on screen go back to the defaults and the saved
 * settings file is not touched, so a reset cannot destroy what the user saved. With a menu present
 * this routes through `nr_menu_inject_reset` so the key and this call cannot diverge. */
void nr_menu_bridge_reset(nr_menu_bridge *bridge);

/* Revert to the SAVED settings, on a bridge with no menu. Through `nr_menu_inject_revert` when
 * there is one, so a button and this call cannot diverge. */
void nr_menu_bridge_revert(nr_menu_bridge *bridge);

/* What Save should write for the effect, and what the settings file last said.
 *
 * The effect itself is the layer's trigger file, not a setting the bridge owns - the layer passes
 * its live state to the panel every frame. These two only carry the value across a Save, so the
 * next launch can restore it. */
void nr_menu_bridge_set_nr_saved(nr_menu_bridge *bridge, int on);

/* The layer's own pacing: whether the network runs ahead of the game, whether the frame stays on
 * the GPU, and how many presents each result covers.
 *
 * These are NOT settings the daemon reads - they are the proxy's `nr-env.txt`, which the LAYER
 * reads once when the game starts. So they are "next launch" settings by construction: the panel
 * shows and writes them, and nothing changes until the game restarts. The setup window writes the
 * same file, and both leave every line they are not changing alone.
 *
 * `available` is 0 when the installation is not shaped the way the panel expects, in which case it
 * must offer none of them rather than edit a file it cannot identify. */
/* The three pacing rows, and then the network.
 *
 * `NR_PACING_PRESET` is not pacing: it is `NR_PRESET`, A or B. It shares this entry point because
 * it shares the plumbing - a value that lives in `nr-env.txt` beside the game, written the moment
 * it changes and read by the layer at the next start - and because a second entry point that did
 * the same writes would be a second place for them to disagree. */
enum { NR_PACING_ASYNC = 0, NR_PACING_GPU_IMAGE = 1, NR_PACING_EVERY = 2, NR_PACING_PRESET = 3 };

/* Name the proxy's environment file instead of taking it from beside this library.
 *
 * The production path is "beside this DLL", because that is where the installer puts
 * `nr-env.txt` and what the proxy reads. A headless fixture is a test executable with no
 * installation around it, so it names the file directly; NULL restores the default. */
void nr_menu_bridge_use_env_file(const char *path);

int nr_menu_bridge_pacing_available(const nr_menu_bridge *bridge);

/* Which network runs: 0 for A, 1 for B. Read from `nr-env.txt` with the pacing rows, so the panel
 * shows what the layer will actually load rather than what the settings file happens to say. */
int nr_menu_bridge_preset(const nr_menu_bridge *bridge);
int nr_menu_bridge_pacing_async(const nr_menu_bridge *bridge);
int nr_menu_bridge_pacing_gpu_image(const nr_menu_bridge *bridge);
int nr_menu_bridge_pacing_every(const nr_menu_bridge *bridge);

/* Set one of them and write the file. Applies the conflicts the setup window states: the GPU
 * image path requires asynchronous mode OFF and every frame, so turning it on turns both of those
 * off, and turning either of those on turns the GPU path off. Returns 1 when the file was written.
 *
 * The setup window's `work/windows-profile.json` is updated with the same three values, so the
 * file stays the panel's own and the profile follows it. See `write_profile_pacing`. */
int nr_menu_bridge_set_pacing(nr_menu_bridge *bridge, int what, int value);

/* Why the GPU image path will not run, in the words the setup window uses, or NULL when it will.
 *
 * This is the half of the refusal the PANEL can see by itself: the daemon also declines a frame
 * whose `detail_strength` or `colour_strength` is not 1 ("GPU composition requires detail and
 * colour strength 1"), and that reason goes to the daemon's log, where nobody looks. A row that
 * reads "on" while the fallback is silently in use is the same shape as issue #17, so the row
 * says which of the two settings is holding it back.
 *
 * Returns NULL when the path is off, because then there is nothing to explain. */
const char *nr_menu_bridge_pacing_gpu_blocker(const nr_menu_bridge *bridge, int language);
int  nr_menu_bridge_nr_saved(const nr_menu_bridge *bridge);

/* Which row the cursor is on, and where it is. */
int nr_menu_bridge_row_count(const nr_menu_bridge *bridge);
const char *nr_menu_bridge_row_label(const nr_menu_bridge *bridge, int row, int language);
const char *nr_menu_bridge_row_value(const nr_menu_bridge *bridge, int row);

/* The one-line explanation for a knob, in `language`, or NULL when there is none.
 *
 * This is what the overlay shows when the pointer rests on a row - OptiScaler's tooltip, which
 * tells you what a knob does without leaving the game. The strings come from the wizard's `Hint.*`
 * through the generated table, so the installer and the overlay explain a knob with the same words
 * in the same language.
 *
 * `row` is an index into `nr_knobs.py`'s order, and the returned string is owned by the bridge's
 * settings table - it stays valid until the next `nr_menu_bridge_refresh`. */
const char *nr_menu_bridge_row_hint(const nr_menu_bridge *bridge, int row, int language);

/* Write the file now, as the save key would. Returns 1 when it was written. */
/* Set the knob at `row` to an absolute value, as a slider does.
 *
 * The value is clamped to the knob's range and snapped to its step by the same code the arrow keys
 * go through, so the two ways of setting a knob cannot disagree about what it accepts. A float knob
 * takes `value`; a choice or flag knob takes `index` as `value`. Writes the settings file, like a
 * move does. Returns 1 when something changed. */
int nr_menu_bridge_set(nr_menu_bridge *bridge, int row, float value);

int nr_menu_bridge_save(nr_menu_bridge *bridge);
void nr_menu_bridge_destroy(nr_menu_bridge *bridge);

/* Rebuild the row text from the current values. Called on any change and on a language switch. */
void nr_menu_bridge_refresh(nr_menu_bridge *bridge);

/* ------------------------------------------------------------------ one frame */

/* Poll the keys, act on what they asked for, and pick up an edit made outside the game.
 *
 * Returns the menu's own state from `nr_menu_update`: 0 closed and untouched, 1 open, 2 the open
 * or closed state changed - so the caller can log the transition once rather than every frame.
 *
 * A knob change is written to the settings file immediately, because the daemon polls that file
 * and that write is what moves the picture. The save key is separate: it exists to keep the values
 * in the commented INI, not to apply them.
 *
 * The NR toggle and the compare key act while the menu is closed. The caller reads those with
 * `nr_menu_take_toggle_request` and `nr_menu_take_compare_change` on the menu handle, because the
 * action they describe is the caller's - flipping the layer's trigger is not the menu's to do. */
int nr_menu_bridge_update(nr_menu_bridge *bridge, float frame_seconds);

/* Whether something happened that the caller should act on. */
int  nr_menu_bridge_dirty(const nr_menu_bridge *bridge);
void nr_menu_bridge_clear_dirty(nr_menu_bridge *bridge);

/* Which language the menu is showing. */
int  nr_menu_bridge_language(const nr_menu_bridge *bridge);

/* The settings key the cursor is on, for a caller that wants to log it or drive it from a test.
 * NULL when there is no bridge. */
const char *nr_menu_bridge_selected_key(const nr_menu_bridge *bridge);

/* ------------------------------------------------------------------ the knob table, for the check */

/* How many knobs this table has. */
int nr_menu_bridge_knob_count(void);

/* The name of the nth knob, in `nr_knobs.py`'s spelling, or NULL when out of range. A test
 * compares this list against `nr_knobs.KNOBS` so a knob added to one and forgotten in the other
 * fails rather than silently missing from the menu. */
const char *nr_menu_bridge_knob_name(int index);

/* The range and step of the nth knob, when it is a number. Returns 0 for a choice or an index out
 * of range, so the test can skip the ones that have no range. */
/* A choice knob's options, for a UI that has to build a list rather than step one at a time.
 *
 * `nr_menu_bridge_knob_choice_count` is **0 for a numeric knob** and the option count for a choice
 * one, which is how a caller tells the two apart without duplicating the knob table. (The internal
 * stepper's own count returns 1 as a minimum, because it takes a modulo by it; that is not the
 * question this answers.)
 *
 * `_choice_name` writes into the caller's buffer - `size` must be at least
 * NR_KNOB_CHOICE_NAME_SIZE - so a caller collecting several names gets several distinct strings.
 * `_choice_index` is the option currently set, or 0 for a numeric knob.
 *
 * **96, not 32.** The raw names fit in 12 (`cc_small_fp8`), which is why 32 looked generous, but
 * the buffer now holds a LABEL: "A: HT, the model this project reproduces" is 40 bytes, the
 * Russian is 75, and the Chinese one is 36 - and Chinese is 3 bytes a character, so a 31-byte cut
 * can land inside one and produce invalid UTF-8 that ImGui renders as replacement glyphs. Measured
 * across all four languages: every one of the eight labels exceeds 31 bytes, so the panel was
 * showing "A: HT, the model this project r". Nothing crashed and nothing was written wrong, which
 * is exactly why it needed measuring rather than looking at.
 *
 * The count of characters is what misleads here, in the same way it did for the cadence row: the
 * English label is 41 characters and 40 bytes, the Russian is 43 and 75. Size in bytes. */
#define NR_KNOB_CHOICE_NAME_SIZE 96
int nr_menu_bridge_knob_choice_count(int index);
/* The option's LABEL for the panel's list, in `language` - not the raw name. The raw name is what
 * the daemon resolves and what the settings file stores, and the write paths keep using it; this
 * is display only. A value with no translated label falls back to the raw name. */
void nr_menu_bridge_knob_choice_name(int index, int which, int language, char *out, size_t size);
int nr_menu_bridge_knob_choice_index(const nr_menu_bridge *bridge, int index);

int nr_menu_bridge_knob_range(int index, float *low, float *high, float *step,
                              float *default_value);

#ifdef __cplusplus
}
#endif

#endif /* NR_MENU_BRIDGE_H */

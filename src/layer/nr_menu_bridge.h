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
 * `_choice_index` is the option currently set, or 0 for a numeric knob. */
#define NR_KNOB_CHOICE_NAME_SIZE 32
int nr_menu_bridge_knob_choice_count(int index);
void nr_menu_bridge_knob_choice_name(int index, int which, char *out, size_t size);
int nr_menu_bridge_knob_choice_index(const nr_menu_bridge *bridge, int index);

int nr_menu_bridge_knob_range(int index, float *low, float *high, float *step,
                              float *default_value);

#ifdef __cplusplus
}
#endif

#endif /* NR_MENU_BRIDGE_H */

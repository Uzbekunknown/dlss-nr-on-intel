/* Keeping the panel's keys and clicks from the game.
 *
 * The panel reads the keyboard with `GetAsyncKeyState` and the mouse as raw input, and both of those
 * see the hardware whatever the game's window is told - so what the panel took, the game took too:
 * an arrow moved the selection and the player, a click on a slider was a click in the game (Mortal
 * Kombat 11, 2026-10-31). While the panel is open the game is kept from its input in two ways, one
 * for each road input takes into a game:
 *
 *   - **The window's messages** (this file). The game's top-level window is subclassed the first
 *     time the panel opens, and while it is open the game's window procedure is not called for a
 *     key going down, a typed character, the pointer moving, a button going down, the wheel or
 *     `WM_INPUT`. A key or button the game saw go down still gets its release, so nothing is held
 *     down behind the panel; one it never saw go down gets none, so an action bound to a release
 *     does not fire either. That covers a game that takes its input as messages - Unreal does, its
 *     keys through `WM_KEYDOWN` and its mouse through `WM_INPUT`.
 *   - **The raw-input registration** (`nr_raw_mouse.c`). While the panel holds the pointer the
 *     process's mouse and keyboard registrations are ours, on a window of our own, and the game's
 *     are put back after; so a game that drains raw input with `GetRawInputBuffer` instead of
 *     taking `WM_INPUT` gets none either.
 *
 * **Not covered**, because only hooking the functions themselves would: a game that polls
 * `GetAsyncKeyState`, `GetKeyState` or `GetKeyboardState`, reads the pointer with `GetCursorPos`,
 * uses DirectInput, or reads a gamepad (XInput, GameInput). ReShade, OptiScaler and DLSSNR-AMD detour
 * user32 for those; the owner chose not to (2026-10-31). The panel draws nothing for a gamepad, so a
 * pad's input reaching the game is the game's own.
 *
 * The subclass stays on the window for the rest of the process, passing every message through while
 * the panel is shut, and the module that holds it is pinned in memory: a window procedure in a
 * library the Vulkan loader later unloads would be called after it is gone. Windows only; elsewhere
 * these are empty.
 */
#ifndef NR_INPUT_GUARD_H
#define NR_INPUT_GUARD_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32

/* Follow `window`, the game's window, and keep its input from it while `hold` is set. Called every
 * frame; the subclass goes on the first frame that holds, and a window that changes is followed. A
 * NULL window holds nothing. */
void nr_input_guard(void *window, int hold);

/* Hold nothing, and take the subclass off every window where it is still the window's procedure
 * (where something subclassed the window after it, it stays, passing everything through). */
void nr_input_guard_release(void);

#else

static inline void nr_input_guard(void *window, int hold) { (void)window; (void)hold; }
static inline void nr_input_guard_release(void) { }

#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* NR_INPUT_GUARD_H */

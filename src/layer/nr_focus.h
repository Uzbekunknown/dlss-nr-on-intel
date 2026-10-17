/* The desktop the menu lives in: which window in front means "the user is here".
 *
 * A Vulkan layer is a guest inside somebody else's process. It has no window, no message pump and
 * no `WndProc` to subclass - so the two things a normal overlay learns from messages, "am I
 * focused" and "where is the pointer", both have to be asked for directly.
 *
 * **Why this is its own file.** OptiScaler learns focus from `WM_SETFOCUS`/`WM_KILLFOCUS` because it
 * subclasses the game's window procedure (OptiScaler `menu/input/input_system_messages.cpp`). A
 * layer cannot, so the same question is answered by comparing `GetForegroundWindow` against the
 * window this process owns. That decision - what "in front" means, and which windows count as ours
 * - is the one thing here worth testing on its own, so it lives where a test can call it rather
 * than somewhere inside a 3000-line layer source.
 *
 * The rule matters more than it looks. Everything the panel receives is system-wide: the cursor
 * position, the button state, every key. Without a focus test the panel is driven by whatever the
 * user is doing in ANY window - which is exactly the report that produced this file: dragging a
 * progress bar in one application moved a slider in the panel, clicking a title bar in another
 * moved the panel itself.
 */
#ifndef NR_FOCUS_H
#define NR_FOCUS_H

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32

/* Whether `h` belongs to this process. A game may present from a child or a sibling window, so the
 * ancestor chain is compared as well - the same reason `window_is_ours` in the layer exists. */
int nr_window_is_ours(void *h);

/* Whether the window the user is working in right now is one of ours.
 *
 * FALSE for a null handle, and that direction matters: "no window found" must not read as "focused",
 * or the panel would take input before the game's window exists. */
int nr_window_is_foreground(void *hwnd);

#else

static inline int nr_window_is_ours(void *h) { (void)h; return 1; }
static inline int nr_window_is_foreground(void *hwnd) { (void)hwnd; return 1; }

#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* NR_FOCUS_H */

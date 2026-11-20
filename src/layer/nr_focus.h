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

/* While the panel holds the cursor in its one-pixel box: a thread of its own that lets go of the
 * clip the moment `hwnd` is not in front, whether or not the game is still presenting. Start may be
 * called every frame; stop waits for the thread to end, so none of this runs once the layer is gone. */
void nr_clip_guard_start(void *hwnd);
void nr_clip_guard_stop(void);

/* Whether the guard has seen another window in front since it started or since the last call, which
 * clears it. The layer runs on the game's frames, and a game may present none while it is not in
 * front - covered by a screenshot's overlay, say - so the layer can find the game in front on every
 * frame it sees and still have lost the focus in between; the panel then takes the pointer afresh,
 * as after any change of focus. */
int nr_clip_guard_left(void);

#else

static inline int nr_window_is_ours(void *h) { (void)h; return 1; }
static inline int nr_window_is_foreground(void *hwnd) { (void)hwnd; return 1; }
static inline void nr_clip_guard_start(void *hwnd) { (void)hwnd; }
static inline void nr_clip_guard_stop(void) { }
static inline int nr_clip_guard_left(void) { return 0; }

#endif /* _WIN32 */

/* The keyboard's one reader, and the clock the panel's frames are timed on.
 *
 * They came here from `nr_layer_state.h` when the panel's logic moved into `nr_panel.c`, which the
 * DirectX 12 proxy builds as well, and which therefore cannot include the Vulkan layer's state. A
 * key is still read in exactly one place, behind the focus gate in `nr_panel.c`: this macro is the
 * one `GetAsyncKeyState` the panel's sources hold. */
#ifdef _WIN32
#define nr_key_down(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
#else
#define nr_key_down(vk) (0)
#endif

#ifndef _WIN32
#include <time.h>
#endif

static inline double nr_now_seconds(void)
{
#ifdef _WIN32
	static int have_frequency;
	static LARGE_INTEGER cached;
	LARGE_INTEGER counter;
	if (!have_frequency) {
		QueryPerformanceFrequency(&cached);
		have_frequency = 1;
	}
	if (QueryPerformanceCounter(&counter) && cached.QuadPart > 0)
		return (double)counter.QuadPart / (double)cached.QuadPart;
	return 0.0;
#else
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
	return 0.0;
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* NR_FOCUS_H */

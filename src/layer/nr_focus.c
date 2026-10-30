/* The desktop the menu lives in. See nr_focus.h for why this is separate from the layer. */
#include "nr_focus.h"

#ifdef _WIN32

int nr_window_is_ours(void *h)
{
	HWND w = (HWND)h;
	if (!w) return 0;
	DWORD pid = 0;
	GetWindowThreadProcessId(w, &pid);
	return pid == GetCurrentProcessId();
}

int nr_window_is_foreground(void *hwnd)
{
	HWND h = (HWND)hwnd;
	if (!h) return 0;
	HWND fg = GetForegroundWindow();
	if (!fg) return 0;
	/* The same window, or one sharing a root: a game may take focus on a child while presenting
	 * from the top-level one, and that is still "the user is in the game". */
	if (fg == h || GetAncestor(fg, GA_ROOT) == GetAncestor(h, GA_ROOT)) return 1;
	/* Otherwise it has to be this process. A launcher, a crash reporter or an overlay window that
	 * belongs to the game's process is still the game as far as the panel is concerned - and
	 * comparing by process is what makes that true without enumerating anything. */
	DWORD fg_pid = 0;
	GetWindowThreadProcessId(fg, &fg_pid);
	return fg_pid == GetCurrentProcessId();
}

#endif /* _WIN32 */

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

/* The clip the panel holds on the cursor, let go of as soon as the game is not in front.
 *
 * The layer runs only when the game presents, and a game that pauses when it loses the focus
 * presents nothing. The clip is system-wide, so it stayed on the panel's one pixel over whatever
 * the user had turned to - the Start menu, a screenshot's selection - and the mouse could do
 * nothing there (Mortal Kombat 11, 2026-10-31). This thread only releases: taking the cursor back
 * stays the layer's, on the game's next frame in front. */
static volatile LONG g_guarding;
static HWND volatile g_guarded;
static HANDLE g_guard;

static DWORD WINAPI clip_guard(LPVOID unused)
{
	(void)unused;
	while (g_guarding) {
		if (!nr_window_is_foreground(g_guarded)) ClipCursor(NULL);
		Sleep(25);
	}
	return 0;
}

void nr_clip_guard_start(void *hwnd)
{
	g_guarded = (HWND)hwnd;
	if (g_guard) return;
	InterlockedExchange(&g_guarding, 1);
	g_guard = CreateThread(NULL, 0, clip_guard, NULL, 0, NULL);
	if (!g_guard) InterlockedExchange(&g_guarding, 0);
}

void nr_clip_guard_stop(void)
{
	if (!g_guard) return;
	InterlockedExchange(&g_guarding, 0);
	WaitForSingleObject(g_guard, 1000);
	CloseHandle(g_guard);
	g_guard = NULL;
}

#endif /* _WIN32 */

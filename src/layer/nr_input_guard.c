/* Keeping the panel's keys and clicks from the game's window. See nr_input_guard.h. */
#include "nr_input_guard.h"

#ifdef _WIN32

#include <windows.h>

#include "nr_focus.h"

/* The windows subclassed so far, with the procedure each had before. A game has one; the room for
 * more is for a game that recreates its window, since the subclass on the old one stays wherever
 * something else subclassed it after us. Written under the lock, read under it by the procedure. */
#define GUARDED 8
static SRWLOCK g_lock = SRWLOCK_INIT;
static struct { HWND window; WNDPROC previous; } g_guarded[GUARDED];

/* The window whose input is being kept from it, or NULL while the panel is shut. */
static HWND volatile g_held;

/* What the game saw go down and has not seen come up yet, and what went down while it was kept from
 * the game: keys by virtual-key code, the five buttons as bits (left, right, middle, X1, X2). A
 * release is the game's when it saw the press, and not when the press was kept from it - even after
 * the panel has shut, which is when the release of the key or the click that shut it arrives.
 * Touched by the procedure on the window's own thread, and once before it is installed. */
static unsigned char g_down[256], g_kept[256];
static unsigned g_buttons, g_kept_buttons;

static WNDPROC previous_of(HWND window)
{
	WNDPROC previous = NULL;
	AcquireSRWLockShared(&g_lock);
	for (int i = 0; i < GUARDED; ++i)
		if (g_guarded[i].window == window) previous = g_guarded[i].previous;
	ReleaseSRWLockShared(&g_lock);
	return previous;
}

static void forget(HWND window)
{
	AcquireSRWLockExclusive(&g_lock);
	for (int i = 0; i < GUARDED; ++i)
		if (g_guarded[i].window == window) {
			g_guarded[i].window = NULL;
			g_guarded[i].previous = NULL;
		}
	ReleaseSRWLockExclusive(&g_lock);
	if (g_held == window) g_held = NULL;
}

/* The button a mouse message is about, as its bit in `g_buttons`; 0 for one that is not a button. */
static unsigned button_of(UINT message, WPARAM w)
{
	switch (message) {
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP: return 1u;
	case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP: return 2u;
	case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP: return 4u;
	case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP:
		return GET_XBUTTON_WPARAM(w) == XBUTTON2 ? 16u : 8u;
	default: return 0u;
	}
}

static LRESULT CALLBACK guard_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
	WNDPROC previous = previous_of(window);
	const int held = g_held == window;
	/* What a swallowed message returns: 0, and TRUE for the X buttons, whose documentation asks a
	 * procedure that handled one to say so. */
	const LRESULT swallowed = (message == WM_XBUTTONDOWN || message == WM_XBUTTONDBLCLK
	                           || message == WM_XBUTTONUP) ? TRUE : 0;
	switch (message) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		/* Alt+F4 still closes the game: the game's procedure is what turns it into a close. */
		if (held && !(message == WM_SYSKEYDOWN && w == VK_F4)) {
			if (!g_down[w & 0xFF]) g_kept[w & 0xFF] = 1;
			return 0;
		}
		g_down[w & 0xFF] = 1;
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (g_down[w & 0xFF]) {
			g_down[w & 0xFF] = g_kept[w & 0xFF] = 0;
			break;
		}
		if (g_kept[w & 0xFF] || held) {
			g_kept[w & 0xFF] = 0;
			return 0;
		}
		break;
	case WM_CHAR:
	case WM_SYSCHAR:
	case WM_DEADCHAR:
	case WM_SYSDEADCHAR:
		if (held) return 0;
		break;
	case WM_UNICHAR:
		/* UNICODE_NOCHAR asks whether the procedure takes WM_UNICHAR at all; that is not input. */
		if (held && w != UNICODE_NOCHAR) return 0;
		break;
	case WM_MOUSEMOVE:
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
		if (held) return 0;
		break;
	case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
	case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
		if (held) {
			if (!(g_buttons & button_of(message, w))) g_kept_buttons |= button_of(message, w);
			return swallowed;
		}
		g_buttons |= button_of(message, w);
		break;
	case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: {
		const unsigned button = button_of(message, w);
		if (g_buttons & button) {
			g_buttons &= ~button;
			g_kept_buttons &= ~button;
			break;
		}
		if ((g_kept_buttons & button) || held) {
			g_kept_buttons &= ~button;
			return swallowed;
		}
		break;
	}
	case WM_INPUT:
		/* Raw input the game's window was registered for. DefWindowProc is what frees it, and a
		 * procedure that keeps a WM_INPUT from the window it was meant for still owes that call. */
		if (held) {
			DefWindowProcW(window, message, w, l);
			return 0;
		}
		break;
	case WM_NCDESTROY: {
		LRESULT result = previous ? CallWindowProcW(previous, window, message, w, l)
		                          : DefWindowProcW(window, message, w, l);
		forget(window);
		return result;
	}
	default:
		break;
	}
	return previous ? CallWindowProcW(previous, window, message, w, l)
	                : DefWindowProcW(window, message, w, l);
}

/* Once a window procedure of ours is on a window, this module must outlive it: the Vulkan loader
 * unloads a layer with its instance, and the window would then call into nothing. Pinning keeps the
 * module loaded for the rest of the process. */
static void pin_module(void)
{
	static volatile LONG pinned;
	if (InterlockedExchange(&pinned, 1)) return;
	HMODULE self = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
	                   (LPCWSTR)(void *)&guard_proc, &self);
}

static int guarded(HWND window)
{
	return previous_of(window) != NULL;
}

static int subclass(HWND window)
{
	WNDPROC before = (WNDPROC)GetWindowLongPtrW(window, GWLP_WNDPROC);
	if (!before || before == guard_proc) return before == guard_proc;
	pin_module();
	/* The entry goes in before the procedure does: the window may be handed a message on its own
	 * thread the moment SetWindowLongPtrW returns, and the procedure has to find where to pass it. */
	int slot = -1;
	AcquireSRWLockExclusive(&g_lock);
	for (int i = 0; i < GUARDED && slot < 0; ++i)
		if (!g_guarded[i].window) slot = i;
	if (slot >= 0) {
		g_guarded[slot].window = window;
		g_guarded[slot].previous = before;
	}
	ReleaseSRWLockExclusive(&g_lock);
	if (slot < 0) return 0;
	/* Whatever is held down now went down before there was a procedure here to see it, and the game
	 * saw it: its release is the game's, or a key held while the panel opened would stay down in the
	 * game. Read through the one key reader (nr_focus.h). */
	for (int vk = 1; vk < 256; ++vk) {
		const int down = nr_key_down(vk);
		if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1
		    || vk == VK_XBUTTON2) {
			const unsigned bit = vk == VK_LBUTTON ? 1u : vk == VK_RBUTTON ? 2u : vk == VK_MBUTTON ? 4u
			                     : vk == VK_XBUTTON1 ? 8u : 16u;
			if (down) g_buttons |= bit;
		} else if (down) {
			g_down[vk] = 1;
		}
	}
	SetLastError(0);
	WNDPROC was = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)guard_proc);
	if (!was && GetLastError()) {
		forget(window);
		return 0;
	}
	if (was != before) {   /* something changed it between the two calls: pass to that instead */
		AcquireSRWLockExclusive(&g_lock);
		g_guarded[slot].previous = was;
		ReleaseSRWLockExclusive(&g_lock);
	}
	return 1;
}

void nr_input_guard(void *window, int hold)
{
	HWND w = (HWND)window;
	if (!hold || !w || !IsWindow(w)) {
		g_held = NULL;
		return;
	}
	if (!guarded(w) && !subclass(w)) {
		g_held = NULL;
		return;
	}
	g_held = w;
}

void nr_input_guard_release(void)
{
	g_held = NULL;
	AcquireSRWLockExclusive(&g_lock);
	for (int i = 0; i < GUARDED; ++i) {
		HWND w = g_guarded[i].window;
		if (!w) continue;
		/* Only where ours is still the window's procedure; where something subclassed it after us,
		 * taking ours out would cut that one's chain, so it stays and passes everything through. */
		if (IsWindow(w) && (WNDPROC)GetWindowLongPtrW(w, GWLP_WNDPROC) == guard_proc) {
			SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)g_guarded[i].previous);
			g_guarded[i].window = NULL;
			g_guarded[i].previous = NULL;
		} else if (!IsWindow(w)) {
			g_guarded[i].window = NULL;
			g_guarded[i].previous = NULL;
		}
	}
	ReleaseSRWLockExclusive(&g_lock);
}

#endif /* _WIN32 */

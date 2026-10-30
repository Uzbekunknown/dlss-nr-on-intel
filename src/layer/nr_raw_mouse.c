/* Raw mouse movement, for a panel that pins the system cursor.
 *
 * See nr_raw_mouse.h. The short version: once `ClipCursor` holds the cursor inside a one-pixel
 * box, `GetCursorPos` cannot report movement - it returns the box. The movement still arrives as
 * raw input, which is independent of where the cursor is being held, and that is what this reads.
 */
#include "nr_raw_mouse.h"

#ifdef _WIN32

#include <stdio.h>
#include <string.h>
#include <windows.h>

static HWND g_window;
static int g_registered;
static int g_pending_x;
static int g_pending_y;

static LRESULT CALLBACK nr_raw_mouse_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    (void)h;
    /* Nothing is handled here on purpose: `GetRawInputBuffer` drains the queue without a WndProc.
     * The window exists only to give the registration somewhere to point, and a class with no
     * procedure cannot be registered. */
    return DefWindowProcW(h, msg, w, l);
}

static int ensure_window(void)
{
    if (g_window) return 1;

    static int registered_class;
    if (!registered_class) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = nr_raw_mouse_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"DLSSNRRawMouse";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
        registered_class = 1;
    }

    /* A real top-level window, never shown and never activated. A message-only window would be
     * tidier but does not receive raw input on every Windows version. */
    g_window = CreateWindowExW(0, L"DLSSNRRawMouse", L"", WS_POPUP, 0, 0, 0, 0,
                               NULL, NULL, GetModuleHandleW(NULL), NULL);
    return g_window != NULL;
}

/* The game's own registration for the mouse (usage page 1, usage 2), if it has one.
 *
 * A process holds ONE registration per usage: registering here REPLACES the game's, and a plain
 * `RIDEV_REMOVE` on the way out would leave the game with none - a game that reads the mouse
 * through `WM_INPUT` (most do, UE4 among them) has no mouse after the panel has been opened once.
 * So the game's registration is read before ours goes in, and put back, target window and flags,
 * when ours comes out. PR #20 review, item 1. */
static RAWINPUTDEVICE g_saved_device;
static int            g_have_saved;

/* The game's own mouse registration, read before ours goes in so it can be put back.
 *
 * **`cbSize` is `sizeof(RAWINPUTDEVICE)`, not `sizeof(RAWINPUTHEADER)`.** The parameter is
 * documented as "the size, in bytes, of a RAWINPUTDEVICE structure", and the two structs are
 * different sizes on x64 - 16 against 24. Passing the header's size made the call fail with
 * `ERROR_INVALID_PARAMETER` on every 64-bit process, so nothing was ever saved and
 * `nr_raw_mouse_stop` took the `RIDEV_REMOVE` branch every time, stripping the game's mouse.
 * The 32-bit build restored it by luck: WOW64 accepted the smaller size.
 *
 * **The return value is compared against `(UINT)-1`, not tested for truth.** The API returns
 * `(UINT)-1` on failure, which is truthy, so `if (GetRegisteredRawInputDevices(...))` read a
 * failure as a success - and then walked a buffer it had never filled. `nr_raw_mouse_take`
 * further down already does this correctly.
 *
 * **`ERROR_INSUFFICIENT_BUFFER` is retried.** The count the first call reports is a snapshot; a
 * game that registers a device between the two calls would have its entry dropped. The retry
 * costs one allocation and makes the read honest. */
static void save_game_registration(void)
{
    g_have_saved = 0;
    UINT count = 0;
    if (GetRegisteredRawInputDevices(NULL, &count, sizeof(RAWINPUTDEVICE)) == (UINT)-1 || !count)
        return;

    for (int attempt = 0; attempt < 2; ++attempt) {
        RAWINPUTDEVICE *devices = (RAWINPUTDEVICE *)HeapAlloc(GetProcessHeap(), 0,
                                                             count * sizeof(RAWINPUTDEVICE));
        if (!devices) return;
        UINT got = count;
        if (GetRegisteredRawInputDevices(devices, &got, sizeof(RAWINPUTDEVICE)) != (UINT)-1) {
            for (UINT i = 0; i < got && i < count; ++i) {
                if (devices[i].usUsagePage == 0x01 && devices[i].usUsage == 0x02) {
                    g_saved_device = devices[i];
                    g_have_saved = 1;
                    break;
                }
            }
            HeapFree(GetProcessHeap(), 0, devices);
            return;
        }
        HeapFree(GetProcessHeap(), 0, devices);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return;   /* a real failure: give up */
        count *= 2;                                               /* more devices: ask for more room */
    }
}

int nr_raw_mouse_start(void)
{
    if (g_registered) return 1;
    if (!ensure_window()) return 0;
    save_game_registration();

    RAWINPUTDEVICE device;
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = RIDEV_INPUTSINK;   /* also while the game, not us, is foreground */
    device.hwndTarget = g_window;
    if (!RegisterRawInputDevices(&device, 1, sizeof device)) return 0;

    g_registered = 1;
    g_pending_x = 0;
    g_pending_y = 0;

    /* Movement queued before the panel opened belongs to the game's last frames, not to the
     * pointer. Draining it is what keeps the pointer from lurching on the first frame. */
    {
        BYTE scratch[4 * 1024];
        UINT size;
        do {
            size = (UINT)sizeof scratch;
            if (GetRawInputBuffer((PRAWINPUT)scratch, &size, sizeof(RAWINPUTHEADER)) == 0) break;
        } while (size > 0);
    }
    return 1;
}

void nr_raw_mouse_stop(void)
{
    if (!g_registered) return;
    RAWINPUTDEVICE device;
    if (g_have_saved) {
        /* The game had its own registration: put it back exactly as it was - same target window,
         * same flags - rather than removing it. A REMOVE here is what would strip a WM_INPUT game
         * of its mouse. A restore can fail if the game's target window died while the panel was
         * open; nothing recoverable is left either way, and the state is cleared regardless. */
        device = g_saved_device;
    } else {
        device.usUsagePage = 0x01;
        device.usUsage = 0x02;
        device.dwFlags = RIDEV_REMOVE;
        device.hwndTarget = NULL;
    }
    /* The restore is the whole point of this file, so a failure to restore is worth a line. It
     * used to be silent: the comment already conceded a restore can fail when the game's target
     * window died, and nobody would have known. A line on stderr costs nothing and this runs
     * once per panel open. */
    if (!RegisterRawInputDevices(&device, 1, sizeof device))
        fprintf(stderr, "[dlss-nr] raw mouse: could not put the game's registration back "
                        "(err %lu); a WM_INPUT game may have no mouse until restarted\n",
                GetLastError());
    g_have_saved = 0;
    g_registered = 0;
    g_pending_x = 0;
    g_pending_y = 0;
}

int nr_raw_mouse_take(int *dx, int *dy)
{
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    if (!g_registered) return 0;

    /* Drains the queue, so this is one frame's worth of movement however many packets carried it.
     * Records are padded to `sizeof(RAWINPUT)` and the count comes back as the return value. */
    static BYTE buffer[16 * 1024];
    UINT size = (UINT)sizeof buffer;
    UINT count = GetRawInputBuffer((PRAWINPUT)buffer, &size, sizeof(RAWINPUTHEADER));
    if (count == 0 || count == (UINT)-1) return 0;

    const BYTE *at = buffer;
    int moved = 0;
    for (UINT i = 0; i < count; ++i) {
        const RAWINPUT *input = (const RAWINPUT *)at;
        if (input->header.dwType == RIM_TYPEMOUSE) {
            const RAWMOUSE *mouse = &input->data.mouse;
            /* `MOUSE_MOVE_ABSOLUTE` is a tablet or a remote session: those coordinates are not
             * deltas, and adding them would fling the pointer across the picture. */
            if ((mouse->usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                if (mouse->lLastX || mouse->lLastY) {
                    g_pending_x += mouse->lLastX;
                    g_pending_y += mouse->lLastY;
                    moved = 1;
                }
            }
        }
        at += input->header.dwSize ? input->header.dwSize : sizeof(RAWINPUT);
    }

    if (dx) *dx = g_pending_x;
    if (dy) *dy = g_pending_y;
    g_pending_x = 0;
    g_pending_y = 0;
    return moved;
}

#else /* not Windows */

int nr_raw_mouse_start(void) { return 0; }
void nr_raw_mouse_stop(void) { }
int nr_raw_mouse_take(int *dx, int *dy) { if (dx) *dx = 0; if (dy) *dy = 0; return 0; }

#endif /* _WIN32 */

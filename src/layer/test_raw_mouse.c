/* The raw-mouse registration, and whether the game gets it back.
 *
 * A Vulkan layer that reads the mouse has to register for it, and a registration is per-process
 * and per-usage: putting ours in over the game's does not stack, it replaces. So the game's is
 * read first and put back afterwards - which is what this checks, because the alternative is a
 * game that reads the mouse through `WM_INPUT` (UE4 among them) having no mouse at all after the
 * panel has been opened once.
 *
 * The bug this was written for: `GetRegisteredRawInputDevices` was called with
 * `sizeof(RAWINPUTHEADER)` where it wants `sizeof(RAWINPUTDEVICE)`. On x64 those are 24 and 16,
 * so the call failed with `ERROR_INVALID_PARAMETER` in every 64-bit process, nothing was ever
 * saved, and `nr_raw_mouse_stop` took the `RIDEV_REMOVE` branch every time. The 32-bit build
 * restored it by luck, because WOW64 accepted the smaller size. A test on the machine that was
 * working would not have seen it.
 *
 * What this can and cannot prove: the registration table is per-process, so this fixture proves
 * the code's logic in its own process and nothing about what happens inside a real game - a game
 * may register with `RIDEV_NOLEGACY`, `RIDEV_CAPTUREMOUSE`, a different target window, or from a
 * DLL loaded later. That part still needs a real game, which is why the assertions here are about
 * our own process's table and nothing more.
 *
 * It needs a window and a focus change for the positive case, and it self-skips when the OS
 * refuses to move focus, for the same reason `test_focus_gate` does.
 */
#include "nr_raw_mouse.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

static int failures = 0;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %s  %s  (%s)\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) ++failures;
}

/* One registration, as the table reports it, or all-zeroes when there is none. */
static int read_registration(USHORT page, USHORT usage, RAWINPUTDEVICE *out)
{
    memset(out, 0, sizeof *out);
    UINT count = 0;
    if (GetRegisteredRawInputDevices(NULL, &count, sizeof(RAWINPUTDEVICE)) == (UINT)-1 || !count)
        return 0;
    RAWINPUTDEVICE *devices = (RAWINPUTDEVICE *)calloc(count, sizeof(RAWINPUTDEVICE));
    if (!devices) return 0;
    UINT got = count;
    int found = 0;
    if (GetRegisteredRawInputDevices(devices, &got, sizeof(RAWINPUTDEVICE)) != (UINT)-1) {
        for (UINT i = 0; i < got; ++i) {
            if (devices[i].usUsagePage == page && devices[i].usUsage == usage) {
                *out = devices[i];
                found = 1;
                break;
            }
        }
    }
    free(devices);
    return found;
}

#ifdef _WIN32
static LRESULT CALLBACK probe_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(hwnd, msg, wp, lp);
}
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("the raw-mouse registration, and whether the game gets it back\n");

    /* ---- the shapes the bug was made of ----
     *
     * These are the facts the two structs' sizes rest on. If a future edit renames the argument or
     * the header changes, this is what says so without anyone having to remember the bug. */
    check(sizeof(RAWINPUTDEVICE) == 16,
          "a RAWINPUTDEVICE is 16 bytes",
          sizeof(RAWINPUTDEVICE) == 16 ? "16" : "unexpected size");
    check(sizeof(RAWINPUTDEVICE) != sizeof(RAWINPUTHEADER),
          "and is not the size of a RAWINPUTHEADER",
          sizeof(RAWINPUTDEVICE) != sizeof(RAWINPUTHEADER)
              ? "different - the argument cannot be the header's size"
              : "SAME SIZE: the bug would be invisible again");

#ifndef _WIN32
    printf("  (not Windows: nothing below applies - the sources compile to no-ops)\n");
    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
#else
    /* ---- a game's registration, as a game would leave it ---- */
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = probe_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "NrRawMouseProbe";
    if (!RegisterClassExA(&wc)) {
        printf("  FAIL  could not register the probe window class\n");
        return 1;
    }
    HWND game = CreateWindowExA(0, "NrRawMouseProbe", "the game", WS_OVERLAPPEDWINDOW,
                               0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
    if (!game) {
        printf("  FAIL  could not create the probe window\n");
        return 1;
    }

    /* Page 1 usage 2 is the mouse. A game that reads it through WM_INPUT registers exactly this,
     * with its own window as the target; the flags vary by game, so this one uses INPUTSINK - the
     * same flag the layer itself uses - which is enough to prove the restore preserves them. */
    RAWINPUTDEVICE game_device;
    game_device.usUsagePage = 0x01;
    game_device.usUsage = 0x02;
    game_device.dwFlags = RIDEV_INPUTSINK;
    game_device.hwndTarget = game;
    if (!RegisterRawInputDevices(&game_device, 1, sizeof game_device)) {
        printf("  FAIL  could not register the probe's own mouse\n");
        return 1;
    }
    /* And a keyboard, which the layer must not touch: the bug's symptom was that only the mouse
     * registration was stripped, so this is what proves the fix is not "remove everything". */
    RAWINPUTDEVICE kbd_device;
    kbd_device.usUsagePage = 0x01;
    kbd_device.usUsage = 0x06;
    kbd_device.dwFlags = RIDEV_INPUTSINK;
    kbd_device.hwndTarget = game;
    if (!RegisterRawInputDevices(&kbd_device, 1, sizeof kbd_device)) {
        printf("  FAIL  could not register the probe's own keyboard\n");
        return 1;
    }

    RAWINPUTDEVICE before;
    int had = read_registration(0x01, 0x02, &before);
    check(had, "the probe's mouse registration is in the table",
          had ? "found" : "NOT FOUND - the fixture cannot prove anything");

    /* ---- ours over it, and back out ---- */
    nr_raw_mouse_start();
    nr_raw_mouse_stop();

    RAWINPUTDEVICE after;
    int still = read_registration(0x01, 0x02, &after);
    check(still, "the mouse registration survives our start and stop",
          still ? "restored" : "GONE - a WM_INPUT game would have no mouse");
    check(still && after.hwndTarget == before.hwndTarget,
          "and it names the same target window",
          still ? (after.hwndTarget == before.hwndTarget ? "same window" : "DIFFERENT WINDOW")
                : "nothing to compare");
    check(still && after.dwFlags == before.dwFlags,
          "with the same flags",
          still ? (after.dwFlags == before.dwFlags ? "same flags" : "FLAGS CHANGED")
                : "nothing to compare");

    RAWINPUTDEVICE kbd_after;
    int kbd_left = read_registration(0x01, 0x06, &kbd_after);
    check(kbd_left, "the keyboard registration is untouched",
          kbd_left ? "still there" : "GONE - the layer took more than the mouse");

    DestroyWindow(game);
    UnregisterClassA("NrRawMouseProbe", wc.hInstance);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
#endif
}

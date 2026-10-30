/* Is the panel fed input while another window is in front?
 *
 * The report: "as long as the game window is visible, doing anything outside it drags a slider,
 * selects, moves the panel". The cause was that the panel read the SYSTEM-WIDE cursor and button
 * state with no check that the game was the window the user was working in - so a drag in a browser
 * moved a slider and a click in Explorer moved the panel itself.
 *
 * OptiScaler's `FeedImGui` refuses to feed anything while the game is unfocused
 * (OptiScaler menu/input/input_system.cpp, the `if (!_state.Focused)` early return, which reports
 * "no position" and clears the modifiers). It learns focus from `WM_SETFOCUS`/`WM_KILLFOCUS`
 * because it subclasses the game's `WndProc` (input_system_messages.cpp). A Vulkan layer has no
 * window procedure to subclass, so the same question is asked with `GetForegroundWindow`.
 *
 * **This calls the shipped function**, `nr_focus.c`, rather than restating the rule. An earlier
 * draft of this file reimplemented the predicate "with the same calls", which would have passed
 * whatever the test believed and said nothing about the layer - the failure mode this project has
 * already paid for once, when a test encoded the wrong contract and hid a real bug for a day.
 *
 * Build: tools\build_win.bat and the Makefile both compile nr_focus.c with this.
 */
#include "nr_focus.h"

#include <windows.h>
#include <stdio.h>

static int fails = 0;
#define CHECK(name, cond)                                              \
    do {                                                               \
        printf("  %s  %s\n", (cond) ? "PASS" : "FAIL", name);          \
        if (!(cond)) ++fails;                                          \
    } while (0)

int main(void)
{
    printf("test_focus_gate: is input withheld while the game is not in front?\n\n");

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "nrFocusGateTest";
    RegisterClassA(&wc);

    HWND ours = CreateWindowExA(0, "nrFocusGateTest", "ours", WS_OVERLAPPEDWINDOW,
                                100, 100, 400, 300, NULL, NULL, wc.hInstance, NULL);
    if (!ours) { printf("  could not create a window; skipping\n"); return 0; }
    ShowWindow(ours, SW_SHOW);
    UpdateWindow(ours);

    /* A null handle must NOT read as foreground, or the panel takes input before the game's window
     * exists - the direction that fails open rather than closed. */
    CHECK("a null window is not foreground", !nr_window_is_foreground(NULL));
    CHECK("a null window is not ours", !nr_window_is_ours(NULL));

    CHECK("our own window is ours", nr_window_is_ours(ours));

    SetForegroundWindow(ours);
    Sleep(150);
    if (GetForegroundWindow() == ours) {
        CHECK("our own focused window counts as foreground",
              nr_window_is_foreground(ours));
    } else {
        printf("  (this process could not take the foreground; the positive case is not exercised)\n");
    }

    /* A window this process does not own. The shell window is a stable one to borrow: it always
     * exists, and it is never ours. */
    HWND shell = GetShellWindow();
    if (shell && shell != ours) {
        CHECK("another process's window is not ours", !nr_window_is_ours(shell));
        SetForegroundWindow(shell);
        Sleep(200);
        if (GetForegroundWindow() == ours) {
            printf("  (the OS refused to move focus away; the refusal case is not exercised)\n");
        } else {
            CHECK("another process's window does NOT count as foreground",
                  !nr_window_is_foreground(ours));
        }
        SetForegroundWindow(ours);
        Sleep(150);
    }

    DestroyWindow(ours);
    printf("\n%s\n", fails ? "FAILED" : "ok");
    return fails;
}

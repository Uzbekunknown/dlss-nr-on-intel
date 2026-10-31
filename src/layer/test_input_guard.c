/* The panel's keys and clicks kept from the game's window (nr_input_guard.c).
 *
 * A window of its own stands in for the game's, with a procedure that counts what reaches it, and
 * the messages are sent to it directly - SendMessage calls the window's procedure on this thread, so
 * nothing is synthesised for the desktop, no focus moves, and the fixture runs anywhere. What it
 * proves is the procedure's rule:
 *
 *   - while the panel is open the game gets no key press, character, pointer move, button press,
 *     wheel or WM_INPUT;
 *   - a key or button the game saw go down gets its release, even while the panel is open, so
 *     nothing stays held down in the game; one it never saw go down gets none, even after the panel
 *     has shut, so a release-bound action does not fire;
 *   - Alt+F4 and every message that is not input still arrive;
 *   - with the panel shut everything arrives, and taking the guard off gives the window back its
 *     own procedure.
 *
 * What it cannot prove is a real game's input path - a game that polls GetAsyncKeyState or reads
 * DirectInput is outside this by design (nr_input_guard.h) - which is why the game test exists.
 */
#include "nr_input_guard.h"

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

#ifdef _WIN32
/* What reached the game's own procedure, by message. */
static int g_seen[0x500];

static LRESULT CALLBACK game_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message < 0x500) ++g_seen[message];
    if (message == WM_USER + 1) return 42;
    return DefWindowProcW(window, message, w, l);
}

/* Send one message and say whether the game's procedure saw it. */
static int reaches(HWND window, UINT message, WPARAM w, LPARAM l)
{
    int before = message < 0x500 ? g_seen[message] : 0;
    SendMessageW(window, message, w, l);
    return message < 0x500 && g_seen[message] > before;
}
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("the panel's input, kept from the game's window\n");
#ifndef _WIN32
    printf("  (not Windows: the guard is a no-op there)\n");
    return 0;
#else
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = game_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"NrInputGuardGame";
    if (!RegisterClassExW(&wc)) {
        printf("  FAIL  could not register the stand-in window class\n");
        return 1;
    }
    HWND game = CreateWindowExW(0, wc.lpszClassName, L"the game", WS_OVERLAPPEDWINDOW,
                                0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
    if (!game) {
        printf("  FAIL  could not create the stand-in window\n");
        return 1;
    }

    /* ---- shut: the guard is not even on the window yet ---- */
    nr_input_guard(game, 0);
    check((WNDPROC)GetWindowLongPtrW(game, GWLP_WNDPROC) == game_proc,
          "a shut panel does not subclass the game's window", "its own procedure");
    /* Opened and shut once, so the guard is on the window and sees what the game is handed next:
     * a press from before the subclass existed is known only by the key's state, which a message
     * sent here does not change. */
    nr_input_guard(game, 1);
    nr_input_guard(game, 0);
    check(reaches(game, WM_KEYDOWN, 'D', 0), "the game gets a key while the panel is shut", "D down");
    check(reaches(game, WM_LBUTTONDOWN, MK_LBUTTON, 0), "and a click", "left down");

    /* ---- open ---- */
    nr_input_guard(game, 1);
    check((WNDPROC)GetWindowLongPtrW(game, GWLP_WNDPROC) != game_proc,
          "an open panel subclasses it", "a procedure in front of the game's");
    check(!reaches(game, WM_KEYDOWN, 'A', 0), "a key press does not reach the game", "A down");
    check(!reaches(game, WM_CHAR, 'a', 0), "nor its character", "WM_CHAR");
    check(!reaches(game, WM_KEYUP, 'A', 0), "nor the release of a key the game never saw go down",
          "A up");
    check(reaches(game, WM_KEYUP, 'D', 0),
          "but a key the game saw go down before the panel opened is released in the game", "D up");
    check(!reaches(game, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10)), "no pointer move", "WM_MOUSEMOVE");
    check(!reaches(game, WM_RBUTTONDOWN, MK_RBUTTON, 0), "no button press", "right down");
    check(!reaches(game, WM_RBUTTONUP, 0, 0), "and not its release", "right up");
    check(reaches(game, WM_LBUTTONUP, 0, 0),
          "but the release of a button pressed in the game arrives", "left up");
    check(!reaches(game, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0), "no wheel", "WM_MOUSEWHEEL");
    check(!reaches(game, WM_INPUT, RIM_INPUTSINK, 0), "no raw input", "WM_INPUT");
    check(!reaches(game, WM_SYSKEYDOWN, VK_MENU, 0x20000000), "no Alt", "WM_SYSKEYDOWN Alt");
    check(reaches(game, WM_SYSKEYDOWN, VK_F4, 0x20000000), "but Alt+F4 still closes the game",
          "WM_SYSKEYDOWN F4");
    check(reaches(game, WM_SETCURSOR, (WPARAM)game, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)),
          "and what is not input arrives", "WM_SETCURSOR");
    {
        int before = g_seen[WM_USER + 1];
        LRESULT answer = SendMessageW(game, WM_USER + 1, 0, 0);
        check(g_seen[WM_USER + 1] == before + 1 && answer == 42,
              "with the game's own answer", answer == 42 ? "42" : "the answer was lost");
    }
    check(!reaches(game, WM_KEYDOWN, 'B', 0), "a key pressed in the panel", "B down");

    /* ---- shut again ---- */
    nr_input_guard(game, 0);
    check(!reaches(game, WM_KEYUP, 'B', 0),
          "is not released in the game after the panel shuts: the game never saw it go down",
          "B up");
    check(reaches(game, WM_KEYDOWN, 'C', 0), "and the game has its keys back", "C down");
    check(reaches(game, WM_KEYUP, 'C', 0), "and their releases", "C up");
    check(reaches(game, WM_MOUSEMOVE, 0, MAKELPARAM(20, 20)), "and its pointer", "WM_MOUSEMOVE");

    /* ---- off ---- */
    nr_input_guard_release();
    check((WNDPROC)GetWindowLongPtrW(game, GWLP_WNDPROC) == game_proc,
          "taking the guard off gives the window its own procedure back", "game_proc");
    nr_input_guard(game, 1);
    check(!reaches(game, WM_KEYDOWN, 'E', 0), "and it can be held again", "E down");
    nr_input_guard(game, 0);
    nr_input_guard_release();

    /* ---- a window that goes away ---- */
    nr_input_guard(game, 1);
    DestroyWindow(game);
    nr_input_guard(game, 1);   /* a dead window holds nothing and does not crash */
    check(1, "a destroyed window is let go", "no fault");
    nr_input_guard_release();
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
#endif
}

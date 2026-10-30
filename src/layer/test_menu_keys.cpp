/* The panel's hotkeys, checked where they cannot be faked.
 *
 * The report was "S does not save, R does not reset, END does nothing" while the panel drew and
 * its buttons worked. The cause was structural: all three lived inside `nr_menu_update`'s
 * `if (m->open && m->read_keys)` block, and the ImGui build sets `read_keys` to 0 - so the block
 * never ran. Two earlier attempts at a test missed it, and both are recorded here because each
 * looked like it worked:
 *
 *   1. Driving `nr_menu_bridge_save` / `_reset` passes whatever the key code does: those entry
 *      points do not go through the key reading at all.
 *   2. Driving a real `nr_menu` needs a Vulkan device - `nr_menu_create` asks the driver for
 *      memory properties - so a headless test cannot reach `nr_menu_update` with `read_keys` off.
 *
 * What CAN be checked without a device is the structure that was wrong: that each hotkey's read
 * is outside the `read_keys` guard, and that the effect toggle is Home rather than END. That is a
 * source check, so it fails the moment somebody moves one back inside the block - which is the
 * regression this file exists for.
 */
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %s  %s  (%s)\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) ++failures;
}

/* The text between the start of `nr_menu_update` and the end of its `read_keys` block, plus the
 * text before it. Split there so a hotkey can be asserted to be in the right half. */
static int find(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

int main(void)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    FILE *f = fopen("src/layer/nr_menu.c", "rb");
    if (!f) {
        printf("  FAIL  nr_menu.c was not readable  (run from the repository root)\n");
        return 1;
    }
    static char source[200000];
    size_t n = fread(source, 1, sizeof source - 1, f);
    source[n] = '\0';
    fclose(f);

    printf("the panel's hotkeys, by structure\n");

    /* The block that was dead. Everything between this marker and its closing brace is skipped
     * when `read_keys` is 0, so nothing a player must be able to press may live there. */
    const char *guard = strstr(source, "if (m->open && m->read_keys) {");
    if (!guard) {
        printf("  FAIL  the read_keys guard was not found  (has nr_menu_update been restructured?)\n");
        return 1;
    }

    /* The S and R reads, which an earlier fix moved ABOVE the guard so the ImGui build could see
     * them at all - it sets `read_keys` to 0, and inside the guard they were dead.
     *
     * That fix was right about the symptom and wrong about the boundary, and this assertion held
     * the wrong boundary in place: being outside the guard is also what left S and R with NO focus
     * check, so an S typed in a chat window wrote the settings file. They are back inside a guard
     * now - `m->open && m->read_keys` - and the ImGui build reads them at the layer's level
     * instead (`nr_layer.c`, gated on the game being in front), which is where they belonged.
     *
     * What must stay true either way: S and R are READ somewhere, and the guard they sit behind
     * asks about focus, not about ImGui. */
    const char *save = strstr(source, "if (key_pressed(m, NR_KEY_S)) m->save_pressed = 1;");
    const char *reset = strstr(source, "if (key_pressed(m, NR_KEY_R)) m->reset_pressed = 1;");

    check(save != NULL, "S is still read somewhere", save ? "found" : "NOT FOUND");
    check(save != NULL && save > guard,
          "and it sits behind the read_keys guard",
          save && save > guard
              ? "inside the guard - and the ImGui build reads it at the layer's level instead"
              : "outside the guard again - no focus check, an unfocused S saves");
    check(reset != NULL, "R is still read somewhere", reset ? "found" : "NOT FOUND");
    check(reset != NULL && reset > guard,
          "and so does R",
          reset && reset > guard
              ? "inside the guard"
              : "outside the guard again - no focus check, an unfocused R resets");

    /* The effect toggle is Home, 0x24, and no longer END, 0x23. END is bound by enough games
     * that the key never arrived; the report was "END does nothing". */
    check(find(source, "#define NR_KEY_TOGGLE  0x24"),
          "the effect toggle is Home (0x24)",
          find(source, "#define NR_KEY_TOGGLE  0x24") ? "0x24" : "not 0x24 - see the define");
    check(!find(source, "#define NR_KEY_COMPARE"),
          "the comparison key is gone",
          find(source, "#define NR_KEY_COMPARE") ? "NR_KEY_COMPARE still defined" : "removed");

    /* The comparison feature itself: no compare-mode transitions left in the key handling. This
     * is what "delete the Home function" means - the key moved to the toggle, so any remaining
     * compare_mode bump on a key would be a second meaning on one key. */
    check(!find(source, "if (key_pressed(m, NR_KEY_C))"),
          "no key still cycles the comparison",
          find(source, "if (key_pressed(m, NR_KEY_C))") ? "NR_KEY_C still handled" : "removed");

    /* The layer reads Home, S and R itself for the ImGui build, next to INS and L. */
    /* Both files, concatenated. The menu's glue moved to `nr_menu_layer.c`, and these checks are
     * about a SHAPE being present in the layer's code - which is now two files. Naming one would
     * fail the next time a function moved, which is a worse test than one that says "somewhere in
     * the layer". */
    static char lsource[1600000];
    size_t ln = 0;
    {
        /* **Every file a key reader could hide in.** `nr_menu.c` is the one that used to hold
         * one - the state machine read `GetAsyncKeyState` itself until this round - and it was not
         * in this list, so the mutation that put a reader back there was not seen. A text scan
         * that cannot see the file the defect lived in is not a check. */
        static const char *const files[] = {
            "src/layer/nr_layer.c", "src/layer/nr_menu_layer.c", "src/layer/nr_layer_state.h",
            "src/layer/nr_menu.c", "src/layer/nr_menu_font.c", "src/layer/nr_menu_bridge.c",
        };
        for (int fi = 0; fi < (int)(sizeof files / sizeof files[0]); ++fi) {
            FILE *layer = fopen(files[fi], "rb");
            if (!layer) {
                printf("  FAIL  %s was not readable (run from the repository root)\n", files[fi]);
                return 1;
            }
            size_t got = fread(lsource + ln, 1, sizeof lsource - 1 - ln, layer);
            ln += got;
            fclose(layer);
            if (ln + 1 < sizeof lsource) lsource[ln++] = '\n';
        }
        lsource[ln] = '\0';
    }

    check(find(lsource, "nr_menu_inject_toggle(data->menu)"),
          "the layer raises the toggle for the ImGui build",
          find(lsource, "nr_menu_inject_toggle(data->menu)") ? "present" : "MISSING");
    check(find(lsource, "nr_menu_inject_save(data->menu)"),
          "the layer raises Save for the ImGui build",
          find(lsource, "nr_menu_inject_save(data->menu)") ? "present" : "MISSING");
    check(find(lsource, "nr_menu_inject_reset(data->menu)"),
          "the layer raises Reset for the ImGui build",
          find(lsource, "nr_menu_inject_reset(data->menu)") ? "present" : "MISSING");
    /* **`keys[0x24]`, and that is the point of the whole change.**
     *
     * Three files used to call `GetAsyncKeyState` on their own - this layer, `nr_menu.c` and
     * `nr_menu_ui.cpp` - so the focus gate had to be remembered in each, and two of them forgot it
     * at least once. The keyboard is now swept ONCE in `nr_menu_layer_present`, behind one gate,
     * and every consumer reads the swept array. This asserts the Home key is still read there, in
     * that shape: a `GetAsyncKeyState(0x24)` anywhere would mean a fourth reader had appeared. */
    check(find(lsource, "keys[0x24]"),
          "and reads Home itself (0x24), from the one swept array",
          find(lsource, "keys[0x24]") ? "keys[0x24]" : "MISSING - a second reader came back");
    /* And the reading really is in one place: the sweep is the only `GetAsyncKeyState` left. */
    {
        int sweeps = 0;
        for (const char *p = lsource; (p = strstr(p, "GetAsyncKeyState(")) != NULL; ++p) ++sweeps;
        char detail[128];
        snprintf(detail, sizeof detail, "%d GetAsyncKeyState call(s) in the layer's sources", sweeps);
        check(sweeps == 1, "the keyboard is read in exactly one place", detail);
    }

    /* ---- the bridge must not consume the toggle ----
     *
     * `nr_menu_bridge_update` runs BEFORE the layer asks the menu for the toggle, and
     * `nr_menu_take_toggle_request` clears the flag. So a bridge that takes it leaves the layer
     * with nothing: END did nothing, and then Home did nothing, for exactly this reason while
     * every panel button worked. The bridge has no way to act on the request - the trigger file
     * is the layer's - so it must leave it alone. */
    FILE *bridge_src = fopen("src/layer/nr_menu_bridge.c", "rb");
    if (!bridge_src) {
        printf("  FAIL  nr_menu_bridge.c was not readable\n");
        return 1;
    }
    static char bsource[400000];
    size_t bn = fread(bsource, 1, sizeof bsource - 1, bridge_src);
    bsource[bn] = '\0';
    fclose(bridge_src);

    check(!find(bsource, "if (nr_menu_take_toggle_request(b->menu))"),
          "the bridge does NOT consume the toggle request",
          find(bsource, "if (nr_menu_take_toggle_request(b->menu))")
              ? "still consumed - the layer then sees nothing and the key does nothing"
              : "left for the layer, which owns the trigger");

    /* ---- and the panel must not let nav eat Home ----
     *
     * With the panel open, ImGui treats Home as "focus the first item", so the key moved the
     * selection instead of reaching the toggle. Claiming it for the window is what stops nav. */
    FILE *ui_src = fopen("src/layer/nr_menu_ui.cpp", "rb");
    if (!ui_src) {
        printf("  FAIL  nr_menu_ui.cpp was not readable\n");
        return 1;
    }
    static char usource[400000];
    size_t un = fread(usource, 1, sizeof usource - 1, ui_src);
    usource[un] = '\0';
    fclose(ui_src);

    /* **The result is stored before it is used twice, and that is not style.**
     *
     * Written as `check(find(usource, X), "...", find(usource, X) ? "claimed" : "not claimed")`,
     * this reported PASS with the "not claimed" detail - both calls on the same buffer, disagreeing.
     * MSVC at /O2 merged the two identical calls and then took the wrong arm; the assertion passed
     * on the `ok` it computed and printed the other branch's text, so a real regression in the
     * panel (Home no longer claimed, and ImGui nav eating it) would have been announced as PASS.
     * Reading the answer ONCE removes the expression the compiler is free to rearrange. */
    int claims_home = find(usource, "ImGui::SetKeyOwner(ImGuiKey_Home");
    check(claims_home,
          "the panel claims Home so nav cannot act on it",
          claims_home ? "claimed"
                      : "not claimed - Home will move the selection instead of toggling the effect");

    /* ---- and the Win32 the layer needs is gated, so Linux still builds ----
     *
     * `nr_layer.c` compiled on Linux before this branch: `windows.h` sat inside `#ifdef _WIN32`
     * with a pthread arm beside it. This branch added the input path - window helpers, six
     * `GetAsyncKeyState` reads, the cursor release, the frame timer - and 27 lines of it were bare,
     * so `make` stopped at `nr_layer.c`. The author found four of them left after the first pass.
     *
     * A running test cannot see this: the defect is "this file does not compile on a platform this
     * machine has no compiler for". So the check reads the source the way the ABI ones do, and
     * asserts the SHAPES rather than the line numbers - the gate being `#ifdef _WIN32` around the
     * body, and `nr_key_down` being how a key is read. */
    {
        int cursor_gated = find(lsource, "#ifdef _WIN32\n\tShowCursor(TRUE);")
                           && find(lsource, "ClipCursor(NULL);\n#endif");
        check(cursor_gated, "the cursor release is gated",
              cursor_gated ? "ShowCursor and ClipCursor inside #ifdef _WIN32"
                           : "MISSING - a Linux build reaches ShowCursor");
        int timer_shared = find(lsource, "double nr_now_seconds(void)")
                           && find(lsource, "clock_gettime(CLOCK_MONOTONIC, &ts)");
        check(timer_shared, "the frame timer has a non-Windows arm",
              timer_shared ? "nr_now_seconds, with CLOCK_MONOTONIC beside QPC"
                           : "MISSING - LARGE_INTEGER and QueryPerformance* unguarded");
        /* And the key reads go through the gate rather than around it. */
        int keys_gated = find(lsource, "#define nr_key_down(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)")
                         && find(lsource, "#define nr_key_down(vk) (0)");
        check(keys_gated, "every key read goes through the platform gate",
              keys_gated ? "nr_key_down, with a no-op arm off Windows"
                         : "MISSING - bare GetAsyncKeyState somewhere");
    }

    /* ---- the C/C++ boundary is 32-bit safe ----
     *
     * Dead or Alive 5 is a 32-bit DirectX 9 game, and it crashed with the panel open about four
     * seconds in, right after DXVK creates the swapchain, at an address in no module. Two defects,
     * both invisible on x64 where every calling convention is the same and every handle is a
     * pointer:
     *
     *   1. The GPA typedef had no `VKAPI_PTR`. On x86 Vulkan's entry points are `__stdcall`, so
     *      every resolve unbalanced the stack by the two arguments it never cleaned up - a
     *      corrupted return address, which is what a wild `ret` address looks like.
     *   2. `nrex_image` / `nrex_image_view` returned `void *`. Vulkan's non-dispatchable handles are
     *      `uint64_t` on 32-bit builds (the macro keys on `_WIN64`/`__LP64__`), so a pointer return
     *      dropped the top half and the blend pass was handed a truncated handle.
     *
     * A third one the second exposed: the loader context was `void *ctx[]`, and two of its four
     * slots are `VkDevice`/`VkInstance`, which are 64 bits there too.
     *
     * These are read from the source because the defect is a TYPE, and a type is not something a
     * running test can observe on the platform where it is harmless. The assertions are negative
     * on purpose: they fail if someone reintroduces the shape. */
    check(find(usource, "VKAPI_PTR *nrex_gpa_device_fn"),
          "the device resolver carries VKAPI_PTR",
          find(usource, "VKAPI_PTR *nrex_gpa_device_fn")
              ? "__stdcall on x86" : "MISSING - the stack unbalances by 8 bytes per call on 32-bit");
    check(find(usource, "VKAPI_PTR *nrex_gpa_instance_fn"),
          "the instance resolver carries VKAPI_PTR",
          find(usource, "VKAPI_PTR *nrex_gpa_instance_fn")
              ? "__stdcall on x86" : "MISSING");
    check(find(usource, "uint64_t nrex_image(nrex_ui *ui)"),
          "nrex_image returns a 64-bit handle",
          find(usource, "uint64_t nrex_image(nrex_ui *ui)")
              ? "uint64_t" : "MISSING - a void* return truncates VkImage on x86");
    check(find(usource, "uint64_t nrex_image_view(nrex_ui *ui)"),
          "nrex_image_view returns a 64-bit handle",
          find(usource, "uint64_t nrex_image_view(nrex_ui *ui)")
              ? "uint64_t" : "MISSING - a void* return truncates VkImageView on x86");
    check(!find(usource, "void **ctx = (void **)user"),
          "the loader context is not void* slots",
          find(usource, "void **ctx = (void **)user")
              ? "back to void* - VkDevice and VkInstance are truncated on x86"
              : "uint64_t slots");
    check(find(usource, "uint64_t *ctx = (uint64_t *)user"),
          "the loader context uses 64-bit slots",
          find(usource, "uint64_t *ctx = (uint64_t *)user")
              ? "uint64_t slots" : "MISSING");

    /* ---- and the same boundary further in ----
     *
     * Two more of the same two shapes, found by reading rather than by the crash:
     *
     *   - `nr_menu_resolve_fn` (nr_menu.h) wraps `vkGetDeviceProcAddr`/`vkGetInstanceProcAddr` and
     *     had no calling convention, so on x86 it declared them `__cdecl` and every call unbalanced
     *     the stack - the defect `nrex_gpa_fn` had, one header over.
     *   - `nr_menu.c` cast two NON-dispatchable handles (`VkImageView`, `VkBuffer`) through
     *     `uintptr_t`, which is 32 bits on x86, undoing the `uint64_t` the caller had just used.
     *     The `VkCommandBuffer` casts beside them are correct: that handle is a real pointer.
     *
     * The checks read the sources as text, the way the rest of this file does. */
    FILE *menu_h = fopen("src/layer/nr_menu.h", "rb");
    FILE *menu_c = fopen("src/layer/nr_menu.c", "rb");
    if (menu_h && menu_c) {
        static char hsrc[400000];
        static char csrc[400000];
        size_t hn = fread(hsrc, 1, sizeof hsrc - 1, menu_h);
        hsrc[hn] = '\0';
        size_t cn = fread(csrc, 1, sizeof csrc - 1, menu_c);
        csrc[cn] = '\0';
        int resolver_stdcall = find(hsrc, "NR_MENU_CALL *nr_menu_resolve_fn") && find(hsrc, "__stdcall");
        check(resolver_stdcall, "nr_menu.h's resolve pointer declares its calling convention",
              resolver_stdcall ? "NR_MENU_CALL is __stdcall on Windows"
                               : "MISSING - __cdecl, and x86 unbalances the stack on every call");
        int image_view_ok = !find(csrc, "(VkImageView)(uintptr_t)");
        check(image_view_ok, "VkImageView is not narrowed through uintptr_t",
              image_view_ok ? "cast straight from the 64-bit parameter"
                            : "STILL NARROWED - a 64-bit handle cut to 32 on x86");
        int buffer_ok = !find(csrc, "(VkBuffer)(uintptr_t)");
        check(buffer_ok, "VkBuffer is not narrowed through uintptr_t",
              buffer_ok ? "cast straight from the 64-bit parameter"
                        : "STILL NARROWED - a 64-bit handle cut to 32 on x86");
    } else {
        check(0, "nr_menu.h and nr_menu.c were readable", "MISSING");
    }
    if (menu_h) fclose(menu_h);
    if (menu_c) fclose(menu_c);

    /* ---- the focus gate on nr_menu.c's own reader ----
     *
     * `nr_menu.c` reads Home with no gate at all and S/R with only `m->open`, neither of which
     * checks focus - and the ImGui build leaves both live because they are outside the
     * `read_keys` block. So the layer's own gated Home did not protect it: an unfocused Home
     * toggled the effect. The fix gates both on `read_keys`, which the layer holds off while the
     * game is not in front.
     *
     * The owner's constraint, which this must not break: **Home works whether the panel is open
     * or closed.** `read_keys` is 1 in the quad build always and 0 in the ImGui build always, so
     * the gate is about focus, never about the panel. An assertion that Home is gated on
     * `m->open` would be the wrong fix passing. */
    FILE *menu_src = fopen("src/layer/nr_menu.c", "rb");
    if (!menu_src) {
        printf("  FAIL  nr_menu.c was not readable\n");
        return 1;
    }
    static char msource[400000];
    size_t mn = fread(msource, 1, sizeof msource - 1, menu_src);
    msource[mn] = '\0';
    fclose(menu_src);

    check(find(msource, "m->read_keys && key_pressed(m, NR_KEY_TOGGLE)"),
          "nr_menu.c's Home read is gated on read_keys",
          find(msource, "m->read_keys && key_pressed(m, NR_KEY_TOGGLE)")
              ? "gated" : "UNGATED - a second owner of the toggle, acting while the game is behind");
    check(find(msource, "if (m->open && m->read_keys) {"),
          "and S and R are gated on read_keys too",
          find(msource, "if (m->open && m->read_keys) {")
              ? "open + read_keys" : "open only - an unfocused S writes the settings file");
    check(!find(msource, "if (key_pressed(m, NR_KEY_TOGGLE))"),
          "no ungated Home read remains",
          find(msource, "if (key_pressed(m, NR_KEY_TOGGLE))")
              ? "still there" : "none");

    /* The latch, which is the half that turns a gate into a dead key. */
    check(find(msource, "void nr_menu_release_keys(nr_menu *m)"),
          "the menu can forget which keys are held",
          find(msource, "void nr_menu_release_keys(nr_menu *m)")
              ? "present" : "MISSING - the first press after returning to the game is swallowed");
    check(find(lsource, "nr_menu_release_keys(data->menu)"),
          "the layer clears the latch while gated",
          find(lsource, "nr_menu_release_keys(data->menu)")
              ? "cleared" : "NOT CALLED - Home is dead for one press after alt-tab");

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}

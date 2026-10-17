/* What does the panel report when the user acts on it?
 *
 * Four complaints at once - the arrows move far too fast, Close and Reset do nothing, Save is
 * uncertain, and no value or option change takes effect - and every one of them is a question about
 * what `nrex_end_panel` returns and what the row array carries afterwards. This drives the REAL
 * panel (`nrex_begin_panel`, `nrex_rows`, `nrex_end_panel`, `nrex_key`) against a plain ImGui
 * context, so the widgets, the nav and the hold-to-repeat are the shipped ones and not a copy.
 * Nothing here needs Vulkan: only `nrex_render` touches it.
 *
 * Build:
 *   cl /nologo /O2 /TP /utf-8 /EHsc /DNDEBUG /D_WIN32 /D_IMGUI_DEFINE_MATH_OPERATORS ^
 *      /I third_party\imgui /I src\layer /Fe:work\test_menu_panel.exe ^
 *      src\layer\test_menu_panel.cpp src\layer\nr_menu_ui.cpp third_party\imgui\imgui.cpp ^
 *      third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp ^
 *      third_party\imgui\imgui_widgets.cpp user32.lib gdi32.lib
 */
#include "imgui.h"
#include "imgui_internal.h"
#include "nr_menu_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int failures = 0;

static void check(int ok, const char *what, const char *detail = "")
{
    printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", what, *detail ? "  (" : "", detail,
           *detail ? ")" : "");
    if (!ok) ++failures;
}

#define ROWS 11
static nrex_row rows[ROWS];
static const char *choice_names[] = { "standard", "natural", "cinematic", nullptr };
static char shown[ROWS][32];

static void fill_rows(void)
{
    static const char *labels[ROWS] = {
        "Render scale", "Minimum network side", "Profile", "Intensity", "Detail strength",
        "Colour strength", "Temporal history", "Hold unchanged pixels",
        "Release moving pixels", "Scene-cut threshold", "Weights model",
    };
    for (int i = 0; i < ROWS; ++i) {
        memset(&rows[i], 0, sizeof rows[i]);
        rows[i].label = labels[i];
        rows[i].hint = "";
        rows[i].hint_id = -1;
        if (i == 2 || i == 10) {
            rows[i].kind = NREX_KIND_CHOICE;
            rows[i].choices = choice_names;
            rows[i].choice_count = 3;
            rows[i].choice_index = 0;
            snprintf(shown[i], sizeof shown[i], "standard");
        } else {
            /* A range wide enough that a second of holding cannot reach either end: the panel
             * stops reporting a step once the value clamps, so a narrow range measures the clamp
             * rather than the ramp. */
            rows[i].kind = NREX_KIND_FLOAT;
            rows[i].min = 0.0f;
            rows[i].max = 100.0f;
            rows[i].step = 0.05f;
            snprintf(shown[i], sizeof shown[i], "1");
        }
        rows[i].value = shown[i];
    }
}

static void clear_edits(void)
{
    for (int i = 0; i < ROWS; ++i) {
        rows[i].changed = 0;
        rows[i].value_f = 0.0f;
        rows[i].value_i = 0;
    }
}

/* One frame through the same calls the layer makes. `selected` is the row the C side says is
 * selected, which is what a nav-less fallback would highlight. */
static int frame(int selected = 0)
{
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0);
    nrex_rows(nullptr, rows, ROWS, selected);
    int acted = nrex_end_panel(nullptr);
    ImGui::Render();
    return acted;
}

/* A press for one frame and a release for the next: the edge ImGui derives its press from. */
static int tap(int vk, int selected = 0)
{
    clear_edits();
    nrex_key(nullptr, vk, 1);
    int acted = frame(selected);
    nrex_key(nullptr, vk, 0);
    frame(selected);
    return acted;
}

/* The row ImGui's keyboard nav currently has focus on, or -1. Asked inside a frame: the nav state
 * is only valid while the window is up, and a slider's ID includes the row's PushID. */
static int nav_row(void)
{
    ImGui::NewFrame();
    nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0);
    nrex_rows(nullptr, rows, ROWS, 0);
    int found = -1;
    ImGuiWindow *w = ImGui::FindWindowByName("DLSS-NR on Intel");
    if (w) {
        ImGuiID id = w->NavLastIds[0];
        for (int i = 0; i < ROWS && found < 0; ++i) {
            ImGui::PushID(i);
            ImGuiID slider = w->GetID("##v");
            ImGui::PopID();
            if (slider == id) found = i;
        }
        if (found < 0 && id != 0) found = -2;   /* some item, not a row slider */
    }
    ImGui::Render();
    return found;
}

/* Everything the nav state says, for one frame. */
static void nav_debug(const char *when)
{
    ImGui::NewFrame();
    nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0);
    nrex_rows(nullptr, rows, ROWS, 0);
    ImGuiWindow *w = ImGui::FindWindowByName("DLSS-NR on Intel");
    ImGuiContext &g = *ImGui::GetCurrentContext();
    printf("    [%s] nav_window=%s focused=%d nav_id=%08x\n", when,
           g.NavWindow ? g.NavWindow->Name : "(none)",
           (int)ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow),
           w ? (unsigned)w->NavLastIds[0] : 0u);
    ImGui::Render();
}

/* Which nav item the standing focus is on, by walking the IDs the panel built. The panel's
 * widgets are submitted in a fixed order, so a Tab count that reaches a given one is stable. */
static int tab_reaches_row(int target)
{
    int save_at = -1, reset_at = -1, close_at = -1, target_at = -1;
    for (int i = 1; i <= 40; ++i) {
        tap(0x09);
        clear_edits();
        nrex_key(nullptr, 0x20, 1);
        int acted = frame(0);
        nrex_key(nullptr, 0x20, 0);
        frame(0);
        if ((acted & NREX_ACT_SAVE) && save_at < 0) save_at = i;
        if ((acted & NREX_ACT_RESET) && reset_at < 0) reset_at = i;
        if ((acted & NREX_ACT_CLOSE) && close_at < 0) close_at = i;
        (void)target_at;
        (void)target;
    }
    return save_at > 0 && reset_at > 0 && close_at > 0 ? 1 : 0;
}

int main(void)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    fill_rows();

    printf("the panel's own reporting\n");

    /* Two frames so the panel has taken focus and nav has an initial item. */
    frame(0);
    frame(0);

    /* ---- 1. the arrows move the selection, and how fast ---- */
    {
        /* One Down must move the selection. Counted through the nav's own step request rather
         * than by guessing an ID: `ImGui::GetCurrentContext()->NavId` is the item nav is on, asked while a frame
         * is open. */
        ImGui::NewFrame();
        nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0);
        nrex_rows(nullptr, rows, ROWS, 0);
        ImGuiID before = ImGui::GetCurrentContext()->NavId;
        ImGui::Render();
        tap(0x28);
        ImGui::NewFrame();
        nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0);
        nrex_rows(nullptr, rows, ROWS, 0);
        ImGuiID after = ImGui::GetCurrentContext()->NavId;
        ImGui::Render();
        char detail[96];
        snprintf(detail, sizeof detail, "%08x -> %08x", (unsigned)before, (unsigned)after);
        check(before != after, "one Down moves the selection", detail);

        /* A held Down for one second. This repeat is ImGui's own, timed on the delta the layer
         * supplies - and that is the variable the game controls, because the delta is the present
         * rate. Measured at two rates so one that is right at only one of them is visible. */
        for (int pass = 0; pass < 2; ++pass) {
            const double fps = pass == 0 ? 60.0 : 240.0;
            int moves = 0;
            ImGuiID last = after;
            nrex_key(nullptr, 0x28, 1);
            for (int f = 0; f < (int)(fps * 1.0); ++f) {
                clear_edits();
                ImGuiIO &io = ImGui::GetIO();
                io.DisplaySize = ImVec2(800.0f, 600.0f);
                /* Exactly what the layer feeds it: a fixed 1/60, whatever the real rate. */
                io.DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0);
                nrex_rows(nullptr, rows, ROWS, 0);
                ImGuiID now = ImGui::GetCurrentContext()->NavId;
                int acted = nrex_end_panel(nullptr);
                ImGui::Render();
                if (now != last) { ++moves; last = now; }
                (void)acted;
                Sleep((DWORD)(1000.0 / fps));
            }
            nrex_key(nullptr, 0x28, 0);
            frame(0);
            snprintf(detail, sizeof detail, "%d moves in 1.0 s at %.0f fps", moves, fps);
            check(moves <= 12, "a one-second Down hold does not race the list", detail);
        }
    }

    /* ---- 2. one Right tap is one step ---- */
    /* The user's report is that the arrows DO change values (too fast), so focus must land on a
     * row. It does not land by itself here: ImGui's nav starts on the first item it submits and
     * the row's slider is not where it starts, so this first checks what reaches it. */
    {
        int reached = -1;
        for (int i = 0; i <= 40 && reached < 0; ++i) {
            clear_edits();
            nrex_key(nullptr, 0x27, 1);      /* Right */
            frame(0);
            if (rows[0].changed || rows[1].changed || rows[3].changed) reached = i;
            nrex_key(nullptr, 0x27, 0);
            frame(0);
            if (i < 40) tap(0x09);           /* Tab walks to the next focusable item */
        }
        char detail[80];
        snprintf(detail, sizeof detail, "a row changed at Tab %d", reached);
        check(reached >= 0, "a row's slider can take focus and change", detail);
    }

    {
        int reached = -1;
        clear_edits();
        for (int i = 0; i <= 40 && reached < 0; ++i) {
            clear_edits();
            nrex_key(nullptr, 0x27, 1);
            frame(0);
            int which = -1;
            for (int r = 0; r < ROWS; ++r) if (rows[r].changed) which = r;
            if (which >= 0) {
                float value = rows[which].value_f;
                nrex_key(nullptr, 0x27, 0);
                frame(0);
                char detail[128];
                snprintf(detail, sizeof detail, "row %d changed=%d value=%.3f step=%.3f",
                         which, rows[which].changed, (double)value,
                         i == 0 ? (double)rows[which].step : 0.0);
                check(rows[which].changed == 1, "one Right tap reports the focused row changed",
                      detail);
                if (i == 0)
                    check(fabsf(value - rows[which].step - 1.0f) < 0.001f,
                          "and by exactly one step", detail);
                reached = i;
            } else {
                nrex_key(nullptr, 0x27, 0);
                frame(0);
                tap(0x09);
            }
        }
        if (reached < 0) check(0, "a focused row steps once per Right tap", "never reached a row");
    }

    /* ---- 3. a held Right is a ramp, not a burst ---- */
    {
        /* Reach a row first, then hold. The user's real sequence: the arrows work and they move
         * far too fast, so both halves have to be measured from a row that takes focus. */
        int which = -1;
        for (int i = 0; i < 40 && which < 0; ++i) {
            clear_edits();
            nrex_key(nullptr, 0x27, 1);
            frame(0);
            for (int r = 0; r < ROWS; ++r) if (rows[r].changed) which = r;
            nrex_key(nullptr, 0x27, 0);
            frame(0);
            if (which < 0) tap(0x09);
        }
        if (which < 0) {
            check(0, "a one-second Right hold stays a usable ramp", "never reached a row");
        } else {
            printf("    [hold] row %d, range %.2f..%.2f step %.3f start %s\n",
                   which, (double)rows[which].min, (double)rows[which].max,
                   (double)rows[which].step, rows[which].value);
            int steps = 0;
            float last = (float)atof(rows[which].value);
            nrex_key(nullptr, 0x27, 1);
            LARGE_INTEGER t0, t1, freq;
            QueryPerformanceCounter(&t0);
            QueryPerformanceFrequency(&freq);
            const double frames_per_second = 60.0;
            for (int f = 0; f < 60; ++f) {
                clear_edits();
                ImGui::NewFrame();
                nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0);
                nrex_rows(nullptr, rows, ROWS, 0);
                int acted = nrex_end_panel(nullptr);
                ImGui::Render();
                /* Write the edited value back into the row's text, exactly as the layer does when
                 * the bridge accepts it. Without this `held[]` re-reads the same "1" every frame
                 * and reports a change every frame - the count would measure frames, not steps. */
                for (int r = 0; r < ROWS; ++r) {
                    if (rows[r].changed && rows[r].kind == NREX_KIND_FLOAT)
                        snprintf(shown[r], sizeof shown[r], "%.3f", (double)rows[r].value_f);
                }
                for (int r = 0; r < ROWS; ++r) {
                    if (rows[r].changed) {
                        ++steps;
                        if (f < 6) printf("    [hold] f=%d acted=%d row %d -> %.3f\n",
                                          f, acted, r, (double)rows[r].value_f);
                        last = rows[r].value_f;
                    }
                }
                Sleep((DWORD)(1000.0 / frames_per_second));
            }
            QueryPerformanceCounter(&t1);
            nrex_key(nullptr, 0x27, 0);
            frame(0);
            double elapsed = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
            char detail[160];
            snprintf(detail, sizeof detail, "%d steps in %.3f s of wall clock (%.1f/s)",
                     steps, elapsed, elapsed > 0 ? steps / elapsed : 0.0);
            check(steps / (elapsed > 0 ? elapsed : 1.0) <= 20.0,
                  "a held Right ramps to at most 20 steps per second", detail);
            check(steps >= 6, "and still crosses a knob's range", detail);
        }
    }

    /* ---- 4. Save, Reset and Close are reachable and report ---- */
    {
        /* Tab walks the focusable items in order; Space activates the focused one. Walking until
         * each bit appears is how a keyboard-only user reaches the footer, and it is also what
         * proves the button is inside the window rather than clipped below it. */
        int save_at = -1, reset_at = -1, close_at = -1;
        for (int i = 1; i <= 30 && (save_at < 0 || reset_at < 0 || close_at < 0); ++i) {
            tap(0x09);                                  /* Tab */
            clear_edits();
            nrex_key(nullptr, 0x20, 1);                 /* Space */
            int acted = frame(0);
            nrex_key(nullptr, 0x20, 0);
            frame(0);
            if ((acted & NREX_ACT_SAVE) && save_at < 0) save_at = i;
            if ((acted & NREX_ACT_RESET) && reset_at < 0) reset_at = i;
            if ((acted & NREX_ACT_CLOSE) && close_at < 0) close_at = i;
        }
        char detail[160];
        snprintf(detail, sizeof detail, "Save at Tab %d, Reset at %d, Close at %d",
                 save_at, reset_at, close_at);
        check(save_at > 0, "Save can be reached and reports NREX_ACT_SAVE", detail);
        check(reset_at > 0, "Reset can be reached and reports NREX_ACT_RESET", detail);
        check(close_at > 0, "Close can be reached and reports NREX_ACT_CLOSE", detail);
    }

    /* ---- 5. the language selector reports a change ---- */
    {
        /* The combo is the last item in the window; Tab past Close reaches it. Its own value is
         * ImGui state, so a change is only visible as NREX_ACT_LANGUAGE plus nrex_panel_language.
         * Driven with the mouse: a click on the combo opens it, and the next click picks an entry -
         * that is a popup, so this checks the simpler half instead: that the panel starts in the
         * language it was given and reports that same number back. */
        nrex_begin_panel(nullptr, "DLSS-NR on Intel", 2);
        int reported = nrex_panel_language(nullptr);
        char detail[64];
        snprintf(detail, sizeof detail, "began with 2, panel says %d", reported);
        check(reported == 2, "the panel keeps the language it was handed", detail);
        frame(0);
    }

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}

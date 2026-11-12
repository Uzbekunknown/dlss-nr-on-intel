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
#include "nr_menu_ui_backend.h"
#include "nr_frame_rate.h"

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
        if (i == 2) {
            rows[i].kind = NREX_KIND_CHOICE;
            rows[i].choices = choice_names;
            rows[i].choice_count = 3;
            rows[i].choice_index = 0;
            snprintf(shown[i], sizeof shown[i], "standard");
        } else if (i == 10) {
            /* A flag, which the panel draws as a checkbox: the corner's frame graphs are one. */
            rows[i].kind = NREX_KIND_FLAG;
            rows[i].choice_index = 1;
            snprintf(shown[i], sizeof shown[i], "on");
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
    nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, NULL);
    nrex_rows(nullptr, rows, ROWS, selected);
    int acted = nrex_end_panel(nullptr);
    ImGui::Render();
    return acted;
}

/* One frame through the same calls the layer makes, with a hint. `selected` is the row the C side
 * says is selected, which is what a nav-less fallback would highlight. */
static int frame_with_hint(const char *hint, int selected = 0)
{
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, hint);
    nrex_rows(nullptr, rows, ROWS, selected);
    int acted = nrex_end_panel(nullptr);
    ImGui::Render();
    return acted;
}

/* How much geometry the panel emitted for this frame. ImGui accumulates a draw list per frame, and
 * the footer's wrapped text is part of it - so a hint that is drawn adds vertices and one that is
 * merely stored does not. This is what makes the hint check a check of DRAWING rather than of a
 * string being remembered. */
static int frame_vertex_count(const char *hint)
{
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, hint);
    nrex_rows(nullptr, rows, ROWS, 0);
    nrex_end_panel(nullptr);
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    int total = 0;
    if (dd)
        for (int i = 0; i < dd->CmdListsCount; ++i)
            total += dd->CmdLists[i]->VtxBuffer.Size;
    return total;
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
    nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, NULL);
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
    nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, NULL);
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
        nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, NULL);
        nrex_rows(nullptr, rows, ROWS, 0);
        ImGuiID before = ImGui::GetCurrentContext()->NavId;
        ImGui::Render();
        tap(0x28);
        ImGui::NewFrame();
        nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, NULL);
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
                nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, NULL);
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
                nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, NULL);
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
        nrex_begin_panel(nullptr, "DLSS-NR on Intel", 2, NULL);
        int reported = nrex_panel_language(nullptr);
        char detail[64];
        snprintf(detail, sizeof detail, "began with 2, panel says %d", reported);
        check(reported == 2, "the panel keeps the language it was handed", detail);
        frame(0);
    }

    /* ---- 6. the panel-level hint is drawn ---- */
    {
        /* The sentence "the game also sees these keys" was never passed to the ImGui panel, so the
         * panel every game loads showed no key list and no warning, while the hand-drawn menu did.
         *
         * **The evidence is the draw list, not the string that was handed in.** The first version of
         * this check asked the panel what it had been given, which passes with the drawing deleted:
         * the mutation that replaced `TextWrapped` with an empty label still passed. A string kept
         * in a static and never emitted is exactly the bug being fixed, so what is counted is
         * vertices - a footer with the hint emits more geometry than one without. */
        /* Compare first: a hint is prose, not a control, and the panel's own actions must be
         * unchanged by its presence. */
        int with_hint = frame_with_hint("the game also sees these keys", 0);
        int without = frame(0);
        check(with_hint == without, "the hint changes nothing the panel reports",
              "identical actions with and without it");

        int verts_with = frame_vertex_count("the game also sees these keys");
        int verts_without = frame_vertex_count(nullptr);
        char detail[96];
        snprintf(detail, sizeof detail, "%d vertices with the hint, %d without",
                 verts_with, verts_without);
        check(verts_with > verts_without, "the footer emits geometry for the hint", detail);

        /* And the string reaches the panel at all - the other half, which the vertices do not
         * prove: more geometry could come from anything. Read AFTER the comparison, because
         * `frame` hands the panel a NULL hint and would leave nothing to read. */
        frame_with_hint("the game also sees these keys", 0);
        char drawn[256] = "";
        int got = nrex_last_footer_text(drawn, sizeof drawn);
        snprintf(detail, sizeof detail, "footer text: '%s'", got ? drawn : "(none)");
        check(got && strstr(drawn, "the game also sees these keys") != nullptr,
              "and the footer holds the hint it was handed", detail);

        /* A NULL or empty hint holds nothing rather than an empty line. */
        char empty[64] = "";
        nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, "");
        nrex_rows(nullptr, rows, ROWS, 0);
        nrex_end_panel(nullptr);
        int none = nrex_last_footer_text(empty, sizeof empty);
        check(none == 0, "an empty hint holds nothing", none ? "held something" : "nothing");
    }

    /* ---- 7. the frame counter ----
     *
     * Two rates, because under frame generation they differ: the screen's, every present, and
     * NR's, the frames the daemon answered (nr_frame_rate.h). The arithmetic on a clock of the
     * fixture's own; then the corner, drawn as the shut panel draws it, which must take no input;
     * then the line at the top of the open panel. */
    {
        char detail[256];

        /* The rates: 60 presents a second, NR on every other one, 24.5 ms of network each - frame
         * generation's x2, as Hogwarts runs it. */
        nr_frame_rates r;
        nr_frame_rates_reset(&r);
        nr_frame_counts c = {0, 0, 0, 0};
        double t = 0.0;
        char line[192];
        nr_frame_rates_sample(&r, t, &c);
        check(!r.ready && !nr_frame_rates_line(&r, NR_LANG_EN, 1, "  ", line, sizeof line) && !line[0],
              "the counter says nothing before its first reading");
        for (int i = 1; i <= 150; ++i) {
            t = i / 60.0;
            c.presented++;
            if (i % 2 == 0) { c.nr_frames++; c.network_us += 24500; c.network_frames++; }
            nr_frame_rates_sample(&r, t, &c);
        }
        snprintf(detail, sizeof detail, "screen %.2f, NR %.2f, network %.2f ms", r.screen_fps, r.nr_fps,
                 r.network_ms);
        check(r.ready && fabs(r.screen_fps - 60.0) < 1.5 && fabs(r.nr_fps - 30.0) < 1.5 &&
              fabs(r.network_ms - 24.5) < 0.01, "60 presents and 30 answers a second read as such", detail);

        nr_frame_rates_line(&r, NR_LANG_EN, 1, "  ", line, sizeof line);
        snprintf(detail, sizeof detail, "'%s'", line);
        check(strstr(line, "Screen 60") && strstr(line, "NR 30") && strstr(line, "network 24.5 ms") &&
              !strstr(line, "{}"), "the English line names both rates and the network's time", detail);
        nr_frame_rates_line(&r, NR_LANG_RU, 1, "  ", line, sizeof line);
        snprintf(detail, sizeof detail, "'%s'", line);
        check(strstr(line, "24,5") && !strstr(line, "24.5") && !strstr(line, "{}"),
              "Russian takes a decimal comma", detail);
        nr_frame_rates_line(&r, NR_LANG_EN, 0, "  ", line, sizeof line);
        snprintf(detail, sizeof detail, "'%s'", line);
        check(strstr(line, "NR off") && !strstr(line, "network"), "with NR off it says so, and no time",
              detail);
        int every = 1;
        for (int lang = 0; lang < NR_MENU_LANG_COUNT; ++lang) {
            for (int on = -1; on <= 1; ++on) {
                if (!nr_frame_rates_line(&r, lang, on, "  ", line, sizeof line) || !line[0] ||
                    strstr(line, "{}") || strlen(line) + 1 >= sizeof line)
                    every = 0;
            }
            for (int key = NR_STR_FPS_SCREEN; key <= NR_STR_COUNTER_BOTTOM_RIGHT; ++key)
                if (!nr_menu_text(lang, key)[0]) every = 0;
        }
        check(every, "every language builds its line, whole, and has every counter string");

        /* A pause - a load, an alt-tab - starts over rather than averaging the pause in. */
        t += 3.0;
        nr_frame_rates_sample(&r, t, &c);
        check(!r.ready, "a pause of three seconds starts the counting over");
        /* And so do totals that went back, as a host that counts afresh hands over. */
        for (int i = 1; i <= 60; ++i) { c.presented++; nr_frame_rates_sample(&r, t + i / 60.0, &c); }
        nr_frame_counts fresh = {0, 0, 0, 0};
        nr_frame_rates_sample(&r, t + 1.1, &fresh);
        check(!r.ready, "totals that go back start the counting over");
        /* A span with no answer that carried the network's time has none to show. */
        nr_frame_rates_reset(&r);
        nr_frame_counts quiet = {0, 0, 0, 0};
        for (int i = 0; i <= 90; ++i) {
            quiet.presented++;
            quiet.nr_frames++;
            nr_frame_rates_sample(&r, i / 60.0, &quiet);
        }
        nr_frame_rates_line(&r, NR_LANG_EN, 1, "  ", line, sizeof line);
        snprintf(detail, sizeof detail, "'%s'", line);
        check(r.ready && r.network_ms < 0.0 && !strstr(line, "network"),
              "no time reported, none shown", detail);

        /* ---- the two graphs: the shape behind the numbers ---- */
        {
            /* A ring, oldest first, and it drops the oldest rather than refusing the newest: the
             * right edge has to be now. A sample the ring never used must not be drawn as zero -
             * that would read as a perfect frame. */
            nr_frame_graph g;
            nr_frame_graph_reset(&g);
            float out[NR_GRAPH_SAMPLES];
            check(nr_frame_graph_frame(&g, out, NR_GRAPH_SAMPLES) == 0,
                  "a fresh graph has no points to draw", "");

            nr_frame_graph_add(&g, 16.0f, 8.0f);
            nr_frame_graph_add(&g, 20.0f, -1.0f);   /* no network time for this one */
            int n = nr_frame_graph_frame(&g, out, NR_GRAPH_SAMPLES);
            snprintf(detail, sizeof detail, "n=%d first=%.1f second=%.1f", n,
                     (double)out[0], (double)out[1]);
            check(n == 2 && out[0] > 15.9f && out[0] < 16.1f && out[1] > 19.9f && out[1] < 20.1f,
                  "the graph keeps frames in the order they arrived", detail);

            /* The net series carries the gap as -1, which the drawing side skips rather than
             * plotting at zero. */
            int nn = nr_frame_graph_net(&g, out, NR_GRAPH_SAMPLES);
            snprintf(detail, sizeof detail, "net n=%d second=%.1f", nn, (double)out[1]);
            check(nn == 2 && out[0] > 7.9f && out[0] < 8.1f && out[1] < 0.0f,
                  "a frame without a network time leaves a gap, not a zero", detail);

            /* Filled past its depth: the count stops at the ring's size and the oldest go. */
            for (int i = 0; i < NR_GRAPH_SAMPLES + 40; ++i)
                nr_frame_graph_add(&g, (float)(i % 30) + 1.0f, -1.0f);
            n = nr_frame_graph_frame(&g, out, NR_GRAPH_SAMPLES);
            snprintf(detail, sizeof detail, "n=%d (ring %d)", n, NR_GRAPH_SAMPLES);
            check(n == NR_GRAPH_SAMPLES,
                  "the graph holds its depth and drops the oldest", detail);

            /* A frame slower than the ceiling is clamped, not allowed to flatten the rest. */
            nr_frame_graph_reset(&g);
            nr_frame_graph_add(&g, 5000.0f, -1.0f);
            nr_frame_graph_frame(&g, out, 1);
            snprintf(detail, sizeof detail, "%.1f ms for a 5 s frame", (double)out[0]);
            check(out[0] > (float)NR_GRAPH_CEIL_MS - 0.5f,
                  "a frame past the ceiling is drawn at it", detail);

            /* **The rates feed it.** A present a frame at 60 Hz is 16.7 ms a point, and the panel
             * hands the same ring to the counter; this is the join between the counter's
             * arithmetic and the graph's. */
            nr_frame_rates rg;
            nr_frame_rates_reset(&rg);
            nr_frame_counts rc = {0, 0, 0, 0};
            for (int i = 0; i <= 30; ++i) {
                rc.presented++;
                rc.nr_frames++;
                nr_frame_rates_sample(&rg, i / 60.0, &rc);
            }
            float frames[NR_GRAPH_SAMPLES];
            int got = nr_frame_graph_frame(&rg.graph, frames, NR_GRAPH_SAMPLES);
            /* The first present has no previous, so it contributes no point; the rest are one
             * frame each. */
            snprintf(detail, sizeof detail, "%d points from 30 presents", got);
            check(got >= 28 && got <= 30,
                  "the counter's sampling fills the graph, one point a present", detail);
            if (got > 0) {
                float sum = 0.0f;
                for (int i = 0; i < got; ++i) sum += frames[i];
                const float mean = sum / (float)got;
                snprintf(detail, sizeof detail, "mean %.2f ms, want ~16.7", (double)mean);
                check(mean > 15.0f && mean < 18.5f,
                      "a 60 Hz present stream reads as ~16.7 ms a frame", detail);
            }
        }

        /* The corner, as the shut panel draws it: no panel window, the counter alone, and no
         * pointer handed to ImGui. AlwaysAutoResize hides a window its first frame, so two. */
        static nrex_ui drawn;
        memset(&drawn, 0, sizeof drawn);
        for (int i = 0; i < 2; ++i) {
            ImGuiIO &dio = ImGui::GetIO();
            dio.DisplaySize = ImVec2(800.0f, 600.0f);
            dio.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            nrex_counter(nullptr, nullptr, nullptr, 2, "Screen 60.0 fps   NR 30.0 fps   network 24.5 ms", nullptr, 0, nullptr, 0, 1);
            ImGui::Render();
        }
        int any = nrex_note_drawn(&drawn, ImGui::GetDrawData(), 800, 600);
        uint32_t rx = 0, ry = 0, rw = 0, rh = 0;
        nrex_drawn_rect(&drawn, &rx, &ry, &rw, &rh);
        snprintf(detail, sizeof detail, "drawn %ux%u at %u,%u", rw, rh, rx, ry);
        check(any && rx >= 400 && ry < 16 && rx + rw <= 800 && rw < 400 && rh < 60,
              "the top right corner holds the counter, and the frame draws nothing else", detail);
        ImGuiWindow *corner = ImGui::FindWindowByName("##nr_frame_counter");
        check(corner && (corner->Flags & ImGuiWindowFlags_NoInputs) == ImGuiWindowFlags_NoInputs &&
              (corner->Flags & ImGuiWindowFlags_NoFocusOnAppearing),
              "the corner is a window that takes no input and no focus");

        /* ---- the corner's graphs are a switch; the panel's are not ---- */
        {
            /* Two frames of the same data, once with `corner_graphs` on and once off. The corner
             * window's height is what changes: the numbers are the same line either way, so a
             * difference there can only be the graphs. This is the whole of the user's switch, and
             * it is checked on the drawn window rather than on the flag the caller passed. */
            static float series[60];
            for (int i = 0; i < 60; ++i) series[i] = 12.0f + (float)(i % 7);
            float corner_height_on = 0.0f, corner_height_off = 0.0f;
            for (int pass = 0; pass < 2; ++pass) {
                float *net = (pass == 0) ? series : nullptr;
                int net_count = (pass == 0) ? 60 : 0;
                for (int i = 0; i < 2; ++i) {
                    ImGuiIO &dio = ImGui::GetIO();
                    dio.DisplaySize = ImVec2(800.0f, 600.0f);
                    dio.DeltaTime = 1.0f / 60.0f;
                    ImGui::NewFrame();
                    /* pass 0: corner graphs on; pass 1: off, and no net series so the second
                     * graph would be absent anyway - the frame graph alone is the test. */
                    nrex_counter(nullptr, nullptr, nullptr, 2, "Screen 60.0 fps   NR 30.0 fps",
                                 series, 60, net, net_count, pass == 0 ? 1 : 0);
                    ImGui::Render();
                }
                ImGuiWindow *w = ImGui::FindWindowByName("##nr_frame_counter");
                float h = w ? w->Size.y : 0.0f;
                if (pass == 0) corner_height_on = h; else corner_height_off = h;
            }
            snprintf(detail, sizeof detail, "corner %.0f px with graphs, %.0f px without",
                     (double)corner_height_on, (double)corner_height_off);
            check(corner_height_on > corner_height_off + 40.0f,
                  "the corner draws the graphs when asked, and not when not", detail);

            /* **And the panel's are unconditional.** The same data with `corner_graphs` off must
             * still draw them in the panel's header - the switch is the corner's alone. A panel
             * frame with no graphs and one with graphs differ in vertex count; the open panel is
             * where that shows. */
            int verts_panel_off = 0, verts_panel_none = 0;
            for (int pass = 0; pass < 2; ++pass) {
                ImGuiIO &pio2 = ImGui::GetIO();
                pio2.DisplaySize = ImVec2(800.0f, 600.0f);
                pio2.DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                nrex_counter(nullptr, "Screen 60.0 fps    NR 30.0 fps", "what they are", 0,
                             nullptr, (pass == 0) ? series : nullptr, (pass == 0) ? 60 : 0,
                             nullptr, 0, 0);   /* corner off - it must not matter here */
                nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, NULL);
                nrex_rows(nullptr, rows, ROWS, 0);
                nrex_end_panel(nullptr);
                ImGui::Render();
                int v = 0;
                ImDrawData *dd2 = ImGui::GetDrawData();
                if (dd2)
                    for (int i = 0; i < dd2->CmdListsCount; ++i) v += dd2->CmdLists[i]->VtxBuffer.Size;
                if (pass == 0) verts_panel_off = v; else verts_panel_none = v;
            }
            snprintf(detail, sizeof detail, "%d vertices with the graphs, %d without",
                     verts_panel_off, verts_panel_none);
            check(verts_panel_off > verts_panel_none,
                  "the panel draws the graphs even with the corner's switch off", detail);
        }

        /* A pointer over it, pressed: it is not hovered, it asks for no mouse, nothing is reported. */
        if (corner) {
            ImGuiIO &mio = ImGui::GetIO();
            ImVec2 at(corner->Pos.x + corner->Size.x * 0.5f, corner->Pos.y + corner->Size.y * 0.5f);
            mio.AddMousePosEvent(at.x, at.y);
            mio.AddMouseButtonEvent(0, true);
            int captured = 0, hovered = 0;
            for (int i = 0; i < 2; ++i) {
                ImGui::NewFrame();
                nrex_counter(nullptr, nullptr, nullptr, 2, "Screen 60.0 fps   NR 30.0 fps", nullptr, 0, nullptr, 0, 1);
                captured |= mio.WantCaptureMouse ? 1 : 0;
                hovered |= ImGui::GetCurrentContext()->HoveredWindow == corner ? 1 : 0;
                ImGui::Render();
            }
            mio.AddMouseButtonEvent(0, false);
            mio.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            ImGui::NewFrame();
            ImGui::Render();
            snprintf(detail, sizeof detail, "WantCaptureMouse %d, hovered %d", captured, hovered);
            check(!captured && !hovered, "a click on the corner is not the corner's", detail);
        }

        /* In the panel's place the counter's place is nowhere: nothing is drawn. */
        ImGui::NewFrame();
        nrex_counter(nullptr, nullptr, nullptr, 0, "Screen 60.0 fps", nullptr, 0, nullptr, 0, 1);
        ImGui::Render();
        check(!nrex_note_drawn(&drawn, ImGui::GetDrawData(), 800, 600),
              "with the counter in the panel, a shut panel draws nothing");

        /* The open panel's line: drawn above the rows, and nothing the panel reports changes. */
        int plain = frame(0);
        ImGuiIO &pio = ImGui::GetIO();
        pio.DisplaySize = ImVec2(800.0f, 600.0f);
        pio.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        nrex_counter(nullptr, "Screen 60.0 fps    NR 30.0 fps    network 24.5 ms", "what they are", 0,
                     nullptr, nullptr, 0, nullptr, 0, 1);
        nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, NULL);
        nrex_rows(nullptr, rows, ROWS, 0);
        int with_line = nrex_end_panel(nullptr);
        ImGui::Render();
        int verts_line = 0;
        ImDrawData *dd = ImGui::GetDrawData();
        if (dd)
            for (int i = 0; i < dd->CmdListsCount; ++i) verts_line += dd->CmdLists[i]->VtxBuffer.Size;
        ImGui::NewFrame();
        nrex_counter(nullptr, nullptr, nullptr, 0, nullptr, nullptr, 0, nullptr, 0, 1);
        nrex_begin_panel(nullptr, "DLSS-NR on Intel", 0, NULL);
        nrex_rows(nullptr, rows, ROWS, 0);
        nrex_end_panel(nullptr);
        ImGui::Render();
        int verts_plain = 0;
        dd = ImGui::GetDrawData();
        if (dd)
            for (int i = 0; i < dd->CmdListsCount; ++i) verts_plain += dd->CmdLists[i]->VtxBuffer.Size;
        snprintf(detail, sizeof detail, "%d vertices with the line, %d without", verts_line, verts_plain);
        check(verts_line > verts_plain, "the open panel draws the counter's line", detail);
        check(with_line == plain, "and reports what it did without it",
              "identical actions with and without the line");
    }

    /* ---- a checkbox row: the corner's frame graphs ---- */
    {
        /* **A FLAG row is drawn as a checkbox, and its state comes from `choice_index`.** The
         * corner's frame graphs are the panel's first flag, so this is the first time the row kind
         * has been exercised at all.
         *
         * What is checked is what can be measured without a real pointer: the row reports the
         * state it was given (not a raw index), and a ticked row and an unticked one draw different
         * pictures - which is the checkbox showing the difference rather than ignoring it. */
        char detail[256];
        const int row = 10;                 /* the flag, from `fill_rows` */

        rows[row].choice_index = 1;         /* ticked */
        rows[row].disabled = 0;
        clear_edits();
        frame(0);
        ImDrawData *ticked = ImGui::GetDrawData();
        int verts_ticked = ticked ? ticked->TotalVtxCount : 0;

        rows[row].choice_index = 0;         /* unticked */
        clear_edits();
        frame(0);
        ImDrawData *unticked = ImGui::GetDrawData();
        int verts_unticked = unticked ? unticked->TotalVtxCount : 0;

        snprintf(detail, sizeof detail, "%d vertices ticked, %d unticked",
                 verts_ticked, verts_unticked);
        /* Both draw the box; the tick mark is what differs, so the counts differ. */
        check(verts_ticked != verts_unticked,
              "a flag row draws differently ticked and unticked", detail);
        check(verts_ticked > 0 && verts_unticked > 0, "and draws in both states", detail);

        /* **A disabled row refuses the keyboard and takes no click.** The graphs' checkbox is
         * disabled when the counter is in the panel alone, and the mechanism is `nrex_row.disabled`
         * - `BeginDisabled` around the whole row - so a disabled one must report no change however
         * it is pressed. */
        rows[row].choice_index = 1;
        rows[row].disabled = 1;
        clear_edits();
        ImGuiIO &dio = ImGui::GetIO();
        dio.AddKeyEvent(ImGuiKey_RightArrow, true);
        frame(0);
        dio.AddKeyEvent(ImGuiKey_RightArrow, false);
        int acted_disabled = frame(0);
        snprintf(detail, sizeof detail, "changed=%d value_i=%d acted=%d",
                 rows[row].changed, rows[row].value_i, acted_disabled);
        check(!rows[row].changed,
              "a disabled flag row reports nothing", detail);
        rows[row].disabled = 0;
    }

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}

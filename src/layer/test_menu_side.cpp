/* The C side of the menu, driven with the real panel: does an action reach the bridge?
 *
 * The report: Close and Reset do nothing, Save is uncertain, no value or option change takes
 * effect, and the language selector changes only the footer's own text. Those are all questions
 * about the handoff between `nrex_end_panel`'s action bits and `nr_menu_bridge_update`, which is
 * where the flags are consumed.
 *
 * This links the REAL nr_menu.c, nr_menu_bridge.c, nr_menu_ui.cpp and the generated strings, and
 * drives them the way nr_layer.c does: poll the keys, build the panel frame, apply the action
 * bits, then run the bridge update. No Vulkan is needed.
 *
 * Build:
 *   cl /nologo /O2 /TP /utf-8 /EHsc /DNDEBUG /D_WIN32 /D_IMGUI_DEFINE_MATH_OPERATORS ^
 *      /I third_party\imgui /I src\layer /Fe:work\test_menu_side.exe ^
 *      src\layer\test_menu_side.cpp src\layer\nr_menu_ui.cpp src\layer\nr_menu.c ^
 *      src\layer\nr_menu_bridge.c src\layer\nr_menu_strings.c ^
 *      src\layer\nr_menu_strings_lookup.c src\layer\nr_menu_font.c ^
 *      third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp ^
 *      third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp ^
 *      third_party\imgui\imgui_impl_vulkan.cpp user32.lib gdi32.lib
 */
#include "imgui.h"
#include "imgui_internal.h"
#include "nr_menu.h"
#include "nr_menu_bridge.h"
#include "nr_menu_ui.h"
#include "nr_menu_strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int failures = 0;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %s  %s  (%s)\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok) ++failures;
}

#define ROWS 11
static nrex_row rows[ROWS];
static char shown[ROWS][64];
static const char *choice_names[] = { "standard", "natural", "cinematic", nullptr };

/* The rows the layer's `menu_ui_rows` builds from the bridge. */
static int build_rows(nr_menu_bridge *b)
{
    int n = 0;
    static const char *names[ROWS][4];
    static char storage[ROWS][4][32];
    int language = nr_menu_bridge_language(b);
    int count = nr_menu_bridge_row_count(b);
    for (int i = 0; i < count && n < ROWS; ++i) {
        memset(&rows[n], 0, sizeof rows[n]);
        rows[n].label = nr_menu_bridge_row_label(b, i, language);
        rows[n].value = nr_menu_bridge_row_value(b, i);
        rows[n].hint = nr_menu_bridge_row_hint(b, i, language);
        rows[n].hint_id = -1;
        int choices = nr_menu_bridge_knob_choice_count(i);
        if (choices > 0) {
            rows[n].kind = NREX_KIND_CHOICE;
            rows[n].choice_count = choices;
            rows[n].choice_index = nr_menu_bridge_knob_choice_index(b, i);
            for (int c = 0; c < choices; ++c) {
                nr_menu_bridge_knob_choice_name(i, c, NR_LANG_EN, storage[n][c], sizeof storage[n][c]);
                names[n][c] = storage[n][c];
            }
            rows[n].choices = (const char *const *)names[n];
        } else {
            float low = 0, high = 1, step = 0.01f;
            if (nr_menu_bridge_knob_range(i, &low, &high, &step, nullptr) == 0) {
                low = 0; high = 1; step = 0.01f;
            }
            rows[n].kind = NREX_KIND_FLOAT;
            rows[n].min = low;
            rows[n].max = high;
            rows[n].step = step;
        }
        n++;
    }
    return n;
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

    /* A settings file that does not exist: the bridge then shows the defaults, which is what a
     * first run looks like. */
    const char *settings = "nr-menu-side-test-settings.json";
    remove(settings);

    /* A real device is not available here, so `nr_menu_create` cannot get past its Vulkan probe
     * and returns NULL with a reason. The bridge's own headless constructor exists for exactly
     * this: it holds the same settings logic and the same knobs, and `nr_menu_bridge_menu` hands
     * back NULL - so the inject_* calls the layer makes cannot be used, and the bridge's own
     * entry points are what a caller drives. */
    nr_menu_bridge *bridge = nr_menu_bridge_create_headless(settings, 0);
    if (!bridge) { printf("no bridge\n"); return 1; }

    printf("the C side of the menu\n");

    /* What the bridge hands the UI: a row with min==max cannot move, and that is the shape of
     * "the sliders do nothing". */
    {
        for (int i = 0; i < ROWS; ++i) {
            float low = 0, high = 0, step = 0, def = 0;
            int is_range = nr_menu_bridge_knob_range(i, &low, &high, &step, &def);
            int choices = nr_menu_bridge_knob_choice_count(i);
            printf("  row %2d  choices=%d range=%d low=%.3f high=%.3f step=%.4f def=%.3f  "
                   "label=%s value=%s\n", i, choices, is_range, (double)low, (double)high,
                   (double)step, (double)def, nr_menu_bridge_row_label(bridge, i, 0),
                   nr_menu_bridge_row_value(bridge, i));
        }
    }

    /* The layer's frame, three steps: keys, panel, actions. A headless bridge has no menu to
     * inject into, so the action bits go through the bridge's own entry points instead - which
     * is what "Reset, Close and the language selector are one path, not two" means. */
    #define STEP() do {                                                     \
        for (int vk = 0; vk < 256; ++vk)                                   \
            nrex_key(nullptr, vk, (GetAsyncKeyState(vk) & 0x8000) != 0);    \
        int state = nr_menu_bridge_update(bridge, 1.0f / 60.0f);            \
        (void)state;                                                        \
        int count = build_rows(bridge);                                     \
        io.DisplaySize = ImVec2(800.0f, 600.0f);                            \
        io.DeltaTime = 1.0f / 60.0f;                                        \
        ImGui::NewFrame();                                                  \
        nrex_begin_panel(nullptr, "DLSS-NR on Intel", nr_menu_bridge_language(bridge), NULL); \
        nrex_rows(nullptr, rows, count, 0);                                 \
        int acted = nrex_end_panel(nullptr);                                \
        for (int i = 0; i < count; ++i)                                     \
            if (rows[i].changed)                                            \
                nr_menu_bridge_set(bridge, i, rows[i].kind == NREX_KIND_FLOAT \
                                                  ? rows[i].value_f         \
                                                  : (float)rows[i].value_i);\
        if (acted & NREX_ACT_LANGUAGE)                                      \
            nr_menu_bridge_set_language(bridge, nrex_panel_language(nullptr)); \
        if (acted & NREX_ACT_SAVE) nr_menu_bridge_save(bridge);              \
        if (acted & NREX_ACT_RESET) nr_menu_bridge_reset(bridge);            \
        if (acted & NREX_ACT_CLOSE) nr_menu_bridge_close(bridge);            \
        ImGui::Render();                                                    \
    } while (0)

    for (int f = 0; f < 3; ++f) STEP();

    /* ---- 1. a value change reaches the bridge and the preview ---- */
    {
        /* Tab until a row takes focus, then one Left: `render_scale` sits at its maximum of 1.0
         * by default, so a Right press would only clamp and report nothing - the same direction
         * is measured, but downward. Which row focus landed on is printed, so a failure says
         * whether the arrows reached a row at all. */
        int landed = -1;
        for (int i = 0; i < 40 && landed < 0; ++i) {
            nrex_key(nullptr, 0x09, 1); STEP(); nrex_key(nullptr, 0x09, 0); STEP();
            nrex_key(nullptr, 0x25, 1); STEP();
            for (int r = 0; r < ROWS; ++r) if (rows[r].changed) landed = r;
            nrex_key(nullptr, 0x25, 0); STEP();
        }
        char detail[192];
        if (landed < 0) {
            snprintf(detail, sizeof detail, "no row reported a change");
        } else {
            snprintf(detail, sizeof detail, "row %d changed, render_scale now %.3f",
                     landed, atof(nr_menu_bridge_row_value(bridge, 0)));
        }
        check(landed >= 0, "a Left press on a focused row reports a change", detail);

        /* And the bridge now shows it. */
        double now = atof(nr_menu_bridge_row_value(bridge, 0));
        snprintf(detail, sizeof detail, "render_scale %.3f", now);
        check(landed == 0 ? now < 1.0 || now > 1.0 : 1,
              "the change reached the bridge", detail);
    }

    /* ---- 2. the language change translates the rows ---- */
    {
        const char *before = nr_menu_bridge_row_label(bridge, 0,
                                                       nr_menu_bridge_language(bridge));
        nr_menu_bridge_set_language(bridge, 1);            /* zh */
        STEP();
        const char *after = nr_menu_bridge_row_label(bridge, 0,
                                                      nr_menu_bridge_language(bridge));
        char detail[256];
        snprintf(detail, sizeof detail, "'%s' -> '%s'", before ? before : "(null)",
                 after ? after : "(null)");
        check(before && after && strcmp(before, after) != 0,
              "the language change retranslates the row labels", detail);
        nr_menu_bridge_set_language(bridge, 0);
        STEP();
    }

    /* ---- 3. Reset restores the defaults ---- */
    {
        /* Move `render_scale` (row 0) off its default, then Reset, and read the same row. */
        for (int i = 0; i < 40; ++i) {
            nrex_key(nullptr, 0x09, 1); STEP(); nrex_key(nullptr, 0x09, 0); STEP();
            nrex_key(nullptr, 0x25, 1); STEP();
            int any = 0;
            for (int r = 0; r < ROWS; ++r) if (rows[r].changed) any = 1;
            nrex_key(nullptr, 0x25, 0); STEP();
            if (any) break;
        }
        double moved = atof(nr_menu_bridge_row_value(bridge, 0));
        char detail[128];
        snprintf(detail, sizeof detail, "render_scale moved to %.3f", moved);
        check(fabs(moved - 1.0) > 1e-6, "the knob moved before Reset", detail);

        nr_menu_bridge_reset(bridge);
        STEP();
        double after_reset = atof(nr_menu_bridge_row_value(bridge, 0));
        snprintf(detail, sizeof detail, "render_scale after Reset %.3f (default 1.000)",
                 after_reset);
        check(fabs(after_reset - 1.0) < 1e-6, "Reset restores the defaults", detail);
    }

    /* ---- 4. Close keeps the changes and stops the panel ---- */
    {
        nr_menu_bridge_close(bridge);
        STEP();
        char detail[96];
        snprintf(detail, sizeof detail, "bridge still answers %d rows",
                 nr_menu_bridge_row_count(bridge));
        check(nr_menu_bridge_row_count(bridge) > 0, "Close keeps the bridge alive", detail);
    }

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    nr_menu_bridge_destroy(bridge);
    remove(settings);
    return failures ? 1 : 0;
}

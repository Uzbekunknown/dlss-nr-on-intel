/* The ImGui-based panel, behind the plain-C interface in nr_menu_ui.h.
 *
 * The style follows OptiScaler's settings window - a single dark panel, a scrolled list of labelled
 * controls with the value on the right, a footer of action buttons - which is read off its
 * screenshots and its layout code, not copied from it: OptiScaler is GPL-3.0 and this file is new
 * work in an Apache-2.0 tree. What is borrowed is the arrangement and the ImGui idiom, which is what
 * ImGui's own demo shows anyway.
 *
 * This file is the panel and nothing that draws it: the context, the widgets, the keys and the
 * pointer. A renderer takes ImGui's draw data from there - nr_menu_ui_vk.cpp into an image of its
 * own that the Vulkan layer blends onto the swapchain image, nr_menu_ui_d3d12.cpp over a DXGI back
 * buffer for the DirectX 12 proxy. Until 2026-10-31 this file was the Vulkan one as well.
 *
 * Input: the C side polls GetAsyncKeyState - a layer has no window and no message pump - and feeds
 * this file through nrex_key/nrex_mouse. ImGui derives edges itself, so holding a key is holding it.
 */
#include "nr_menu_ui.h"
#include "nr_menu_ui_backend.h"
#include "nr_menu_strings.h"   /* nr_menu_text: the graphs' names, in the panel's language */
#include "nr_frame_rate.h"     /* nr_frame_graph_axis: the graphs' percentile axis */

#include "imgui.h"
/* For `SetKeyOwner` only.
 *
 * It is `[Internal]` in 1.93, but the public shortcut `SetItemKeyOwner` registers an owner only
 * while the item is hovered or active, and this needs the key claimed while the item is merely
 * FOCUSED - which is the case the arrows have to work in. The header is a stock part of the
 * vendored tree; nothing in `third_party/imgui` is patched, and `src/tools/check_imgui_pristine.py`
 * still passes. */
#include "imgui_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>    /* ceilf, for the graphs' axis */
#include <cstring>
#ifndef _WIN32
#include <time.h>    /* CLOCK_MONOTONIC, for nrex_now_seconds */
#endif

/* ------------------------------------------------------------------ diagnostics
 *
 * Two kinds of message, and they must not be confused.
 *
 * NREX_ERROR is a real failure the person running the game needs to know about: a backend that
 * would not start, an image that would not resize. It always prints.
 *
 * NREX_TRACE is development chatter - "backend functions loaded", "queue idle", the pointer's
 * position every time it moves. It used to print unconditionally, which meant a released layer wrote
 * a line to the game's stderr on every mouse move and dumped Vulkan function pointers and a queue
 * handle while starting. That is noise at best and a small information leak at worst, in somebody
 * else's process. It compiles to nothing unless NREX_TRACE_LOGGING is defined, which is how this
 * file was developed and is not how it ships.
 *
 * The one that mattered most is the pointer trace: it was per-move, unthrottled, and it is what
 * made the menu feel like it was doing work while the pointer was nowhere near it. */
#ifndef NREX_TRACE_LOGGING
#define NREX_TRACE(...) ((void)0)
#else
#define NREX_TRACE(...) fprintf(stderr, __VA_ARGS__)
#endif
#define NREX_ERROR(...) fprintf(stderr, __VA_ARGS__)
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <vector>

#ifdef _WIN32
/* The .def file names the exports; dllexport here would disagree with the header's plain
 * declarations and C2375 follows, so on Windows the macro is empty. */
#define NREX_API
#else
#define NREX_API __attribute__((visibility("default")))
#endif

/* ------------------------------------------------------------------ the context */

/* The ImGui context the panel draws with, whichever renderer draws it: the keyboard repeat, the
 * look, and the font with the glyphs the four languages need. It came out of the Vulkan creation
 * when the panel gained a Direct3D 12 renderer (nr_menu_ui_d3d12.cpp); the renderer's own setup
 * follows it in each. */
bool nrex_context_create(const char *font_bytes, size_t font_size, const int *codepoints,
                         int codepoint_count, const char **why)
{
    setvbuf(stderr, NULL, _IONBF, 0);   /* a layer's stderr is redirected; block buffering ate
                                            every log after the last flush and made the freeze
                                            look like it happened somewhere it did not */
    NREX_TRACE("[ui] creating ImGui context\n");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    NREX_TRACE("[ui] context ok, loading font\n");
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;              /* a layer must not write an imgui.ini beside the game */

    /* Key repeat, for the keyboard-navigation half of the panel only.
     *
     * This decides how fast a held arrow walks FOCUS from row to row (and Tab through the items).
     * It does not touch value stepping inside a row: that is `nrex_repeat` below, with a delay and
     * an acceleration of its own, because one fixed rate cannot both leave a single click alone and
     * cross a knob's whole range in a second.
     *
     * Windows' defaults are 0.500 / 0.031. The delay is pulled in a little so a held key does not
     * read as dead, and the rate is left near enough to the default that a held Tab does not blur. */
    io.KeyRepeatDelay = 0.300f;
    io.KeyRepeatRate  = 0.045f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    /* OptiScaler's look, read off its window: dark, slightly blue, compact rows. */
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 4.0f;
    st.FrameRounding = 3.0f;
    st.GrabRounding = 3.0f;
    st.WindowPadding = ImVec2(10, 8);
    st.FramePadding = ImVec2(6, 4);
    st.ItemSpacing = ImVec2(8, 4);
    st.ScrollbarSize = 12.0f;
    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.06f, 0.07f, 0.09f, 0.97f);
    c[ImGuiCol_TitleBg]         = ImVec4(0.08f, 0.09f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.13f, 0.19f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.13f, 0.15f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.19f, 0.24f, 0.31f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.24f, 0.32f, 0.42f, 1.00f);
    c[ImGuiCol_SliderGrab]      = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);
    c[ImGuiCol_SliderGrabActive]= ImVec4(0.45f, 0.70f, 0.95f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.18f, 0.26f, 0.36f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.28f, 0.42f, 0.58f, 1.00f);
    c[ImGuiCol_CheckMark]       = ImVec4(0.45f, 0.75f, 0.95f, 1.00f);
    c[ImGuiCol_Text]            = ImVec4(0.86f, 0.89f, 0.93f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);

    /* The font. The C side hands over a TTF and the glyph ranges the four languages need; without
     * it the embedded proggy covers ASCII only, which is fine for English and useless for the
     * rest - the exact hole the hand-drawn menu fell into. */
    ImFontConfig fc;
    fc.OversampleH = 2;
    fc.OversampleV = 1;
    const ImWchar *ranges = nullptr;
    static ImVector<ImWchar> built_ranges;
    if (codepoints && codepoint_count > 0) {
        /* ImGui wants pairs of (first, last), terminated by 0. The caller hands over the exact
         * codepoints the menu's strings use, sorted; each run of consecutive values becomes one
         * pair. That is a few hundred glyphs instead of the twenty-one thousand a whole CJK block
         * asks for - and a block that large does not fit one atlas, which is what made every glyph
         * blank rather than only the Chinese ones. */
        built_ranges.clear();
        built_ranges.push_back(0x20);   /* always keep printable ASCII: labels, numbers, units */
        built_ranges.push_back(0x7E);
        int run_start = -1, previous = -2;
        for (int i = 0; i < codepoint_count; ++i) {
            int cp = codepoints[i];
            if (cp < 0x20 || cp == 0x7F) continue;
            if (cp == previous) continue;
            if (cp == previous + 1) { previous = cp; continue; }
            if (run_start >= 0) {
                built_ranges.push_back((ImWchar)run_start);
                built_ranges.push_back((ImWchar)previous);
            }
            run_start = cp;
            previous = cp;
        }
        if (run_start >= 0) {
            built_ranges.push_back((ImWchar)run_start);
            built_ranges.push_back((ImWchar)previous);
        }
        built_ranges.push_back(0);
        ranges = built_ranges.Data;
    }
    if (font_bytes && font_size)
        io.Fonts->AddFontFromMemoryTTF((void *)font_bytes, (int)font_size, 16.0f, &fc, ranges);
    else
        io.Fonts->AddFontDefault();

    (void)why;
    return true;
}

/* ------------------------------------------------------------------ frames */

/* Defined with the cursor, below, and called here - which is the only place it can be called from:
 * `NewFrame` is what turns the queued position events into `io.MousePos`, so the fallback has to go
 * in after it and before anything is drawn. */
static void nrex_place_software_cursor(nrex_ui *ui);

extern "C" NREX_API void nrex_new_frame(nrex_ui *ui, float w, float h, float dt)
{
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(w, h);
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
    /* The renderer's own step for the frame, if it has one: Vulkan's backend has, the D3D12
     * renderer has none (it takes ImGui's texture requests when it draws). */
    if (ui && ui->backend_new_frame) ui->backend_new_frame(ui);
    ImGui::NewFrame();
    nrex_place_software_cursor(ui);
    if (ui) ui->in_frame = true;
}

/* The rows the panel is drawing this frame, so a widget can keep its own value while it is being
 * edited. Sized to the most rows any menu has. */
#define NREX_MAX_ROWS 24

/* The panel's language, as the selector left it. Read back by the caller, which asks the menu for
 * the change - the same flag the L key sets. */
static int g_language = 0;

/* The panel-level line the caller hands in each frame: the panel's keys. Empty draws nothing. 256 is the longest of the four languages' hint plus
 * room - the Chinese is the longest and it wraps in the footer rather than being cut. */
static char g_panel_hint[256] = "";

static nrex_ui *g_ui = nullptr;
static int g_actions = 0;
/* The effect's state, as the layer reported it for this frame, and what the user asked for by
 * clicking the checkbox. -1 in g_nr_enabled means "this install has no trigger path", which is
 * drawn differently from "off": one is a state, the other is the absence of one. */
static int g_nr_enabled = 0;
static int g_nr_request = -1;
/* The caller's row array for this frame. The widgets write their edits into it directly, which is
 * what lets the caller read them back after `nrex_end_panel` - a local copy would lose them, and
 * that lost copy is why a dragged slider changed nothing. */
static nrex_row *g_rows = nullptr;

/* **The two graphs, after OptiScaler's.** The counter's numbers say what the last second averaged;
 * these say whether it was steady, which is the difference a player actually feels.
 *
 *   frame  green - milliseconds between presents, so frame generation's extra frames are the
 *                  short ones and a stall is a spike;
 *   NR     blue  - the network's milliseconds, the cost NR adds to those frames.
 *
 * Both, because a player deciding whether NR is the cause needs the pair: a high frame time with a
 * low NR time is the game, and both high is the network.
 *
 * Copied from the caller's array rather than pointed at: the counter is drawn once a frame from a
 * frame's stack, and the samples live in the panel's struct, which outlives this call. The C side
 * fills a ring once a present (`nr_frame_rate.h`'s `nr_frame_graph`) and hands the run here; this
 * file only draws it, which is what keeps the arithmetic testable without ImGui. */
static float g_graph_frame[NREX_GRAPH_SAMPLES];
static float g_graph_net[NREX_GRAPH_SAMPLES];
static int   g_graph_frame_count = 0;
static int   g_graph_net_count = 0;
/* Whether the CORNER draws them. The panel always does - see `nrex_counter`. */
static int   g_corner_graphs = 1;

/* **The knob table's columns, which the graphs' row has to line up with.** One definition here
 * and one use there: the knob rows and the graph rows are drawn by different code, and two copies
 * of "where the label column starts" is exactly how they drifted apart - the graph row drew its
 * text at 340 px while the knob labels sat at 292, and the eye saw two columns pretending to be
 * one. A knob row is slider + spacing + field + spacing, then the label; a graph row is plot +
 * spacing, then its name and band. Both share the same text-column start, so the plot's width is
 * DERIVED: column start, minus the gap the graph row puts between plot and text. */
static const ImU32 kGraphFrameColour = IM_COL32(120, 220, 120, 255);
static const ImU32 kGraphNetColour   = IM_COL32(120, 170, 255, 255);
/* The corner's: a small box in the corner of a game, with its text under it rather than beside -
 * the corner has no rows to line up with, and every pixel it takes is a pixel of the picture. */
static const float kGraphWidth = 180.0f;
static const float kGraphHeight = 34.0f;
/* The panel's columns. `kKnobTextColumn` is where both the knob labels and the graphs' names sit;
 * the plot is whatever is left of it after the 10 px gap. 276 is the combo's own width
 * (190 + spacing + 86), and one more spacing lands the label. */
static const float kKnobSlider = 190.0f;
static const float kKnobField = 86.0f;
static const float kKnobSpacing = 8.0f;          /* ImGui's default ItemSpacing.x */
static const float kKnobTextColumn = kKnobSlider + kKnobSpacing + kKnobField + kKnobSpacing;
static const float kPanelGraphGap = 10.0f;
static const float kPanelGraphWidth = kKnobTextColumn - kPanelGraphGap;   /* 282 */
static const float kPanelGraphHeight = 30.0f;


/* **One graph, with its name and its axis in a column to the LEFT of the plot.**
 *
 * The axis used to be ImGui's overlay value, printed inside the plot against the top edge, where
 * it sat on top of the trace it was labelling - the one part of the picture that must stay
 * readable. It is a separate column now: the name on the first line (that is what tells the two
 * graphs apart, so it does not need a legend), the axis under it, and neither one over the data.
 *
 * `has_data` is false when every point is a gap - a frame the daemon answered without a network
 * time, which is every frame on the CPU path. The plot is not drawn at all then, rather than drawn
 * as a flat line along the bottom: a line at zero says "the network is free", and the truth is
 * "there is nothing to measure here". The column keeps its height so the box does not jump.
 *
 * A gap in the middle of a series is drawn as a gap, because this plot is drawn by hand (see
 * `draw_one_graph`): `PlotLines` would clamp -1 to the floor of its box and draw a line along it,
 * which reads as "the network answered in no time" - the one meaning this graph must never give. */
/* **One graph: the plot on the left, its name and its band on the right.**
 *
 * The order is deliberate and comes from what the eye does with a settings list. Every row below
 * these two has its control on the left and its LABEL on the right (`slider  value  Render scale`),
 * so a graph that put its label first would be the only row in the panel reading the other way -
 * which is exactly how the first version looked: a column of text, then a box, then a wider box.
 *
 * `width` is the plot; the text column is measured from the widest name and the widest band, so the
 * two rows line up with each other and the right edge lands where the rows below end.
 *
 * `id` is passed in rather than built from the name: two `PlotLines("")` in one window collide on
 * the empty ID, which is the "2 visible items with conflicting ID" the panel showed. */
static void draw_one_graph(const char *id, const char *name, const float *series, int count,
                           ImU32 colour, float width, int label_right, float height)
{
    char axis[32] = "";
    float lo = 0.0f, hi = 0.0f;
    int any = 0;
    for (int i = 0; i < count; ++i) if (series[i] >= 0.0f) { any = 1; break; }
    if (any) {
        nr_frame_graph_axis(series, count, &lo, &hi);
        nr_frame_graph_axis_text(series, count, axis, sizeof axis);
    } else {
        snprintf(axis, sizeof axis, "%s", "-");
    }
    /* The corner's text goes above its plot, so the box stays narrow; the panel's goes beside it,
     * so the row reads the way the knob rows below do. */
    if (!label_right) {
        ImGui::TextUnformatted(name);
        ImGui::TextDisabled("%s", axis);
    }
    if (any) {
        /* **The line is smoothed, the axis is not.** `lo`/`hi` came from the raw series above, so
         * a stall is still drawn at its full height; the smoothing only softens the hedge of
         * per-frame jitter into the trend the graph is for. A gap stays a gap through the filter,
         * which is how the network's missing samples keep their shape.
         *
         * **Drawn by hand, because `PlotLines` cannot leave a hole.** It normalises every point
         * through `ImSaturate`, so a gap - which `nr_frame_graph_smooth` hands back as -1 - is
         * clamped to the bottom of the plot and drawn as a line along it: the picture then claims
         * the network answered in zero milliseconds, which is the one reading this graph must
         * never give. `ImDrawList::AddLine` per run of real samples draws the holes as holes, and
         * costs about what `PlotLines` did - the same number of segments, without a scratch
         * buffer and without the value clamp that caused this.
         *
         * A single isolated sample has no neighbour to join, so it gets a dot: one frame the
         * network answered, drawn as one, rather than a point of a line that is not there. */
        float smooth[NR_GRAPH_SAMPLES];
        const int smoothed = nr_frame_graph_smooth(series, count, smooth, NR_GRAPH_SAMPLES);
        const float *plot = smoothed > 0 ? smooth : series;
        const int plot_count = smoothed > 0 ? smoothed : count;

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(width, height));
        ImDrawList *draw = ImGui::GetWindowDrawList();
        const float span = hi - lo;
        const float step_x = plot_count > 1 ? width / (float)(plot_count - 1) : 0.0f;
        const float px_per_value = span > 0.0f ? height / span : 0.0f;
        bool have_prev = false;
        ImVec2 prev(0.0f, 0.0f);
        for (int i = 0; i < plot_count; ++i) {
            if (plot[i] < 0.0f) { have_prev = false; continue; }
            float v = plot[i];
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            const ImVec2 at(origin.x + step_x * (float)i,
                            origin.y + height - (v - lo) * px_per_value);
            if (have_prev) {
                draw->AddLine(prev, at, colour, 1.0f);
            } else {
                draw->AddRectFilled(ImVec2(at.x - 1.0f, at.y - 1.0f),
                                    ImVec2(at.x + 1.0f, at.y + 1.0f), colour);
            }
            prev = at;
            have_prev = true;
        }
    } else {
        /* An empty box of the same size, with a line of text in it: the row keeps its place and
         * says why it is empty rather than pretending to be a graph. */
        ImVec2 at = ImGui::GetCursorScreenPos();
        char empty_id[64];
        snprintf(empty_id, sizeof empty_id, "e%s", id);
        ImGui::InvisibleButton(empty_id, ImVec2(width, height));
        ImGui::GetWindowDrawList()->AddRect(at, ImVec2(at.x + width, at.y + height),
                                            IM_COL32(90, 90, 96, 160));
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const char *why = "no network time";
        ImVec2 sz = ImGui::CalcTextSize(why);
        dl->AddText(ImVec2(at.x + (width - sz.x) * 0.5f, at.y + (height - sz.y) * 0.5f),
                    IM_COL32(140, 140, 150, 200), why);
    }
    if (label_right) {
        /* To the RIGHT of the plot, centred on its height - the same order the knob rows use, and
         * the reason the panel no longer reads as two blocks of text with a box between them. */
        ImGui::SameLine(0.0f, kPanelGraphGap);
        ImGui::BeginGroup();
        const float line = ImGui::GetTextLineHeight();
        const float pad = (height - line * 2.0f) * 0.5f;
        if (pad > 0.0f) ImGui::Dummy(ImVec2(0.0f, pad));
        ImGui::TextUnformatted(name);
        ImGui::TextDisabled("%s", axis);
        ImGui::EndGroup();
    }
}

/* **The two graphs, drawn once.** The panel's header and the corner both call this, so the axis,
 * the colours and the gap handling cannot drift between them.
 *
 * `frame` and `net` are the series and their lengths, oldest first. A zero length draws nothing,
 * which is what a host with no counts passes. `width` is the plot's own width.
 *
 * `label_right` says where the text goes, and the two callers want opposite answers:
 *   - the PANEL's header has the knob table under it (slider, value, label, 486 wide), so its
 *     graphs put the text on the RIGHT like every row below - one reading order down the panel;
 *   - the CORNER is a small box in the corner of a game with no rows under it, so the text goes
 *     ABOVE the plot, which keeps the box narrow instead of making it twice as wide. */
static void draw_graphs(const float *frame, int frame_count, const float *net, int net_count,
                        float width, int label_right, float height)
{
    if (frame_count <= 0) return;
    const char *frame_name = nr_menu_text(g_language, NR_STR_GRAPH_FRAME);
    const char *net_name = nr_menu_text(g_language, NR_STR_GRAPH_NET);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));
    /* The ids carry the caller: the panel's header and the corner both draw, and two `PlotLines`
     * with the same id in one window is the "2 visible items with conflicting ID" the panel showed
     * while both were open. */
    const char *frame_id = label_right ? "##nr_frame_ms_panel" : "##nr_frame_ms_corner";
    const char *net_id = label_right ? "##nr_net_ms_panel" : "##nr_net_ms_corner";
    draw_one_graph(frame_id, frame_name, frame, frame_count, kGraphFrameColour, width,
                   label_right, height);
    if (net_count > 0)
        draw_one_graph(net_id, net_name, net, net_count, kGraphNetColour, width,
                       label_right, height);
    ImGui::PopStyleVar();
}

/* The frame counter's line for the top of the panel, and what it means, as `nrex_counter` was last
 * handed them. Copied, like the footer's hint. 1024 for the hint: its Russian is over 500 bytes. */
static char g_counter_line[192] = "";
static char g_counter_hint[1024] = "";
/* The knob's explanation, shown while the pointer rests on the row just submitted.
 *
 * Call it immediately after the item the tip belongs to: `IsItemHovered` asks about the last item,
 * which is why this takes no argument and reads the hint separately - the item is implicit.
 *
 * OptiScaler shows a tooltip on hover and it is the difference between a settings list you can use
 * and a list of numbers with names beside them; nobody should have to leave the game to find out
 * what "hold unchanged pixels" does. The text comes from the same generated table the installer's
 * tooltips use, so the two explain a knob with the same words in the same language.
 *
 * A NULL or empty hint draws nothing, which is what the display-only rows want - they are their own
 * explanation.
 *
 * The wrap position is an ABSOLUTE local X, which is how ImGui's own demo writes it
 * (`PushTextWrapPos(GetFontSize() * 35.0f)`). Two earlier attempts in this file were both wrong and
 * both measured: `0.0f` is the very start of the line, so every glyph wrapped onto its own line; and
 * `GetCursorPos().x + 420.0f` inside a freshly-begun tooltip is the wrap point plus 420, past which
 * nothing is drawn - the tip came out 16x12 pixels, an empty window with a visible frame. 320 pixels
 * is about 40 characters at this font size, which wraps the longest hint over four lines. */
static void nrex_hint_tooltip(const char *hint)
{
    if (!hint || !hint[0]) return;
    if (!ImGui::IsItemHovered()) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(320.0f);
    ImGui::TextUnformatted(hint);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
}

/* ------------------------------------------------- hold-to-repeat for the arrow keys
 *
 * How a held Left/Right steps a value, and why it is not ImGui's repeat.
 *
 * ImGui has a typematic repeat of its own, and 1.93 exposes presets for it
 * (`ImGuiInputFlags_RepeatRate*`), but every one of them is a FIXED rate behind one global delay.
 * A settings slider is exactly the shape that breaks, and it breaks in both directions at once:
 *
 *   - the stock 500 ms delay means the first repeat of a hold arrives late enough that the key reads
 *     as "not working at all" (the report: press and hold, nothing happens, give up);
 *   - winding that delay down globally makes a click land on the repeat, so one tap steps two or
 *     three values instead of one, and the number jumps about instead of walking.
 *
 * So the repeat is computed here, as two rules rather than one number:
 *
 *   click   the press steps exactly once, immediately - no timer involved;
 *   hold    nothing further for `NREX_REPEAT_WARMUP`, then an interval that starts at
 *           `NREX_REPEAT_FIRST` and shrinks by `NREX_REPEAT_ACCEL` on every repeat down to
 *           `NREX_REPEAT_MIN`.
 *
 * A 175 ms press - longer than most clicks - is therefore still one step and only one, while a
 * third of a second of holding is already walking at a useful rate. The ceiling is what matters:
 * the first version of this reached ~75 steps/s, which is a knob that crosses its whole range in a
 * quarter second and cannot be stopped on a number - the report that sent this comment back. It
 * now tops out at 20 steps/s, so a 0.05..1.0 range (19 steps) takes about a second to cross while
 * still being quicker than tapping.
 *
 * Timed on a WALL CLOCK, not `ImGui::GetTime()`. ImGui's clock runs at whatever step the layer
 * feeds it - a fixed 1/60 once, now the real seconds between presents - so at 30 FPS every
 * interval would double here and at 144 FPS nearly halve. A hold is felt in real seconds, so it
 * is measured in them. */
#define NREX_REPEAT_WARMUP 0.140f
#define NREX_REPEAT_FIRST  0.075f
#define NREX_REPEAT_ACCEL  0.850f
#define NREX_REPEAT_MIN    0.050f      /* 20 steps/s at the ceiling */
#define NREX_REPEAT_BURST  2           /* steps one frame may apply, so a hitch cannot fling the value */

struct nrex_repeat {
    int    dir;      /* -1 = left, +1 = right, 0 = idle */
    float  interval; /* the gap the NEXT repeat is due after */
    double next;     /* when that repeat is due, in seconds */
};

static nrex_repeat s_repeat[NREX_MAX_ROWS];

/* When the panel thinks "now" is. Windows: the performance counter, which is what the value-repeat
 * arithmetic wants (a monotonic clock with sub-microsecond resolution, and one the OS keeps across
 * suspend). Off Windows: `CLOCK_MONOTONIC`, which is the same promise on POSIX.
 *
 * **Why this is here and not in the C side.** The repeat logic owns its own clock so that a frame
 * that takes 100 ms and one that takes 10 ms both repeat at the right rate - the caller's frame
 * time is an input to the panel, not a clock. One function, one place, no second definition of
 * "now" to drift. */
static double nrex_now_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    static int have_frequency;
    if (!have_frequency) { QueryPerformanceFrequency(&frequency); have_frequency = 1; }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* The panel: one window, a scrolled body of rows, a footer. OptiScaler's arrangement. */
static void nrex_draw_panel(nrex_ui *u, const char *title, int language,
                            const nrex_row *rows, int count, int selected)
{
    (void)u;
    /* Focus on the first frame, and keep it.
     *
     * ImGui's keyboard navigation only moves inside the FOCUSED window, and a layer has no message
     * pump to focus one the way a platform backend does. Without this the arrows reach `io` and are
     * dropped for want of a focused window - which is precisely "the arrow keys do nothing" while
     * the key trace shows them arriving. The panel is the only window the layer has, so it takes
     * focus whenever focus is free rather than only on the first frame.
     *
     * No `AlwaysAutoResize`: it overrides the requested size, so the window shrinks to its content
     * and ImGui's hit rectangles no longer match where the panel is drawn. A fixed size keeps the
     * mouse test and the picture in agreement.
     *
     * `NoNavInputs` is deliberately NOT set - the whole point is that the keyboard drives it. */
    /* The panel is sized against the PICTURE, not against a fixed number of pixels.
     *
     * It was a fixed 560x380, which was fine for eleven knobs and is not fine for fourteen: the
     * last three could not be reached, and the only way to see them was to drag the window
     * bigger. Doing that is what crashed the game - the window could be dragged past the edges of
     * the swapchain image it has to be drawn into, and ImGui happily drew outside it.
     *
     * So the default size is now "as much of the picture as the panel wants", and the size is
     * CLAMPED to the picture every frame. The clamp is the part that matters: a resize survives
     * across frames and across game window sizes, so a size that was legal once can be illegal
     * later - a user who drags the panel wide in a large window and then starts the game smaller
     * would otherwise be drawing off the image again. */
    {
        ImGuiIO &io = ImGui::GetIO();
        float want_w = io.DisplaySize.x - 48.0f;
        float want_h = io.DisplaySize.y - 48.0f;
        /* A ceiling as well as a floor: on a 1920x1080 picture "as tall as the picture" would be
         * a settings panel covering the game, and the point of an overlay is to leave something
         * to look at. 640x720 shows all fourteen rows with room to spare. */
        if (want_w > 640.0f) want_w = 640.0f;
        if (want_h > 720.0f) want_h = 720.0f;
        if (want_h < 240.0f) want_h = 240.0f;
        if (want_w < 200.0f) want_w = 200.0f;
        ImGui::SetNextWindowSize(ImVec2(want_w, want_h), ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
        ImGui::SetNextWindowFocus();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    /* Movable, and that is deliberate.
     *
     * I pinned this with `NoMove` after a report that dragging moved the panel, and that was the
     * wrong fix: the panel is the only thing on screen the user can arrange, and taking the ability
     * away to avoid a stray press threw out the feature to dodge the bug. It has been put back.
     *
     * What made me think it was unsafe was an argument that does not hold: that a moved panel would
     * disagree with the hit test. It cannot - ImGui positions the window in the same UI-image pixels
     * the pointer arrives in, so a window at (400, 200) and a pointer at (450, 220) are in one
     * coordinate space, and ImGui does the hit test itself. `SetNextWindowPos` is `FirstUseEver`, so
     * wherever the user leaves it is where it stays.
     *
     * The stray press is real but it is not a reason to forbid moving: the panel only receives a
     * click while it is open, and while it is open a press on its own title bar is a press on the
     * panel. A press anywhere else belongs to the game and never reaches ImGui, because
     * `nrex_mouse` reports "no position" outside the UI image. */
    /* `NoResize`: the panel is sized to the picture, and a user-resized window that outgrows the
     * swapchain image makes ImGui draw outside the render target - which is an access violation in
     * the driver, not a warning. Dragging the border to see the last three rows is what crashed
     * the game.
     *
     * In exchange the panel is made tall enough to show everything without resizing (see the
     * default size above), and it keeps a scrollbar for the case where it is not.
     *
     * A per-frame clamp was tried first and is wrong: it has to run BEFORE `Begin` to have any
     * effect, and reading the window's current size there means `FindWindowByName` on a window
     * that has not been created yet on the first frame. It crashed the game on every INS press. */
    if (!ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize))
        { ImGui::End(); ImGui::PopStyleVar(); return; }

    /* Home belongs to the effect toggle, not to ImGui's navigation.
     *
     * ImGui's keyboard nav treats Home as "jump to the first item", so pressing it with the panel
     * open moved the selection to the first slider and never reached the layer - the report was
     * "with the panel open, Home selects the first slider; with it closed, Home does nothing".
     * Claiming it for the window stops nav acting on it; the layer reads the physical key with
     * `GetAsyncKeyState` and does not care who ImGui thinks owns it. */
    ImGui::SetKeyOwner(ImGuiKey_Home, ImGui::GetID(title));

    /* Home belongs to the effect toggle, not to ImGui's navigation.
     *
     * ImGui's keyboard nav treats Home as "jump to the first item", so pressing it with the panel
     * open moved the selection to the first slider and never reached the layer - the report was
     * "with the panel open, Home selects the first slider; with it closed, Home does nothing".
     * Claiming it for the window stops nav acting on it; the layer reads the physical key with
     * `GetAsyncKeyState` and does not care who ImGui thinks owns it.
     *
     * `ImGuiKey_Home`, not the End key: the toggle is Home and the comparison it replaced was
     * Home, so there is no second key to keep clear here. */

    /* The frame counter's line, above the rows: the screen's rate, NR's and the network's time,
     * with what the two rates are on hover. Text, not an item - keyboard navigation starts at the
     * first row as it always has. */
    if (g_counter_line[0]) {
        ImGui::TextUnformatted(g_counter_line);
        nrex_hint_tooltip(g_counter_hint);
        /* **The panel's graphs, always.** The corner's are the user's switch (`nrex_counter`'s
         * `corner_graphs`); these are not - a player who opened the panel is looking at the
         * numbers, and the shape behind them is why the panel is open. Drawn here rather than
         * under the corner's copy so the two are independent: turning the corner's off must not
         * take the panel's with it.
         *
         * The same axis rule as the corner's, and the same helper. */
        draw_graphs(g_graph_frame, g_graph_frame_count, g_graph_net, g_graph_net_count,
                    kPanelGraphWidth, 1, kPanelGraphHeight);
        ImGui::Separator();
    }

    /* Three fixed columns, so the controls line up as a table rather than drifting with each
     * label's length.
     *
     * Before this, every row placed its widgets back to back and the window was narrower than
     * slider + field + label wanted, so the label was clipped and the controls sat on top of one
     * another. A column width also means the eye can run down the values, which is the whole point
     * of a settings list. */
    const float kSliderWidth = 190.0f;
    const float kFieldWidth  = 86.0f;
    const float kLabelColumn = 210.0f;

    for (int i = 0; i < count; ++i) {
        const nrex_row &r = rows[i];
        ImGui::PushID(i);
        /* A disabled row is drawn greyed and refuses input as a whole: every widget below is
         * submitted under BeginDisabled, which is what makes "cannot be changed right now" mean
         * the same thing for a slider, a combo and a checkbox. */
        if (r.disabled) ImGui::BeginDisabled();
        switch (r.kind) {
        case NREX_KIND_FLOAT: {
            /* While the row is being edited the widget keeps its own number.
             *
             * The C side's string is stale until the change has been written to the settings file
             * and read back, so re-reading it every frame rebuilt the slider from the PRE-edit value
             * - which is what made Enter open a slider and then lose it, and why the arrows never
             * moved anything. `IsItemActive` is also exactly the condition under which ImGui lets
             * the arrows move a slider. */
            static float held[NREX_MAX_ROWS];
            static int   holding[NREX_MAX_ROWS];
            if (!holding[i]) held[i] = (float)atof(r.value ? r.value : "0");
            float v = held[i];

            /* No value text on the slider: the field beside it already shows the number, and two
             * copies of it read as a rendering fault rather than as a control. */
            ImGui::SetNextItemWidth(kSliderWidth);
            if (ImGui::SliderFloat("##v", &v, r.min, r.max, "")) {
                g_actions |= NREX_ACT_CHANGED;
                if (g_rows) { g_rows[i].changed = 1; g_rows[i].value_f = v; }
            }
            holding[i] = ImGui::IsItemActive() ? 1 : 0;

            /* Arrows step the value while this row has keyboard nav focus - no Enter needed.
             *
             * The two ways ImGui lets an arrow move a slider, and why BOTH are needed here:
             *
             *   ACTIVE   `SliderBehavior` claims Left/Right via `IsActiveIdUsingNavDir` while the
             *            item is active (`imgui_widgets.cpp`, `make_active = clicked ||
             *            g.NavActivateId == id`), so nav is suppressed and ImGui's own code steps the
             *            value. Enter activates, which is why Enter-then-arrow works.
             *
             *   FOCUSED  An item that is merely focused - what Tab gives - has claimed nothing, so
             *            `UpdateNavInput` (imgui.cpp) queues a directional move on the same keypress
             *            and nav wins. Left did nothing (the slider is the leftmost item in the row)
             *            while Right moved focus into the number field: reported exactly as "left
             *            works, right selects the number box".
             *
             * The second case is why this handler exists: a settings list where you Tab to a row and
             * then have to press Enter before the arrows do anything reads as broken.
             *
             * `IsKeyPressed(key, false)` is the EDGE only, and the hold is timed by `nrex_repeat`
             * above. `repeat = true` would add ImGui's own timer on top of this one - two
             * independent repeats on one key, which is what makes a value tick back and forth
             * instead of walking - and that is the bug this replaced. */
            nrex_repeat &rep = s_repeat[i];
            float dir = 0.0f;
            int steps = 0;
            if (ImGui::IsItemFocused()) {
                /* Both arrows belong to this row while it has focus, which is the other half of
                 * keeping a hold inside the row: ImGui's nav asks for the directional keys with
                 * `ImGuiKeyOwner_NoOwner` (`NavUpdateCreateMoveRequest`), so an owner is exactly
                 * what stops it re-targeting focus halfway through a value. Claimed every frame -
                 * ownership is dropped when the key is released - and harmless to ImGui's own
                 * slider tweak, which reads the keys through `GetKeyPressedAmount` and does not
                 * look at ownership at all. */
                const ImGuiID row_id = ImGui::GetItemID();
                ImGui::SetKeyOwner(ImGuiKey_LeftArrow,  row_id);
                ImGui::SetKeyOwner(ImGuiKey_RightArrow, row_id);

                const bool left_press  = ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false);
                const bool right_press = ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
                const bool left_down   = ImGui::IsKeyDown(ImGuiKey_LeftArrow);
                const bool right_down  = ImGui::IsKeyDown(ImGuiKey_RightArrow);

                if (left_press || right_press) {
                    /* the click: one step, and the hold is armed from here */
                    dir = right_press ? 1.0f : -1.0f;
                    steps = 1;
                    rep.dir = right_press ? 1 : -1;
                    rep.interval = NREX_REPEAT_FIRST;
                    rep.next = nrex_now_seconds() + NREX_REPEAT_WARMUP + NREX_REPEAT_FIRST;
                } else if ((rep.dir < 0 && left_down) || (rep.dir > 0 && right_down)) {
                    /* the hold: catch up on however many repeats are due */
                    dir = (float)rep.dir;
                    const double now = nrex_now_seconds();
                    while (steps < NREX_REPEAT_BURST && now >= rep.next) {
                        steps++;
                        rep.interval *= NREX_REPEAT_ACCEL;
                        if (rep.interval < NREX_REPEAT_MIN) rep.interval = NREX_REPEAT_MIN;
                        rep.next += rep.interval;
                    }
                    /* A stall - alt-tab, a load hitch - must not leave the timer seconds behind and
                     * then fire a burst at full speed: rebase instead. */
                    if (steps >= NREX_REPEAT_BURST && now > rep.next)
                        rep.next = now + rep.interval;
                } else {
                    rep.dir = 0;
                }
            } else {
                rep.dir = 0;
            }
            if (steps > 0) {
                float step = (r.step > 0.0f) ? r.step : 0.01f;
                float moved = held[i] + dir * step * (float)steps;
                if (moved < r.min) moved = r.min;
                if (moved > r.max) moved = r.max;
                if (moved != held[i]) {
                    held[i] = moved;
                    holding[i] = 0;
                    g_actions |= NREX_ACT_CHANGED;
                    if (g_rows) { g_rows[i].changed = 1; g_rows[i].value_f = moved; }
                }
            }

            /* A field beside it, because a slider has no field to type into - ImGui hides one
             * behind Ctrl+Click and nothing says so. Commits on Enter, through the same write-back
             * as the slider: one path, two ways in.
             *
             * `ImGuiItemFlags_NoNav` keeps it out of keyboard focus entirely - Tab AND the arrows.
             * The number it shows is the one the slider already shows, so stepping focus through
             * both is stepping through the same value twice; and it is what made the Right arrow
             * `ImGuiItemFlags_NoNav` keeps the field out of keyboard focus entirely - Tab AND the
             * arrows - so Enter on a row selects the SLIDER. That is the user's rule: "Enter should
             * select the progress bar, not the number box, because it duplicates the value; only the
             * slider is adjustable".
             *
             * It also removes the field as a directional target, which is the other half of why the
             * Right arrow used to land here instead of changing the value: nav moves between
             * focusable items, and this was the only one to the right of the slider.
             *
             * The flag rather than `PushTabStop(false)`: that one removes the item from Tab only,
             * and the arrows are the half that was actually broken. A mouse still reaches the field,
             * which is the only way in that needs it - `IsItemActive` above is unaffected. */
            ImGui::SameLine();
            ImGui::SetNextItemWidth(kFieldWidth);
            float typed = v;
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            bool committed = ImGui::InputFloat("##t", &typed, 0.0f, 0.0f, "%.3f",
                                               ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopItemFlag();
            if (committed) {
                if (typed < r.min) typed = r.min;
                if (typed > r.max) typed = r.max;
                held[i] = typed;
                holding[i] = 0;
                g_actions |= NREX_ACT_CHANGED;
                if (g_rows) { g_rows[i].changed = 1; g_rows[i].value_f = typed; }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(r.label ? r.label : "");
            nrex_hint_tooltip(r.hint);
            break;
        }
        case NREX_KIND_CHOICE: {
            int idx = r.choice_index;
            ImGui::SetNextItemWidth(kSliderWidth + kFieldWidth +
                                    ImGui::GetStyle().ItemSpacing.x);
            bool picked = ImGui::Combo("##c", &idx, r.choices, r.choice_count);
            /* Left/Right step the choice, as the hint says they change a row. ImGui gives a combo
             * no arrow tweak - only Enter opens it - so Right on the Network row did nothing and
             * the hint was wrong there. The keys are claimed the way the slider rows above claim
             * them, so nav does not take Right as a move to another item. One step a press, no
             * repeat, and no wrap: the ends of a list stay ends. */
            if (!picked && ImGui::IsItemFocused()) {
                const ImGuiID row_id = ImGui::GetItemID();
                ImGui::SetKeyOwner(ImGuiKey_LeftArrow,  row_id);
                ImGui::SetKeyOwner(ImGuiKey_RightArrow, row_id);
                const int step = ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ? 1
                               : ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false) ? -1 : 0;
                if (step != 0 && idx + step >= 0 && idx + step < r.choice_count) {
                    idx += step;
                    picked = true;
                }
            }
            if (picked) {
                g_actions |= NREX_ACT_CHANGED;
                if (g_rows) { g_rows[i].changed = 1; g_rows[i].value_i = idx; }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(r.label ? r.label : "");
            nrex_hint_tooltip(r.hint);
            break;
        }
        case NREX_KIND_FLAG: {
            bool on = r.choice_index != 0;
            bool flipped = ImGui::Checkbox("##f", &on);
            /* **Left/Right flip it too, as they change every other row.** The hint says the arrows
             * change a row and Up/Down move between them; a flag answered only the mouse, so the
             * only keys that could reach it were Tab and Space, neither of which the hint
             * mentions. ImGui's checkbox has no arrow handling of its own, so this claims the keys
             * the way the combo row above does, and only while the row is focused. */
            if (!flipped && ImGui::IsItemFocused()) {
                const ImGuiID row_id = ImGui::GetItemID();
                ImGui::SetKeyOwner(ImGuiKey_LeftArrow,  row_id);
                ImGui::SetKeyOwner(ImGuiKey_RightArrow, row_id);
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false)) {
                    on = !on;
                    flipped = true;
                }
            }
            if (flipped) {
                g_actions |= NREX_ACT_CHANGED;
                if (g_rows) { g_rows[i].changed = 1; g_rows[i].value_i = on ? 1 : 0; }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(r.label ? r.label : "");
            nrex_hint_tooltip(r.hint);
            break;
        }
        default:
            ImGui::TextDisabled("%s  %s", r.label, r.value ? r.value : "");
        }
        (void)kLabelColumn;
        if (r.disabled) ImGui::EndDisabled();
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::Button("Save"))   g_actions |= NREX_ACT_SAVE;
    ImGui::SameLine();
    if (ImGui::Button("Reset"))  g_actions |= NREX_ACT_RESET;
    ImGui::SameLine();
    /* Revert: back to what is SAVED, where Reset goes to the code's defaults. Two different
     * questions - "I broke something, give me the standard values" and "I was experimenting, give
     * me back what I had" - and one button cannot answer both. */
    if (ImGui::Button("Revert")) g_actions |= NREX_ACT_REVERT;
    ImGui::SameLine();
    if (ImGui::Button("Close"))  g_actions |= NREX_ACT_CLOSE;

    /* The effect, on the footer where the other whole-picture controls are.
     *
     * It reads the state the layer passes in rather than keeping one: the Home key and the setup
     * window's button flip the same trigger file, and a checkbox with its own copy would show the
     * wrong thing as soon as either was used. A click only REPORTS - the layer decides, so the
     * three ways in cannot disagree. */
    {
        bool on = g_nr_enabled > 0;
        ImGui::SameLine();
        ImGui::TextUnformatted("NR");
        ImGui::SameLine();
        if (ImGui::Checkbox("##nr", &on)) {
            g_nr_request = on ? 1 : 0;
            g_actions |= NREX_ACT_NR_TOGGLE;
        }
        /* No state to show - the trigger path did not resolve. The box is drawn unticked and
         * disabled rather than pretending the effect is off. */
        if (g_nr_enabled < 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("this install has no trigger path; NR cannot be switched here");
        }
    }

    /* The language, as a control and not only a hotkey. The strings live on the C side, so this
     * only records the choice; the caller reads it back and asks the menu for the change through
     * the same flag the L key sets. */
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    static const char *lang_names[] = { "English", "中文", "Русский", "Español" };
    int chosen = g_language;
    ImGui::Combo("##lang", &chosen, lang_names, 4);
    if (chosen != g_language) {
        g_language = chosen;
        g_actions |= NREX_ACT_LANGUAGE;
    }

    /* The panel-level hint, on the footer under the controls.
     *
     * This is where the panel's keys are listed - and where, until the panel kept its keys from
     * the game (nr_input_guard.h, 2026-10-31), the sentence "the game also sees these keys" was.
     * It lived only in `NR_STR_HINT`, and only the hand-drawn menu was handed it - so the ImGui
     * panel, which is what every game actually loads, showed no key list and no warning at all. A
     * player reading the panel had no way to learn that walking its rows also walked the game's
     * menus, which is what the MK11 title-screen report was.
     *
     * `TextWrapped`, because the Chinese and Russian lines are long and a cut sentence is worse
     * than a wrapped one: the footer is the only place the panel has for prose. */
    if (g_panel_hint[0]) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", g_panel_hint);
        ImGui::PopStyleColor();
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

extern "C" NREX_API void nrex_begin_panel(nrex_ui *ui, const char *title, int language,
                                          const char *hint)
{    g_ui = ui;
    g_actions = 0;
    /* Cleared here, not in `nrex_rows`: the checkbox is drawn in the footer, after the rows, and a
     * request from the PREVIOUS frame must not be read twice. */
    g_nr_request = -1;
    (void)title;
    if (language >= 0 && language < 4) g_language = language;
    /* The footer line, kept for the draw below. Copied rather than remembered: the caller's string
     * is the generated table's, which lives for the process, but a pointer that outlives its owner
     * is the kind of thing that works until it does not. */
    snprintf(g_panel_hint, sizeof g_panel_hint, "%s", hint ? hint : "");
}

extern "C" NREX_API void nrex_set_nr_enabled(nrex_ui *ui, int on)
{
    (void)ui;
    g_nr_enabled = on < 0 ? -1 : (on ? 1 : 0);
}

/* The frame counter: the panel's line is kept for `nrex_draw_panel`, and the corner is drawn here
 * and now, as its own window.
 *
 * **The corner takes no input, and every flag below is part of that.** It is on screen while the
 * panel is shut, when the keyboard and the mouse are the game's: `NoInputs` keeps the pointer from
 * hovering or clicking it and keeps it out of keyboard navigation, `NoFocusOnAppearing` and
 * `NoBringToFrontOnFocus` keep it from ever taking the focus the panel claims when it opens, and
 * `NoDecoration` and `NoMove` leave nothing to grab. With the panel shut the C side also tells
 * ImGui there is no pointer at all and hands it no keys (`nrex_listen`), so nothing could reach the
 * corner if a flag were missing - the flags are the second door, not the first.
 *
 * Placed against its corner every frame with the pivot on that corner, so its size - which follows
 * its text - grows away from the edge. Drawn before the panel's window, which takes the focus and
 * so stays on top of it. */
extern "C" NREX_API void nrex_counter(nrex_ui *ui, const char *panel_line, const char *hint,
                                      int corner, const char *corner_line,
                                      const float *frame_ms, int frame_count,
                                      const float *net_ms, int net_count, int corner_graphs)
{
    (void)ui;
    snprintf(g_counter_line, sizeof g_counter_line, "%s", panel_line ? panel_line : "");
    snprintf(g_counter_hint, sizeof g_counter_hint, "%s", hint ? hint : "");
    /* Copied and clamped: the caller's arrays are a frame's scratch, and the ring is
     * NREX_GRAPH_SAMPLES deep whatever it hands over. */
    g_graph_frame_count = 0;
    g_graph_net_count = 0;
    if (frame_ms && frame_count > 0) {
        g_graph_frame_count = frame_count < NREX_GRAPH_SAMPLES ? frame_count : NREX_GRAPH_SAMPLES;
        for (int i = 0; i < g_graph_frame_count; ++i) g_graph_frame[i] = frame_ms[i];
    }
    if (net_ms && net_count > 0) {
        g_graph_net_count = net_count < NREX_GRAPH_SAMPLES ? net_count : NREX_GRAPH_SAMPLES;
        for (int i = 0; i < g_graph_net_count; ++i) g_graph_net[i] = net_ms[i];
    }
    /* The corner's graphs, which are the user's switch; the panel's are unconditional. */
    g_corner_graphs = corner_graphs ? 1 : 0;
    if (corner < 1 || corner > 4 || !corner_line || !corner_line[0]) return;

    ImGuiIO &io = ImGui::GetIO();
    const float margin = 8.0f;
    const bool right = corner == 2 || corner == 4;
    const bool bottom = corner == 3 || corner == 4;
    ImGui::SetNextWindowPos(ImVec2(right ? io.DisplaySize.x - margin : margin,
                                   bottom ? io.DisplaySize.y - margin : margin),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.60f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##nr_frame_counter", nullptr, flags)) {
        ImGui::TextUnformatted(corner_line);
        /* The graphs, under the numbers they are the shape of.
         *
         * **The panel's header always has them; the corner follows the user's switch.** A corner
         * window over a game's HUD is competing for the same pixels, so it is the one that can be
         * turned off - and when it is, the corner is the line of numbers it was before the graphs
         * existed.
         *
         * **Scaled to the data, with a floor and a cap.** A fixed axis makes a steady stream and a
         * stuttering one draw the same flat line at 70 fps - 14 ms against 100 ms is a sixth of the
         * height - which is the one thing the picture exists to tell apart. Each series gets
         * `ceil(max, floor)` so its own shape fills the box, and the label carries the number so
         * the reader knows what the height means. */
        if (g_corner_graphs) {
            draw_graphs(g_graph_frame, g_graph_frame_count, g_graph_net, g_graph_net_count,
                        kGraphWidth, 0, kGraphHeight);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

bool nrex_note_drawn(nrex_ui *ui, const ImDrawData *draw, unsigned width, unsigned height)
{
    if (!ui) return false;
    ui->drawn_any = false;
    ui->drawn[0] = ui->drawn[1] = ui->drawn[2] = ui->drawn[3] = 0;
    if (!draw || draw->TotalVtxCount <= 0 || !width || !height) return false;
    float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
    for (const ImDrawList *list : draw->CmdLists) {
        for (const ImDrawVert &v : list->VtxBuffer) {
            if (v.pos.x < x0) x0 = v.pos.x;
            if (v.pos.y < y0) y0 = v.pos.y;
            if (v.pos.x > x1) x1 = v.pos.x;
            if (v.pos.y > y1) y1 = v.pos.y;
        }
    }
    if (x1 < x0 || y1 < y0) return false;
    /* Into image pixels, outward, and two wider for the anti-aliased fringe ImGui draws past a
     * shape's edge; then inside the image. */
    x0 -= draw->DisplayPos.x + 2.0f;
    y0 -= draw->DisplayPos.y + 2.0f;
    x1 = x1 - draw->DisplayPos.x + 2.0f;
    y1 = y1 - draw->DisplayPos.y + 2.0f;
    int ix0 = x0 < 0.0f ? 0 : (int)x0;
    int iy0 = y0 < 0.0f ? 0 : (int)y0;
    int ix1 = x1 > (float)width ? (int)width : (int)x1 + 1;
    int iy1 = y1 > (float)height ? (int)height : (int)y1 + 1;
    if (ix1 > (int)width) ix1 = (int)width;
    if (iy1 > (int)height) iy1 = (int)height;
    if (ix1 <= ix0 || iy1 <= iy0) return false;
    ui->drawn[0] = ix0;
    ui->drawn[1] = iy0;
    ui->drawn[2] = ix1;
    ui->drawn[3] = iy1;
    ui->drawn_any = true;
    return true;
}

extern "C" NREX_API int nrex_drawn_rect(nrex_ui *ui, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    if (!ui || !ui->drawn_any) return 0;
    if (x) *x = (uint32_t)ui->drawn[0];
    if (y) *y = (uint32_t)ui->drawn[1];
    if (w) *w = (uint32_t)(ui->drawn[2] - ui->drawn[0]);
    if (h) *h = (uint32_t)(ui->drawn[3] - ui->drawn[1]);
    return 1;
}

extern "C" NREX_API int nrex_nr_toggle_request(nrex_ui *ui)
{
    (void)ui;
    return g_nr_request;
}

extern "C" NREX_API int nrex_panel_language(nrex_ui *ui)
{
    (void)ui;
    return g_language;
}

extern "C" NREX_API void nrex_rows(nrex_ui *ui, const nrex_row *rows, int count, int selected)
{
    /* `const` in the signature, written to in practice: the array belongs to the caller and the
     * edits have to reach it. The cast is confined to this line, which is the honest place for it -
     * a copy would leave the caller holding the pre-edit values. */
    g_rows = (nrex_row *)(uintptr_t)rows;
    nrex_draw_panel(ui, "DLSS-NR on Intel", 0, rows, count, selected);
}

extern "C" NREX_API int nrex_end_panel(nrex_ui *ui)
{
    (void)ui;
    return g_actions;
}

/* The hint the footer was last handed, for a test that wants to know it was drawn.
 *
 * The draw itself is the only real evidence, and this is deliberately NOT that: it answers "was the
 * string here", not "did the compositor emit vertices for it". The fixture pairs it with the
 * panel's actions being unchanged, which is the half that could regress silently - a hint that
 * swallowed a click, say. Returning 0 for an empty hint means "nothing to draw", which is what an
 * empty string means to the drawer too. */
extern "C" NREX_API int nrex_last_footer_text(char *out, size_t size)
{
    if (!out || size == 0) return 0;
    out[0] = '\0';
    if (!g_panel_hint[0]) return 0;
    int n = snprintf(out, size, "%s", g_panel_hint);
    return n > 0 ? 1 : 0;
}

/* The C side polls GetAsyncKeyState - a layer has no window and no message pump - and hands the
 * state here. This is the platform-backend role, and ImGui's contract for it is specific:
 *
 *   AddKeyEvent describes a CHANGE, not a level. Reporting a held key as down on every frame is
 *   reporting a press on every frame, which is what made Enter reopen and re-confirm the combo it
 *   had just opened. Only transitions are forwarded, so a held key is one press followed by
 *   ImGui's own repeat.
 *
 *   AddInputCharacter is what puts a glyph into a text field. Keys move focus and drive widgets;
 *   they do not type. Without characters, activating a slider showed a selected field that no
 *   number would go into.
 *
 * The virtual-key to ImGuiKey mapping covers what the menu binds. An unmapped key is ignored rather
 * than guessed at. Keys whose character cannot be derived from the virtual key alone (anything
 * needing a layout lookup) still move focus; they simply do not type. */
static unsigned char s_key_down[256];

/* Whether the panel is open, as the layer says before it hands over each frame's keys.
 *
 * ImGui keeps key events in a queue that only `NewFrame` empties, and `NewFrame` runs only while the
 * panel is drawn. Keys handed over while it was shut piled up there, and the next opening played
 * them back, one a frame: arrows pressed in the game since then moved the panel's rows (the owner,
 * Mortal Kombat 11, 2026-10-31). So while shut a key only updates the level kept above, and opening
 * empties the queue and starts every key released. Open by default, for a caller that drives the
 * panel without saying (the fixtures). */
static int s_listening = 1;

extern "C" NREX_API void nrex_listen(nrex_ui *ui, int open)
{
    (void)ui;
    int now = open != 0;
    if (now && !s_listening) {
        ImGuiIO &io = ImGui::GetIO();
        io.ClearEventsQueue();
        io.ClearInputKeys();
    }
    s_listening = now;
}

extern "C" NREX_API void nrex_key(nrex_ui *ui, int vk, int down)
{
    (void)ui;
    if (vk < 0 || vk > 255) return;
    int now = down != 0;
    if (s_key_down[vk] == (unsigned char)now) return;   /* no change: not an event */
    s_key_down[vk] = (unsigned char)now;
    if (!s_listening) return;                            /* shut: remembered, not queued */

    ImGuiIO &io = ImGui::GetIO();
    ImGuiKey key = ImGuiKey_None;
    switch (vk) {
    case 0x26: key = ImGuiKey_UpArrow;    break;
    case 0x28: key = ImGuiKey_DownArrow;  break;
    case 0x25: key = ImGuiKey_LeftArrow;  break;
    case 0x27: key = ImGuiKey_RightArrow; break;
    case 0x0D: key = ImGuiKey_Enter;      break;
    case 0x1B: key = ImGuiKey_Escape;     break;
    case 0x2D: key = ImGuiKey_Insert;     break;
    case 0x23: key = ImGuiKey_End;        break;
    case 0x24: key = ImGuiKey_Home;       break;
    case 0x10: key = ImGuiKey_LeftShift;  break;
    case 0x20: key = ImGuiKey_Space;      break;
    case 0x08: key = ImGuiKey_Backspace;  break;
    case 0x2E: key = ImGuiKey_Delete;     break;
    case 0x09: key = ImGuiKey_Tab;        break;
    case 0xBE: key = ImGuiKey_Period;     break;
    case 0xBD: key = ImGuiKey_Minus;      break;
    default:
        if (vk >= 'A' && vk <= 'Z') key = (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
        else if (vk >= '0' && vk <= '9') key = (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
        break;
    }
    if (key != ImGuiKey_None)
        io.AddKeyEvent(key, now);

    /* The character, on the press only, so a key with no character (an arrow, a function key)
     * types nothing and Control is excluded so Ctrl+S is a shortcut rather than a typed 's'.
     *
     * **Windows** translates through the active keyboard layout with `ToUnicode`, so a layout that
     * puts a digit behind Shift still types that digit - which guessing from the virtual key would
     * not. The key state is BUILT rather than asked for: `GetKeyboardState` reports the calling
     * thread's message queue, and it is only accurate for a thread that pumps messages - which a
     * Vulkan layer does not, it runs inside the game's render thread. The first attempt used it and
     * the typed characters never arrived. `GetAsyncKeyState` is system-wide and needs no pump, so
     * the state is assembled from it, and it also makes a layout's own modifiers come through.
     *
     * **Off Windows** there is no equivalent to ask, so the character is inferred from the virtual
     * key alone - correct for a US layout, wrong for one that needs a modifier. That is the honest
     * limit of a panel that has no window to own an input method; the alternative (no typing at all
     * where a text field exists) is worse. The row that needs text is a number, and digits are
     * digits on every layout. */
#ifdef _WIN32
    if (now && !(GetAsyncKeyState(0x11) & 0x8000)) {
        BYTE state[256];
        memset(state, 0, sizeof state);
        for (int k = 0; k < 256; ++k) {
            SHORT s = GetAsyncKeyState(k);
            if (s & 0x8000) state[k] |= 0x80;      /* the key is down now */
            if (s & 0x0001) state[k] |= 0x01;      /* it was pressed since the last poll */
        }
        state[0x10] = (BYTE)((GetAsyncKeyState(0x10) & 0x8000) ? 0x80 : 0);
        state[0x11] = (BYTE)((GetAsyncKeyState(0x11) & 0x8000) ? 0x80 : 0);
        state[0x12] = (BYTE)((GetAsyncKeyState(0x12) & 0x8000) ? 0x80 : 0);
        state[0x14] = (BYTE)((GetAsyncKeyState(0x14) & 0x8000) ? 0x01 : 0);  /* Caps Lock is a toggle */
        WCHAR buf[8] = {0};
        int n = ToUnicode((UINT)vk, 0, state, buf, 4, 0);
        for (int i = 0; i < n; ++i)
            if (buf[i] >= 0x20)
                io.AddInputCharacter((unsigned int)buf[i]);
    }
#else
    (void)vk;
    if (now && vk >= 0x20 && vk < 0x7F)
        io.AddInputCharacter((unsigned int)vk);
#endif
}

/* ImGui's own pointer, drawn into the frame.
 *
 * `MouseDrawCursor` makes it rasterise an arrow from a texture it already builds into the font
 * atlas, and clearing `HasMouseCursors` stops it asking the OS to change the cursor shape - so the
 * two never fight over who is drawing the pointer. Set per frame by the caller.
 *
 * **It draws nothing without a position**, which is what "the fallback cursor does not appear"
 * turned out to be. ImGui gates its cursor on `io.MousePos` being valid, and `nrex_mouse` reports
 * "no position" (`-FLT_MAX`) whenever the pointer is outside the panel's own image - so the fallback
 * worked only when the pointer was already over the panel, which is precisely when nobody needs it.
 * Measured with `test_soft_cursor.cpp`: 62 vertices with a valid position and 46 without, the
 * difference being the arrow's quad.
 *
 * `nrex_cursor_hint` carries the position separately, so the arrow can be drawn anywhere on screen
 * while hit-testing still sees "no position" and refuses to hover whatever is at the edge. */
static float g_cursor_hint_x = -FLT_MAX;
static float g_cursor_hint_y = -FLT_MAX;

extern "C" NREX_API void nrex_cursor_hint(nrex_ui *ui, float x, float y)
{
    (void)ui;
    g_cursor_hint_x = x;
    g_cursor_hint_y = y;
}

extern "C" NREX_API void nrex_set_software_cursor(nrex_ui *ui, int on)
{
    (void)ui;
    ImGuiIO &io = ImGui::GetIO();
    io.MouseDrawCursor = on != 0;
    if (on)
        io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;
    else
        io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
}

/* Put the software cursor where the pointer is, for the frame about to be built.
 *
 * Called from `nrex_new_frame` AFTER `ImGui::NewFrame`, because `NewFrame` is what processes the
 * queued position events - writing `io.MousePos` before it would be overwritten. Only when ImGui
 * has no position of its own: a real position is still the truth for what is hovered, and moving
 * the arrow to the hint on top of it would make the drawn pointer disagree with the hovered widget.
 *
 * The hint is in the same space the layer computed it in (UI-image pixels), so it is clamped to the
 * image rather than transformed: an arrow at the very edge of the swapchain is still useful, and an
 * arrow outside it would be clipped away with no explanation. */
static void nrex_place_software_cursor(nrex_ui *ui)
{
    if (!ui) return;
    ImGuiIO &io = ImGui::GetIO();
    if (!io.MouseDrawCursor) return;
    if (io.MousePos.x != -FLT_MAX && io.MousePos.y != -FLT_MAX) return;   /* already valid */
    if (g_cursor_hint_x == -FLT_MAX || g_cursor_hint_y == -FLT_MAX) return;
    float x = g_cursor_hint_x, y = g_cursor_hint_y;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (x > io.DisplaySize.x) x = io.DisplaySize.x;
    if (y > io.DisplaySize.y) y = io.DisplaySize.y;
    io.MousePos = ImVec2(x, y);
}

extern "C" NREX_API void nrex_mouse(nrex_ui *ui, float x, float y, int down)
{
    (void)ui;
    ImGuiIO &io = ImGui::GetIO();
    /* A position outside the panel's own image is reported as ImGui's "no position" sentinel rather
     * than as a coordinate. Clamping it would hover whatever sits at the edge, and a click there
     * would land on a widget the pointer was never over. */
    if (x < 0.0f || y < 0.0f || x >= io.DisplaySize.x || y >= io.DisplaySize.y) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
        return;
    }
    /* Trace the pointer in ImGui's coordinates when it moves or clicks. The next report then says
     * whether coordinates arrive at all and where they land, instead of a guess.
     *
     * Behind NREX_TRACE, which is off in a release. It was on, unthrottled, on every pointer move -
     * in somebody else's process, writing to the game's stderr while they played. It earned its keep
     * while the pointer conversion was being worked out and it costs nothing to keep it there. */
    static float last_x = -1.0f, last_y = -1.0f;
    static int last_down = 0;
    if (x != last_x || y != last_y || down != last_down) {
        last_x = x; last_y = y; last_down = down;
        NREX_TRACE("[mouse] x=%.0f y=%.0f down=%d display=%.0fx%.0f\n",
                   x, y, down, io.DisplaySize.x, io.DisplaySize.y);
    }
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, down != 0);
}
extern "C" NREX_API const char *nrex_language_name(int language)
{
    static const char *names[] = {"English", "中文", "Русский", "Español"};
    return (language >= 0 && language < 4) ? names[language] : names[0];
}

/* Does ImGui draw its own cursor under the conditions this layer sets up?
 *
 * The report is "the fallback cursor does not appear when the system cursor is hidden", and the
 * code that decides it looks correct - so the question is which of ImGui's own conditions is not
 * being met. This drives ImGui headlessly and reads `DrawData` for the cursor triangles, which is
 * the same data the panel renders.
 *
 * The interesting variable is the mouse POSITION. `ImGui::SetMouseCursor` only has an effect while
 * a valid position is set, and this layer reports "no position" (`-FLT_MAX`) whenever the pointer
 * is outside the panel's own image - which is most of the screen, and is exactly when a user would
 * notice the system cursor is gone and the fallback is missing.
 *
 * Build:
 *   cl /nologo /O2 /TP /utf-8 /EHsc /DNDEBUG /D_WIN32 /D_IMGUI_DEFINE_MATH_OPERATORS ^
 *      /I third_party\imgui /I src\layer /Fe:work\test_soft_cursor.exe ^
 *      src\layer\test_soft_cursor.cpp third_party\imgui\imgui.cpp ^
 *      third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp ^
 *      third_party\imgui\imgui_widgets.cpp user32.lib gdi32.lib
 */
#include "imgui.h"

#include <stdio.h>
#include <string.h>

/* The cursor is drawn as a quad in the draw data, and its texture is the font atlas's white pixel
 * plus a rectangle ImGui carves out for the arrow. Counting vertices only when
 * `MouseDrawCursor` is on is the signal: the window itself contributes vertices either way. */
static int frame_vertices(bool draw_cursor, bool valid_pos, bool has_backend_cursors,
                          bool focused, bool report[4], float hint_x = -FLT_MAX,
                          float hint_y = -FLT_MAX)
{
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0f, 360.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.MouseDrawCursor = draw_cursor;
    if (has_backend_cursors) io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    else                     io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;

    if (valid_pos) io.AddMousePosEvent(320.0f, 180.0f);
    else           io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    io.AddMouseButtonEvent(0, false);

    ImGui::NewFrame();
    /* The fix, in the same place `nrex_new_frame` applies it: after NewFrame, and only when ImGui
     * has no position of its own. */
    if (hint_x != -FLT_MAX && io.MousePos.x == -FLT_MAX)
        io.MousePos = ImVec2(hint_x, hint_y);
    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(560, 380), ImGuiCond_Always);
    if (!focused) ImGui::SetNextWindowFocus();
    ImGui::Begin("t", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted("hello");
    ImGui::End();
    ImGui::Render();

    ImDrawData *dd = ImGui::GetDrawData();
    int total = 0;
    for (int i = 0; i < dd->CmdListsCount; ++i)
        total += dd->CmdLists[i]->VtxBuffer.Size;

    report[0] = io.MouseDrawCursor;
    report[1] = valid_pos;
    report[2] = (io.BackendFlags & ImGuiBackendFlags_HasMouseCursors) != 0;
    report[3] = ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow);
    return total;
}

int main(void)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    io.Fonts->AddFontDefault();
    io.Fonts->Build();

    bool r[4];
    int fails = 0;

#define CHECK(name, cond)                                                    \
    do {                                                                     \
        printf("  %s  %s\n", (cond) ? "PASS" : "FAIL", name);                 \
        if (!(cond)) ++fails;                                                \
    } while (0)

    printf("test_soft_cursor: does the fallback pointer get drawn?\n\n");

    /* The baseline: no cursor wanted, so the window is all that is in the frame. */
    int no_cursor = frame_vertices(false, true, true, true, r);
    /* Wanted AND positioned: the arrow is in the frame, and this is the reference count. */
    int cursor_over_panel = frame_vertices(true, true, false, true, r);

    CHECK("a valid position draws the cursor", cursor_over_panel > no_cursor);

    /* The reported case: the system cursor is hidden and the pointer is NOT over the panel, so the
     * layer reports "no position". ImGui then draws no ARROW - it contributes 6 vertices more than
     * an empty frame, which is not the arrow's 22, so the count lands well short of the reference.
     * (Asserting equality with `no_cursor` was the first version of this check and it failed: the
     * extra 6 are real and unrelated to the arrow, so "short of the reference" is the honest test.) */
    int no_hint = frame_vertices(true, false, false, true, r);
    CHECK("without a hint, an off-panel pointer draws no arrow",
          no_hint < cursor_over_panel);

    /* The fix: the hint puts the arrow back, without giving ImGui a position for hit-testing. */
    int hinted = frame_vertices(true, false, false, true, r, 500.0f, 300.0f);
    CHECK("the hint draws the cursor with no position of its own", hinted > no_hint);
    CHECK("the hinted cursor is the same arrow a real position draws",
          hinted == cursor_over_panel);

    printf("\n  vertices: none=%d overPanel=%d noHint=%d hinted=%d\n",
           no_cursor, cursor_over_panel, no_hint, hinted);

    /* The hint must not become a hover. Hit-testing reads ImGui's own position, and the point of
     * reporting "no position" is that a widget at the edge is not hovered by a pointer that is
     * somewhere else entirely. */
    {
        ImGuiIO &g = ImGui::GetIO();
        g.DisplaySize = ImVec2(640.0f, 360.0f);
        g.MouseDrawCursor = true;
        g.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        ImGui::NewFrame();
        g.MousePos = ImVec2(300.0f, 200.0f);      /* the hint */
        ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(560, 380), ImGuiCond_Always);
        ImGui::Begin("t", nullptr, ImGuiWindowFlags_NoCollapse);
        bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_None);
        ImGui::End();
        ImGui::Render();
        /* The arrow is drawn at the hint, and ImGui's hover test uses the same MousePos - so in
         * principle a hinted position could hover a widget the pointer is not over. In practice
         * `hovered` comes out 0 here because the window is not focused when this runs, so this
         * probe does not settle the question; what it records is that the hint IS a real position
         * to ImGui, which is why the layer only sets it while the system cursor is hidden. While
         * the OS cursor is visible the real position is used and this never applies. */
        printf("  (hint is a real position to ImGui; only set while the system cursor is hidden.\n"
               "   hovered=%d here, with no focused window - not a hover test)\n",
               hovered ? 1 : 0);
    }

    /* --- does a drag outside the panel move the panel? ---------------------------------------
     *
     * The report is "dragging the cursor after leaving the panel moves the floating window". The
     * panel is `ImGuiWindowFlags_NoCollapse` with the default movable flag and a title bar, so a
     * press on the title bar and a move IS a window move - and the layer feeds real button state
     * with a position, so a drag anywhere ImGui considers the title bar drags the panel.
     *
     * This measures it rather than reasoning about it: press at the title bar, move, and see
     * whether the window's position changed.
     */
    {
        ImGuiIO &g = ImGui::GetIO();
        g.DisplaySize = ImVec2(640.0f, 360.0f);
        g.MouseDrawCursor = false;
        g.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        ImGui::NewFrame();
        ImVec2 before(0, 0), after(0, 0);
        /* `GetWindowPos` is only valid between Begin and End - calling it outside a window's scope
         * is the access violation the first version of this hit, and it took the whole probe down
         * with no output. The position is therefore read inside the frame that cares about it. */
        ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(180, 120), ImGuiCond_Always);
        ImGui::Begin("t", nullptr, ImGuiWindowFlags_NoCollapse);
        before = ImGui::GetWindowPos();
        ImGui::TextUnformatted("x");
        ImGui::End();
        ImGui::Render();

        /* Press on the title bar (y a few pixels above the content) and drag right. */
        g.AddMousePosEvent(60.0f, 30.0f);
        g.AddMouseButtonEvent(0, true);
        ImGui::NewFrame();
        ImGui::Begin("t", nullptr, ImGuiWindowFlags_NoCollapse);
        ImGui::TextUnformatted("x");
        ImGui::End();
        ImGui::Render();
        g.AddMousePosEvent(200.0f, 30.0f);
        ImGui::NewFrame();
        ImGui::Begin("t", nullptr, ImGuiWindowFlags_NoCollapse);
        after = ImGui::GetWindowPos();
        ImGui::TextUnformatted("x");
        ImGui::End();
        ImGui::Render();
        g.AddMouseButtonEvent(0, false);

        printf("\n  title-bar drag: window moved from (%.0f,%.0f) to (%.0f,%.0f)\n",
               before.x, before.y, after.x, after.y);
        printf("  -> a press ON the title bar moves the panel: %s (this is wanted)\n",
               (after.x != before.x || after.y != before.y) ? "YES" : "no");
        CHECK("a press on the title bar CAN move the panel",
              after.x != before.x || after.y != before.y);
    }

    /* --- a drag that STARTS away from the panel must not move it ------------------------------
     *
     * This is the reported bug: "dragging the cursor after leaving the panel moves the floating
     * window". The press happens somewhere else on screen, the pointer then passes over the panel,
     * and the panel must stay put - a window is moved by a press on its title bar, never by a
     * pointer that merely travels across it with the button already down.
     *
     * The `NoMove` fix that came before this forbade moving entirely, which is why it was wrong:
     * the gesture that had to stop is this one, and the gesture that had to keep working is the
     * press on the title bar.
     */
    {
        ImGuiIO &g = ImGui::GetIO();
        g.DisplaySize = ImVec2(640.0f, 360.0f);
        g.MouseDrawCursor = false;
        g.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        ImGui::NewFrame();
        ImVec2 before(0, 0), after(0, 0);
        ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(180, 120), ImGuiCond_Always);
        ImGui::Begin("t3", nullptr, ImGuiWindowFlags_NoCollapse);
        before = ImGui::GetWindowPos();
        ImGui::TextUnformatted("x");
        ImGui::End();
        ImGui::Render();

        /* Press FAR from the panel - bottom right of the display - then drag across it. */
        g.AddMousePosEvent(560.0f, 320.0f);
        g.AddMouseButtonEvent(0, true);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_Always);
        ImGui::Begin("t3", nullptr, ImGuiWindowFlags_NoCollapse);
        ImGui::TextUnformatted("x");
        ImGui::End();
        ImGui::Render();
        /* Now travel over the title bar with the button still down. */
        g.AddMousePosEvent(60.0f, 30.0f);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_Always);
        ImGui::Begin("t3", nullptr, ImGuiWindowFlags_NoCollapse);
        after = ImGui::GetWindowPos();
        ImGui::TextUnformatted("x");
        ImGui::End();
        ImGui::Render();
        g.AddMouseButtonEvent(0, false);

        printf("\n  off-panel press, then drag across the title bar:\n"
               "    (%.0f,%.0f) -> (%.0f,%.0f)\n", before.x, before.y, after.x, after.y);
        CHECK("a drag starting off the panel does not move it",
              after.x == before.x && after.y == before.y);
    }

    /* --- does the row's hint appear on hover? ----------------------------------------------
     *
     * The overlay's settings rows now carry a one-line explanation, shown when the pointer rests on
     * the row - OptiScaler's tooltip, and the thing that tells you what "hold unchanged pixels"
     * does without leaving the game. The strings come from the same generated table the installer
     * uses, so the two explain a knob with the same words.
     *
     * A tooltip is a WINDOW of its own in ImGui, so the frame gains vertices when one opens. That
     * is the measurement: more vertices with the pointer over the row than with it away.
     */
    {
        ImGuiIO &g = ImGui::GetIO();
        g.DisplaySize = ImVec2(640.0f, 360.0f);
        g.MouseDrawCursor = false;
        const char *hint = "Fraction of each dimension processed by the network.";

        auto draw = [&](float mx, float my, int *report = nullptr) {            g.AddMousePosEvent(mx, my);
            g.AddMouseButtonEvent(0, false);
            ImGui::NewFrame();
            if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
                ImGui::SetNextWindowFocus();
            ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(560, 380), ImGuiCond_Always);
            ImGui::Begin("tip", nullptr, ImGuiWindowFlags_NoCollapse);
            if (report) {
                report[0] = ImGui::IsWindowFocused(ImGuiFocusedFlags_None);
                report[1] = ImGui::IsWindowHovered(ImGuiHoveredFlags_None);
            }
            ImGui::Button("row");
            if (report) {
                report[2] = ImGui::IsItemHovered();
                ImVec2 lo = ImGui::GetItemRectMin();
                report[3] = (int)lo.x;
                report[4] = (int)lo.y;
            }
            if (hint && hint[0] && ImGui::IsItemHovered()) {
                if (report) report[5] = 1;   /* the tooltip branch was entered */
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
                bool open = ImGui::BeginTooltip();
                if (report) report[6] = open ? 1 : 0;
                /* The wrap position is an absolute local X - ImGui's own demo passes
                 * `GetFontSize() * 35.0f`. Relative-to-cursor inside a fresh tooltip collapses it. */
                ImGui::PushTextWrapPos(320.0f);
                ImGui::TextUnformatted(hint);
                ImGui::PopTextWrapPos();
                /* Read the size AFTER the text: at Begin time the window still reports the previous
                 * frame's extent, which on the first frame the tip opens is the empty 16x12 - and
                 * that reading is what made a perfectly good tooltip look like a failure. */
                if (report) {
                    report[7] = (int)(ImGui::GetWindowSize().x * 10.0f);
                    report[8] = (int)(ImGui::GetWindowSize().y * 10.0f);
                    report[9] = (int)(ImGui::GetCursorPosY() * 10.0f);
                    report[10] = (int)(ImGui::GetTextLineHeight() * 10.0f);
                    report[11] = (int)(ImGui::CalcTextSize(hint, nullptr, false, 320.0f).x * 10.0f);
                    report[12] = (int)(ImGui::CalcTextSize(hint).x * 10.0f);
                }
                ImGui::EndTooltip();
                ImGui::PopStyleVar();
            }
            ImGui::End();
            ImGui::Render();
            ImDrawData *dd = ImGui::GetDrawData();
            int total = 0;
            for (int i = 0; i < dd->CmdListsCount; ++i)
                total += dd->CmdLists[i]->VtxBuffer.Size;
            return total;
        };

        /* Settle the window's position first, then measure. */
        int report[13] = {0};
        draw(-FLT_MAX, -FLT_MAX);
        int away = draw(600.0f, 300.0f);
        /* Aim at the button's own rect rather than at a guessed coordinate: the first version
         * used (100,50), which is inside the window but not on the item, so `IsItemHovered` was
         * false and the test failed for a reason that had nothing to do with the tooltip. The
         * rect is read from the frame that drew it. */
        draw(100.0f, 50.0f, report);
        float bx = (float)report[3] + 10.0f;
        float by = (float)report[4] + 5.0f;
        /* Hover twice. A tooltip's window is SIZED on the frame it first appears and reports that
         * size from the next frame on, so one frame of hovering measures the empty default 16x12 -
         * which is what made this look like a broken tooltip when the tooltip was working. The
         * second frame is the one that answers the question. */
        draw(bx, by, report);
        int over = draw(bx, by, report);
        printf("\n  the button's rect starts at (%.0f,%.0f); aiming at (%.0f,%.0f)\n",
               (float)report[3], (float)report[4], bx, by);
        printf("  focused=%d win_hovered=%d item_hovered=%d branch=%d Begin=%d\n",
               report[0], report[1], report[2], report[5], report[6]);
        printf("  tip_size=%.1fx%.1f cursor_y_after=%.1f line_height=%.1f\n",
               report[7] / 10.0f, report[8] / 10.0f, report[9] / 10.0f, report[10] / 10.0f);
        printf("  CalcTextSize wrapped=%.1f  unwrapped=%.1f\n",
               report[11] / 10.0f, report[12] / 10.0f);
        printf("  vertices: away=%d, on the row=%d\n", away, over);
        CHECK("the hint appears when the pointer is on the row", over > away);
        CHECK("a hint that is NULL draws nothing extra", away == draw(600.0f, 300.0f));
    }

    /* --- does the Right arrow change the value, or move focus to the field? ------------------
     *
     * The report: "left correct, right selects the number box". ImGui's navigation consumes
     * Left/Right to move BETWEEN items, and it does that before a row's own arrow handling runs -
     * so with a focusable number field to the right of the slider, Right moved focus there and Left
     * (nothing to the left) fell through to the value. Marking the field
     * `ImGuiItemFlags_NoNav` is the fix.
     *
     * This reproduces the row exactly as the panel builds it - slider, then field, then label - and
     * presses Right. The question is whether the slider's value changed or the focus moved.
     */
    {
        ImGuiIO &g = ImGui::GetIO();
        g.DisplaySize = ImVec2(640.0f, 360.0f);
        g.MouseDrawCursor = false;
        g.AddMousePosEvent(-FLT_MAX, -FLT_MAX);

        /* The value PERSISTS across frames. A version that reset it to 0.5 every frame could not
         * see a change at all, and one that re-pinned focus with `SetKeyboardFocusHere` could not
         * see focus move - so it passed whether or not the bug was present, which is how the
         * sabotage check caught it out. */
        float value = 0.5f;
        float other = 0.5f;
        bool field_focused = false;
        bool saw_field_focus = false;

        /* The row, then a SECOND row below it. That second row is what makes this a faithful
         * reproduction: in a synthetic single-row panel there is nowhere for the Right arrow to go,
         * so nav finds no target and the value changes whether or not the field is focusable - the
         * first version of this test passed with the bug reintroduced because of exactly that. A
         * real panel has ten rows and a footer, so nav always has somewhere to move. */
        auto run = [&](bool field_nav, bool pin_focus) {
            ImGui::NewFrame();
            if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
                ImGui::SetNextWindowFocus();
            ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(560, 380), ImGuiCond_Always);
            ImGui::Begin("nav", nullptr, ImGuiWindowFlags_NoCollapse);
            ImGui::PushID(0);
            ImGui::SetNextItemWidth(190.0f);
            /* Focus ONCE. After that ImGui's own navigation owns where focus goes, which is the
             * thing under test. */
            if (pin_focus) ImGui::SetKeyboardFocusHere();
            ImGui::SliderFloat("##v", &value, 0.0f, 1.0f, "");
            if (ImGui::IsItemFocused()) {
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  value -= 0.1f;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) value += 0.1f;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(86.0f);
            float typed = value;
            if (field_nav) {
                ImGui::InputFloat("##t", &typed, 0.0f, 0.0f, "%.3f",
                                  ImGuiInputTextFlags_EnterReturnsTrue);
            } else {
                ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                ImGui::InputFloat("##t", &typed, 0.0f, 0.0f, "%.3f",
                                  ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::PopItemFlag();
            }
            field_focused = ImGui::IsItemFocused();
            if (field_focused) saw_field_focus = true;
            ImGui::SameLine();
            ImGui::TextUnformatted("Render scale");
            ImGui::PopID();

            /* A second row, so there is a focusable item to move to. */
            ImGui::PushID(1);
            ImGui::SliderFloat("##v2", &other, 0.0f, 1.0f, "");
            ImGui::SameLine();
            ImGui::TextUnformatted("Intensity");
            ImGui::PopID();

            ImGui::End();
            ImGui::Render();
        };

        /* Focus the slider, let it settle, then press Right three times. */
        run(false, true);
        for (int i = 0; i < 3; ++i) run(false, false);
        for (int press = 0; press < 3; ++press) {
            g.AddKeyEvent(ImGuiKey_RightArrow, true);
            run(false, false);
            g.AddKeyEvent(ImGuiKey_RightArrow, false);
            run(false, false);
        }

        printf("\n  field NoNav: value=%.2f (from 0.50), field ever focused=%d\n",
               value, saw_field_focus ? 1 : 0);
        CHECK("Right changes the value with the field out of nav", value > 0.5f);
        CHECK("Right never focuses the number field", !saw_field_focus);

        /* And the other direction, so the check is not satisfied by a row where nothing works. */
        float before_left = value;
        g.AddKeyEvent(ImGuiKey_LeftArrow, true);
        run(false, false);
        g.AddKeyEvent(ImGuiKey_LeftArrow, false);
        run(false, false);
        printf("  after Left: value=%.2f (was %.2f)\n", value, before_left);
        CHECK("Left still changes the value", value < before_left);
    }

    printf("\n%s\n", fails ? "FAILED" : "ok");
    ImGui::DestroyContext();
    return fails;
}

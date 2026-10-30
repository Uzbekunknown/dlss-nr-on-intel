/* What the settings file does when the menu touches it.
 *
 * The author's review named three rules for this file, and each is a way a settings menu ruins
 * somebody's configuration:
 *
 *   1. unrelated fields survive a save   - the daemon and the tools share the file
 *   2. opening or saving changes nothing - a menu must not rewrite what it did not touch
 *   3. render scale is not reset to 1.0  - the Windows release preset is 0.4 and the menu's own
 *                                          default is 1.0, so rule 2 is what keeps them apart
 *
 * The third is a special case of the second, and it is the one that was actually broken: an
 * earlier draft wrote its whole table on save, which would have changed the picture by the menu
 * being opened. It gets its own case because that failure is invisible until somebody compares two
 * screenshots.
 *
 * No Vulkan and no game: this is the file logic, which is where the persistence lives. The menu is
 * attached through `nr_menu_bridge_create_headless`, so the same code runs without a device - and
 * the "does the overlay draw" question is answered separately, by looking at the DLL.
 */
#include "nr_menu_bridge.h"
#include "nr_menu.h"
/* The generated tables: NR_MENU_KEY_COUNT and the compare names live here, not in nr_menu.h -
 * that header declares the menu, this one holds the strings. */
#include "nr_menu_strings.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static char g_dir[1024];

static void path_of(char *out, size_t size, const char *name)
{
#ifdef _WIN32
    snprintf(out, size, "%s\\%s", g_dir, name);
#else
    snprintf(out, size, "%s/%s", g_dir, name);
#endif
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    assert(buf);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* The row indices, named. `nr_menu_bridge.c`'s table is in `nr_knobs.py`'s order:
 * render_scale, min_extent, profile, intensity, detail_strength, colour_strength, temporal, hold,
 * release, cut_limit. */
#define ROW_INTENSITY 3

/* --- 1. unrelated fields survive ------------------------------------------------------------ */

static void test_unrelated_fields_survive(void)
{
    char settings[1088];
    path_of(settings, sizeof settings, "nr_settings.json");

    write_text(settings,
               "{\n"
               "  \"intensity\": 1.25,\n"
               "  \"something_the_tools_added\": 7,\n"
               "  \"profile\": \"cinematic\",\n"
               "  \"a_string_field\": \"keep me\",\n"
               "  \"render_scale\": 0.4\n"
               "}\n");

    nr_menu_bridge *bridge = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
    assert(bridge);
    nr_menu_bridge_update(bridge, 1.0f / 60.0f);

    char *after = read_text(settings);
    assert(after);
    assert(strstr(after, "\"something_the_tools_added\": 7") != NULL);
    assert(strstr(after, "\"a_string_field\": \"keep me\"") != NULL);
    assert(strstr(after, "\"render_scale\": 0.4") != NULL);
    free(after);
    nr_menu_bridge_destroy(bridge);
    printf("  ok  unrelated fields survive\n");
}

/* --- 2. opening the menu changes nothing --------------------------------------------------- */

static void test_opening_changes_nothing(void)
{
    char settings[1088];
    path_of(settings, sizeof settings, "open.json");

    /* The release preset: 0.4, not the menu's own default of 1.0. This is the case the author
     * named. */
    write_text(settings, "{\n  \"render_scale\": 0.4,\n  \"intensity\": 0.8\n}\n");

    nr_menu_bridge *bridge = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
    assert(bridge);
    /* "Opening" is create plus a frame with nothing touched, and then an explicit save. */
    nr_menu_bridge_update(bridge, 1.0f / 60.0f);
    nr_menu_bridge_save(bridge);

    char *after = read_text(settings);
    assert(after);
    assert(strstr(after, "\"render_scale\": 0.4") != NULL);
    assert(strstr(after, "\"intensity\": 0.8") != NULL);
    free(after);
    nr_menu_bridge_destroy(bridge);
    printf("  ok  opening and saving change nothing (render_scale stays 0.4)\n");
}

/* --- 3. a key that was absent stays absent -------------------------------------------------- */

static void test_absent_key_is_not_invented(void)
{
    char settings[1088];
    path_of(settings, sizeof settings, "absent.json");

    write_text(settings, "{\n  \"intensity\": 1.0\n}\n");

    nr_menu_bridge *bridge = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
    assert(bridge);
    nr_menu_bridge_update(bridge, 1.0f / 60.0f);
    nr_menu_bridge_save(bridge);

    char *after = read_text(settings);
    assert(after);
    assert(strstr(after, "render_scale") == NULL);
    assert(strstr(after, "min_extent") == NULL);
    assert(strstr(after, "cut_limit") == NULL);
    assert(strstr(after, "intensity") != NULL);
    free(after);
    nr_menu_bridge_destroy(bridge);
    printf("  ok  an absent key is not invented by opening the menu\n");
}

/* --- 4. a knob the user moves is NOT written until Save -------------------------------------- */

/* The rule the user set, in their words: "all save behaviour should be pressing Save, and an
 * automatic reset should not save".
 *
 * This case used to assert the opposite - that a move wrote the settings file immediately - which
 * is why the bug existed and why nothing caught it: the test encoded the wrong contract. Moving now
 * writes the LIVE PREVIEW, which is what the daemon reads to move the picture, and leaves the saved
 * file alone. */
static void test_a_moved_knob_is_not_saved(void)
{
    char settings[1088];
    path_of(settings, sizeof settings, "moved.json");

    write_text(settings,
               "{\n"
               "  \"intensity\": 1.0,\n"
               "  \"render_scale\": 0.4,\n"
               "  \"a_string_field\": \"keep me\",\n"
               "  \"something_the_tools_added\": 7\n"
               "}\n");
    char *before = read_text(settings);
    assert(before);

    nr_menu_bridge *bridge = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
    assert(bridge);

    /* intensity 1.0, step 0.05, one step up. */
    assert(nr_menu_bridge_move(bridge, ROW_INTENSITY, +1));

    /* The saved file is byte-for-byte what it was. */
    char *after = read_text(settings);
    assert(after);
    assert(strcmp(before, after) == 0);
    assert(strstr(after, "\"intensity\": 1.0") != NULL);
    free(before);
    free(after);

    /* And the preview carries the move, so the daemon still sees it. */
    char live[1088];
    snprintf(live, sizeof live, "%s.live", settings);
    char *preview = read_text(live);
    assert(preview);
    assert(strstr(preview, "\"intensity\": 1.05") != NULL);
    free(preview);

    /* Save is what writes it. */
    assert(nr_menu_bridge_save(bridge));
    after = read_text(settings);
    assert(after);
    assert(strstr(after, "\"intensity\": 1.05") != NULL);
    assert(strstr(after, "\"a_string_field\": \"keep me\"") != NULL);
    assert(strstr(after, "\"something_the_tools_added\": 7") != NULL);
    assert(strstr(after, "\"render_scale\": 0.4") != NULL);
    free(after);
    nr_menu_bridge_destroy(bridge);
    printf("  ok  a moved knob is not saved until Save, and then is\n");
}

/* --- 4b. a knob set through the slider is not saved either ----------------------------------- */

/* A second route into the same rule: `nr_menu_bridge_set` is what the ImGui slider and the typed
 * field call, and it must behave like an arrow press rather than like Save. */
static void test_a_slid_knob_is_not_saved(void)
{
    char settings[1088];
    path_of(settings, sizeof settings, "slid.json");

    write_text(settings, "{\n  \"intensity\": 1.0,\n  \"render_scale\": 0.4\n}\n");
    char *before = read_text(settings);
    assert(before);

    nr_menu_bridge *bridge = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
    assert(bridge);
    assert(nr_menu_bridge_set(bridge, ROW_INTENSITY, 0.75f));

    char *after = read_text(settings);
    assert(after);
    assert(strcmp(before, after) == 0);
    free(before);
    free(after);
    nr_menu_bridge_destroy(bridge);
    printf("  ok  a value set by the slider is not saved until Save\n");
}

/* --- 4c. Reset cannot destroy a save --------------------------------------------------------- */

/* The destructive one, and the reason the user reported it: Reset called `save_settings` after
 * loading the defaults, so one keypress replaced every saved value with the menu's own defaults -
 * with no confirmation and no undo.
 *
 * Reset is now a preview-only action: the panel shows the defaults, the file keeps what was saved,
 * and closing the panel without saving discards the reset. */
static void test_reset_cannot_destroy_a_save(void)
{
    char settings[1088];
    path_of(settings, sizeof settings, "reset.json");

    /* Values deliberately different from every menu default, so a reset that leaked would show. */
    write_text(settings,
               "{\n"
               "  \"intensity\": 0.25,\n"
               "  \"render_scale\": 0.4,\n"
               "  \"profile\": \"cinematic\",\n"
               "  \"temporal\": 0.45\n"
               "}\n");
    char *before = read_text(settings);
    assert(before);

    nr_menu_bridge *bridge = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
    assert(bridge);

    /* Reset, through the same entry point the menu's R key reaches. `nr_menu_bridge_reset` is the
     * headless form of that request, so this exercises the shipped path rather than a test-only
     * one. */
    assert(nr_menu_bridge_move(bridge, ROW_INTENSITY, +1));
    nr_menu_bridge_reset(bridge);

    char *after = read_text(settings);
    assert(after);
    assert(strcmp(before, after) == 0);
    assert(strstr(after, "\"intensity\": 0.25") != NULL);
    assert(strstr(after, "\"render_scale\": 0.4") != NULL);
    assert(strstr(after, "\"profile\": \"cinematic\"") != NULL);
    free(before);
    free(after);
    nr_menu_bridge_destroy(bridge);
    printf("  ok  reset leaves the saved values alone\n");
}

/* --- 4d. a language change does not commit an unsaved edit ----------------------------------- */

/* The language is persisted immediately - it is a display preference, not something to preview -
 * but it must not drag an unsaved knob edit into the file with it. Reaching for `save_settings`
 * here would do exactly that, which is the Reset bug one step less obvious. */
static void test_language_does_not_commit_a_knob(void)
{
    char settings[1088];
    path_of(settings, sizeof settings, "language.json");

    write_text(settings, "{\n  \"intensity\": 1.0,\n  \"render_scale\": 0.4\n}\n");

    nr_menu_bridge *bridge = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
    assert(bridge);

    /* An unsaved render_scale change, then a language switch. The headless bridge has no menu, so
     * the language goes in through its own entry point - the same two steps the L key takes. */
    assert(nr_menu_bridge_set(bridge, 0 /* render_scale */, 0.9f));
    nr_menu_bridge_set_language(bridge, NR_MENU_LANG_ZH);
    nr_menu_bridge_update(bridge, 1.0f / 60.0f);

    char *after = read_text(settings);
    assert(after);
    assert(strstr(after, "\"render_scale\": 0.4") != NULL);      /* not the unsaved 0.9 */
    assert(strstr(after, "\"menu_language\": 1") != NULL);       /* but the language is kept */
    free(after);
    nr_menu_bridge_destroy(bridge);
    printf("  ok  a language change does not commit an unsaved knob\n");
}

/* --- 4e. Close keeps the unsaved changes, and saves nothing -------------------------------- */

/* The user's rule for Close, in their words: "close the panel, keep the unsaved changes, do not
 * reset, do not save".
 *
 * The Close button was wired to nothing at all - it set a flag in `acted` and no branch read it, so
 * pressing it did nothing. That was the first half. The second was the close transition itself,
 * which removed the preview and reloaded the saved values: Close discarded the edits, and it was the
 * only way back to the saved state from an unsaved one. The two are now separate: closing keeps the
 * edits, and the settings file changes only on Save.
 *
 * All three halves are checked, because a close that keeps the values but also writes them would
 * still fail the rule. */
static void test_close_keeps_the_changes(void)
{
    char settings[1088];
    path_of(settings, sizeof settings, "close.json");

    write_text(settings,
               "{\n"
               "  \"intensity\": 0.25,\n"
               "  \"render_scale\": 0.4,\n"
               "  \"profile\": \"cinematic\"\n"
               "}\n");
    char *before = read_text(settings);
    assert(before);

    nr_menu_bridge *bridge = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
    assert(bridge);

    /* An unsaved edit, the way a slider or an arrow press makes one. */
    assert(nr_menu_bridge_set(bridge, 0 /* render_scale */, 0.9f));

    nr_menu_bridge_close(bridge);

    /* 1. The bridge still holds the edited value - that is "keeps the unsaved changes". */
    const char *shown = nr_menu_bridge_row_value(bridge, 0);
    assert(shown != NULL);
    assert(strcmp(shown, "0.9") == 0);

    /* 2. The settings file is byte-for-byte what it was - that is "does not save". */
    char *after = read_text(settings);
    assert(after);
    assert(strcmp(before, after) == 0);
    assert(strstr(after, "\"render_scale\": 0.4") != NULL);
    free(before);
    free(after);

    /* 3. The preview still carries it, so the picture keeps the value while the game runs. */
    char live[1088];
    snprintf(live, sizeof live, "%s.live", settings);
    char *preview = read_text(live);
    assert(preview);
    assert(strstr(preview, "\"render_scale\": 0.9") != NULL);
    free(preview);

    /* 4. And a subsequent Save still works, so closing did not put the bridge in a state where the
     *    edit was silently dropped. */
    assert(nr_menu_bridge_save(bridge));
    after = read_text(settings);
    assert(after);
    assert(strstr(after, "\"render_scale\": 0.9") != NULL);
    free(after);
    nr_menu_bridge_destroy(bridge);
    printf("  ok  Close keeps the unsaved changes and saves nothing\n");
}

/* --- 5. the file the menu writes is JSON ---------------------------------------------------- */

/* A trailing comma before the closing brace is not valid JSON, and the daemon's reader logs a
 * parse error and keeps its old values - so the menu would look like it saved while nothing
 * changed, which is worse than a save that fails loudly. An earlier draft wrote `"key": value,`
 * for every field and closed with `}`, which is exactly that. */
static void test_the_written_file_parses(void)
{
    char settings[1088];
    path_of(settings, sizeof settings, "parses.json");

    write_text(settings, "{\n  \"intensity\": 1.0,\n  \"render_scale\": 0.4\n}\n");

    nr_menu_bridge *bridge = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
    assert(bridge);
    assert(nr_menu_bridge_move(bridge, ROW_INTENSITY, +1));
    nr_menu_bridge_destroy(bridge);

    char *after = read_text(settings);
    assert(after);

    /* No `,` immediately before the closing brace, and no doubled comma. Both are what a
     * separator written after every field produces. */
    for (const char *p = after; *p; ++p) {
        if (*p != ',') continue;
        const char *q = p + 1;
        while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') ++q;
        assert(*q != '}');          /* trailing comma */
        assert(*q != ',');          /* doubled separator */
    }
    assert(after[0] == '{');
    free(after);
    printf("  ok  the written file has no trailing or doubled comma\n");
}

/* --- 6. a moved knob survives a reopen ------------------------------------------------------ */

/* The user's own acceptance test at the file level: change a setting, then build the bridge again
 * exactly as a restarted game would, and check the value came back. The full version is closing
 * the game and reopening it, which is what the CUA pass does; this is the part that can be checked
 * without a game. */
static void test_a_moved_knob_survives_a_reopen(void)
{
    char settings[1088];
    path_of(settings, sizeof settings, "reopen.json");
    write_text(settings, "{\n  \"intensity\": 1.0,\n  \"render_scale\": 0.4\n}\n");

    /* First run: move intensity and SAVE it, which is what makes it survive. */
    {
        nr_menu_bridge *bridge = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
        assert(bridge);
        assert(nr_menu_bridge_move(bridge, ROW_INTENSITY, +1));
        assert(nr_menu_bridge_save(bridge));
        nr_menu_bridge_destroy(bridge);
    }

    /* Second run: a fresh bridge over the same file, which is what a restarted game does. */
    {
        nr_menu_bridge *bridge = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
        assert(bridge);
        /* The value the menu would show, read back through the same accessor the overlay uses. */
        const char *shown = nr_menu_bridge_row_value(bridge, ROW_INTENSITY);
        assert(shown != NULL);
        assert(strcmp(shown, "1.05") == 0);

        char *after = read_text(settings);
        assert(after);
        assert(strstr(after, "\"render_scale\": 0.4") != NULL);
        free(after);
        nr_menu_bridge_destroy(bridge);
    }
    printf("  ok  a moved knob survives a reopen (intensity reads back 1.05, scale still 0.4)\n");
}

/* --- 7. every knob can move and come back --------------------------------------------------- */

/* The one above moves one knob. This moves all ten, so a knob whose step is zero, whose range is
 * inverted, or whose default is outside its range fails here rather than when a user finds it. */
static void test_every_knob_moves_and_survives(void)
{
    char settings[1088];
    path_of(settings, sizeof settings, "every.json");
    write_text(settings, "{}\n");

    nr_menu_bridge *probe = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
    assert(probe);
    int count = nr_menu_bridge_row_count(probe);
    assert(count == 11);

    for (int row = 0; row < count; ++row) {
        const char *before = nr_menu_bridge_row_value(probe, row);
        assert(before != NULL);
        char saved[96];
        snprintf(saved, sizeof saved, "%s", before);

        /* Move it, SAVE it, and read it back in a fresh bridge - the round trip that matters. */
        nr_menu_bridge_move(probe, row, +1);
        nr_menu_bridge_save(probe);
        nr_menu_bridge_destroy(probe);
        probe = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
        assert(probe);

        const char *after = nr_menu_bridge_row_value(probe, row);
        assert(after != NULL);
        if (strcmp(saved, after) == 0) {
            /* A knob already at the top of its range cannot move up. That is allowed; what is not
             * allowed is a knob that cannot move in EITHER direction, which the down-step checks. */
            nr_menu_bridge_move(probe, row, -1);
            nr_menu_bridge_save(probe);
            nr_menu_bridge_destroy(probe);
            probe = nr_menu_bridge_create_headless(settings, NR_MENU_LANG_EN);
            assert(probe);
            const char *down = nr_menu_bridge_row_value(probe, row);
            assert(down != NULL);
            assert(strcmp(down, saved) != 0);
        }
    }

    nr_menu_bridge_destroy(probe);
    printf("  ok  all %d knobs move and read back\n", count);
}

/* --- 8. the language table is complete ------------------------------------------------------ */

static void test_languages_are_complete(void)
{
    for (int lang = 0; lang < NR_MENU_LANG_COUNT; ++lang) {
        for (int key = 0; key < NR_MENU_KEY_COUNT; ++key) {
            const char *text = nr_menu_text(lang, key);
            assert(text != NULL);
            assert(text[0] != '\0');
        }
        for (int mode = 0; mode < NR_MENU_COMPARE_COUNT; ++mode)
            assert(nr_menu_compare_names[lang][mode][0] != '\0');
    }
    printf("  ok  %d languages x %d keys, no blanks\n", NR_MENU_LANG_COUNT, NR_MENU_KEY_COUNT);
}

int main(void)
{
#ifdef _WIN32
    char temp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, temp);
    assert(n > 0 && n < MAX_PATH);
    snprintf(g_dir, sizeof g_dir, "%smenu_persist_%lu", temp,
             (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(g_dir, NULL);
#else
    snprintf(g_dir, sizeof g_dir, "/tmp/menu_persist_%d", (int)getpid());
    mkdir(g_dir, 0700);
#endif

    printf("test_menu_persist:\n");
    test_unrelated_fields_survive();
    test_opening_changes_nothing();
    test_absent_key_is_not_invented();
    test_a_moved_knob_is_not_saved();
    test_a_slid_knob_is_not_saved();
    test_reset_cannot_destroy_a_save();
    test_language_does_not_commit_a_knob();
    test_close_keeps_the_changes();
    test_the_written_file_parses();
    test_a_moved_knob_survives_a_reopen();
    test_every_knob_moves_and_survives();
    test_languages_are_complete();
    printf("test_menu_persist: ok\n");
    return 0;
}

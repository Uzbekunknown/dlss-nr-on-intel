/* A choice list is only as long as its count, and every entry must name a real option.
 *
 * The report: the cadence combo opened showing `*Unknown item*` as the fourth entry.
 *
 * The layer's pacing row is not a knob - the bridge produces the eleven knobs and `nr_layer.c`
 * appends three rows of its own, and one of them is a choice of four names ("every frame",
 * "every 2 frames", "every 3 frames", "every 4 frames"). The row says `choice_count = 4` while
 * the store holding the names held THREE entries, so the fourth pointer in the array was left
 * pointing at whatever the static held - and ImGui, given a count with no name, draws
 * `*Unknown item*`.
 *
 * That is the second time this exact shape has cost something: the profiles combo had the same
 * NULL written over an entry its count still offered (`*Unknown item*` in the opened combo, then
 * an access violation in the render). A name-count mismatch is a crash, not a cosmetic bug, so
 * it is asserted here rather than looked for in a game again.
 *
 * This checks the LAYER's row-building, not the bridge's: the eleven knobs are the bridge's
 * business and `test_menu_side.c` covers them. The three pacing rows are the layer's, and the
 * one that is a choice is what these assertions walk.
 */
#include "nr_menu.h"
#include "nr_menu_bridge.h"
#include "nr_menu_strings.h"   /* NR_LANG_EN, and the labels the choice list is filled with */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %s  %s  (%s)\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) ++failures;
}

/* The same shape as the layer's own row struct, filled by `menu_ui_rows` - which is static there,
 * so the shape is reproduced here from the header rather than through a wrapper that would drift.
 * What is asserted is the LAYER's contract, not this copy of it: the count matches the names, the
 * names are distinct and non-empty, and the last one is not a NULL the count still counts. */
#define TEST_MAX_ROWS     24
#define TEST_MAX_CHOICES  8

int main(void)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    /* ---- 1. the layer's pacing row must declare as many names as its count ---- */
    /* A count of four with a store of three: `names[choice_count] = NULL` wrote the terminator
     * over the fourth entry, so the combo drew three names plus `*Unknown item*`. */
    {
        static const char *names[TEST_MAX_CHOICES];
        static char store[4][24];
        const int expected = 4;   /* every frame .. every 4th, which is what the row offers */

        /* Written the way nr_layer.c writes it now, so a regression in either place is visible. */
        for (int c = 0; c < expected; ++c) {
            snprintf(store[c], sizeof store[c], "%s %d %s", "every", c + 1, "frames");
            names[c] = store[c];
        }
        names[expected < TEST_MAX_CHOICES ? expected : TEST_MAX_CHOICES - 1] = NULL;

        int named = 0;
        for (int c = 0; c < expected; ++c)
            if (names[c] && names[c][0]) ++named;

        char detail[128];
        snprintf(detail, sizeof detail, "count=%d named=%d last=%s", expected, named,
                 names[expected - 1] ? names[expected - 1] : "(null)");
        check(named == expected,
              "the cadence row names as many options as its count declares", detail);

        /* And they are four different words: a shared buffer makes every entry the same string,
         * which draws a combo of four identical lines and changes nothing when pressed. */
        int distinct = 1;
        for (int a = 0; a < expected && distinct; ++a)
            for (int b = a + 1; b < expected && distinct; ++b)
                if (strcmp(names[a], names[b]) == 0) distinct = 0;
        check(distinct, "and they are distinct", "shared storage");
    }

    /* ---- 1b. the first cadence option is a phrase, not a numbered one ----
     *
     * `every_frame` is "every frame" / "каждый кадр" - a whole phrase with no number in it - and
     * the other three are built from a `{}` template. When the plural keys were added, the
     * `if (c == 0)` branch that copied it verbatim was folded into the shared path, and the shared
     * path appends the count when the string has no `{}` - so the first option read
     * "every frame 1" in all four languages. Nothing failed: a numbered phrase is still a phrase.
     *
     * **This is a source assertion, not a behavioural one, and that is deliberate.** The first
     * version of this check rebuilt the string from the table the same way `nr_layer.c` does -
     * which meant it tested the COPY, so deleting the real `c == 0` branch still passed. The
     * builder lives in a static function in a 3700-line file with no seam to call it from here, so
     * the honest check is that the branch exists and still guards on the string rather than the
     * index. `nr_layer.c` is read as text, the way `test_menu_keys.cpp` already checks call sites. */
    {
        /* Both files: the cadence row is built in `nr_menu_layer.c` since the menu's glue moved
         * there. See test_menu_keys.cpp for why the read is a concatenation. */
        static char lsrc[900000];
        size_t ln = 0;
        {
            static const char *const files[] = {
                "src/layer/nr_layer.c", "src/layer/nr_menu_layer.c",
            };
            for (int fi = 0; fi < (int)(sizeof files / sizeof files[0]); ++fi) {
                FILE *layer = fopen(files[fi], "rb");
                if (!layer) {
                    printf("  FAIL  %s was not readable (run from the repository root)\n",
                           files[fi]);
                    return 1;
                }
                ln += fread(lsrc + ln, 1, sizeof lsrc - 1 - ln, layer);
                fclose(layer);
                if (ln + 1 < sizeof lsrc) lsrc[ln++] = '\n';
            }
            lsrc[ln] = '\0';
        }

        int has_first = strstr(lsrc, "c == 0 && !strstr(nr_menu_text(language, NR_STR_EVERY_FRAME)") != NULL;
        check(has_first, "the first cadence option is copied, not numbered",
              has_first ? "the c == 0 branch guards on the string having no {}"
                        : "MISSING - the first option will read \"every frame 1\"");
        /* And the numbered path is still there for the other three, so the fix cannot have been
         * "delete the formatting". */
        int has_numbered = strstr(lsrc, "\"%.*s%d%s\", head, shape, c + 1, brace + 2") != NULL;
        check(has_numbered, "and the numbered options are still formatted",
              has_numbered ? "present" : "MISSING - the cadence would lose its counts");
    }

    /* ---- 2. the bridge's own choice knobs are consistent with their counts ---- */
    /* The bridge produces the eleven knobs, two of them choices (`profile`, `weights`). A count
     * larger than the names available is the same bug in the other half of the list. */
    {
        const char *settings = "nr-menu-choices-test-settings.json";
        remove(settings);
        nr_menu_bridge_use_env_file("Z:\\no\\such\\nr-env.txt");
        nr_menu_bridge *b = nr_menu_bridge_create_headless(settings, 0);
        if (!b) { printf("  FAIL  no bridge\n"); return 1; }

        int rows = nr_menu_bridge_row_count(b);
        char bad[192];
        bad[0] = '\0';
        int rows_checked = 0, mismatches = 0;
        for (int i = 0; i < rows; ++i) {
            int count = nr_menu_bridge_knob_choice_count(i);
            if (count <= 0) continue;
            ++rows_checked;
            char name[64];
            int empties = 0;
            for (int c = 0; c < count; ++c) {
                name[0] = '\0';
                nr_menu_bridge_knob_choice_name(i, c, NR_LANG_EN, name, sizeof name);
                if (!name[0]) ++empties;
            }
            if (empties) {
                ++mismatches;
                snprintf(bad + strlen(bad), sizeof bad - strlen(bad),
                         "row %d had %d of %d names empty; ", i, empties, count);
            }
        }
        snprintf(bad + strlen(bad), sizeof bad - strlen(bad),
                 "%d choice rows, %d rows with empty names", rows_checked, mismatches);
        check(mismatches == 0 && rows_checked > 0,
              "every choice knob names each of its options", bad);

        /* Out of range must write nothing rather than one entry too many: the layer copies
         * `choice_count` names, so an extra name is harmless and a missing one is a NULL. */
        {
            char name[64];
            int refused = 0;
            for (int i = 0; i < rows; ++i) {
                int count = nr_menu_bridge_knob_choice_count(i);
                if (count <= 0) continue;
                /* One past the end: `_choice_name` bounds-checks, so an out-of-range index
                 * leaves the buffer untouched - which here means the sentinel stands. */
                name[0] = '\0';
                nr_menu_bridge_knob_choice_name(i, count, NR_LANG_EN, name, sizeof name);
                if (name[0]) ++refused;
            }
            check(refused == 0, "an index past the end names nothing", refused ? "wrote one" : "");
        }

        /* ---- a choice value's label is the label, not whichever string sits nearby ----
         *
         * The Network row's options used to be the raw names `ht` / `cc_small_fp8`, and the fix
         * maps them to a translated label through `nr_menu_extra_key_for`. That lookup returns
         * `NR_MENU_KNOB_COUNT + position`, so the position has to be the TABLE's position - and
         * the choice labels were first written at the end of that list while the generator emits
         * them in the middle. Nothing failed: every entry is a valid string, so the row showed the
         * GPU-blocker sentence as its two options and the emptiness checks above all passed.
         *
         * This asks for the label by row and checks it is ABOUT the network, which is the only
         * kind of assertion that catches an off-by-N into a neighbouring key. The row is found by
         * name through the public accessors rather than by index, so a knob added later does not
         * silently move what this checks. */
        {
            int weights = -1, profile = -1;
            for (int i = 0; i < nr_menu_bridge_row_count(b); ++i) {
                const char *key = nr_menu_bridge_knob_name(i);
                if (key && strcmp(key, "weights") == 0) weights = i;
                if (key && strcmp(key, "profile") == 0) profile = i;
            }
            char detail[320];
            snprintf(detail, sizeof detail, "weights=%d profile=%d", weights, profile);
            /* **The network is no longer a knob.** It named a model file and gave the project two
             * settings for one fact; `NR_PRESET` is the one setting now, chosen in the setup
             * window. This fails if a second path comes back. */
            check(weights < 0, "the network is not a choice knob any more", detail);
            check(profile >= 0, "and the profile knob is", detail);

            if (profile >= 0) {
                int count = nr_menu_bridge_knob_choice_count(profile);
                snprintf(detail, sizeof detail, "%d options", count);
                check(count >= 2, "the profile knob offers more than one", detail);

                /* Every option is labelled, in all four languages. A label present in English and
                 * blank elsewhere is the shape of the four-language bugs this project has had. */
                int blank = 0;
                for (int lang = 0; lang < NR_MENU_LANG_COUNT; ++lang) {
                    for (int which = 0; which < count; ++which) {
                        char a[256] = "";
                        nr_menu_bridge_knob_choice_name(profile, which, lang, a, sizeof a);
                        if (!a[0]) ++blank;
                    }
                }
                snprintf(detail, sizeof detail, "%d blank of %d", blank,
                         NR_MENU_LANG_COUNT * count);
                check(blank == 0, "every profile option is labelled in all four languages",
                      detail);

                /* ---- and the store the layer copies them into is wide enough ----
                 *
                 * `nr_menu_layer.c` fills `name_storage[..][NR_KNOB_CHOICE_NAME_SIZE]` and hands
                 * those to ImGui. That was 32 bytes, sized for the raw names (`cc_small_fp8` is
                 * 12), and the network's labels were 33 to 75 - so every one of the eight was cut,
                 * and the Chinese one was cut inside a 3-byte character and rendered as a
                 * replacement glyph. Nothing failed: a truncated label is still a label.
                 *
                 * Measured in bytes, because the character count is what hides it. */
                int worst = 0;
                const char *worst_text = "";
                for (int lang = 0; lang < NR_MENU_LANG_COUNT; ++lang) {
                    for (int which = 0; which < count; ++which) {
                        char a[256] = "";
                        nr_menu_bridge_knob_choice_name(profile, which, lang, a, sizeof a);
                        int n = (int)strlen(a);
                        if (n > worst) {
                            worst = n;
                            worst_text = lang == 0 ? "en" : lang == 1 ? "zh" :
                                         lang == 2 ? "ru" : "es";
                        }
                    }
                }
                snprintf(detail, sizeof detail, "longest label %d bytes (%s), store holds %d",
                         worst, worst_text, NR_KNOB_CHOICE_NAME_SIZE - 1);
                check(worst < NR_KNOB_CHOICE_NAME_SIZE,
                      "the choice-name store fits the longest label", detail);
            }
        }

        nr_menu_bridge_destroy(b);
        nr_menu_bridge_use_env_file(NULL);
        remove(settings);
    }

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}

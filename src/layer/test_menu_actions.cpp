/* Reset, Revert and the effect checkbox: three different questions, three different answers.
 *
 *   Reset   the code's defaults, and the effect OFF. It must not touch the saved file - a
 *           keypress that destroys what the user saved is not a reset.
 *   Revert  the SAVED settings, back on screen and back in the picture. It must not WRITE the
 *           file - the file is already what it is being reverted to.
 *   the checkbox
 *           a view of the layer's trigger, whose choice goes to the file beside the knobs on the
 *           next Save, and whose Reset target is off.
 *
 * The language is deliberately outside all three: it is not a knob, and a reset that relabelled
 * the panel in another language would be a surprise.
 */
#include "nr_menu.h"
#include "nr_menu_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %s  %s  (%s)\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) ++failures;
}

static double value_at(nr_menu_bridge *b, int row)
{
    const char *text = nr_menu_bridge_row_value(b, row);
    return text ? atof(text) : -1.0;
}

static int file_says(const char *path, const char *key, char *out, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char text[8192];
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = '\0';
    fclose(f);
    char needle[96];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *at = strstr(text, needle);
    if (!at) return 0;
    const char *colon = strchr(at, ':');
    if (!colon) return 0;
    const char *p = colon + 1;
    while (*p == ' ' || *p == '\t') ++p;
    size_t i = 0;
    if (*p == '"') { ++p; while (*p && *p != '"' && i + 1 < size) out[i++] = *p++; }
    else while (*p && *p != ',' && *p != '\n' && *p != '}' && i + 1 < size) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

int main(void)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("Reset, Revert and the effect checkbox\n");

    const char *path = "nr-menu-reset-test-settings.json";
    remove(path);
    /* A saved file: render scale 0.6 (the code default is 1), Chinese, effect on. */
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  FAIL  could not write the fixture\n"); return 1; }
    fputs("{\n  \"render_scale\": 0.6,\n  \"menu_language\": 1,\n  \"nr_enabled\": 1\n}\n", f);
    fclose(f);

    nr_menu_bridge *b = nr_menu_bridge_create_headless(path, 0);
    if (!b) { printf("  FAIL  no bridge\n"); remove(path); return 1; }

    char detail[192];
    char text[64];

    /* The file is the baseline. */
    snprintf(detail, sizeof detail, "render_scale = %.2f, language = %d, nr_enabled = %d",
             value_at(b, 0), nr_menu_bridge_language(b), nr_menu_bridge_nr_saved(b));
    check(value_at(b, 0) > 0.59 && value_at(b, 0) < 0.61 && nr_menu_bridge_nr_saved(b) == 1,
          "the saved values are what the bridge starts from", detail);

    /* ---- Revert puts the SAVED values back, after a change ---- */
    {
        nr_menu_bridge_move(b, 0, +1);       /* 0.6 -> 0.65 */
        nr_menu_bridge_update(b, 1.0f / 60.0f);
        double moved = value_at(b, 0);
        snprintf(detail, sizeof detail, "moved to %.3f", moved);
        check(moved > 0.61, "a knob move takes effect first", detail);

        nr_menu_bridge_revert(b);
        nr_menu_bridge_update(b, 1.0f / 60.0f);
        double back = value_at(b, 0);
        snprintf(detail, sizeof detail, "reverted to %.3f (saved 0.600)", back);
        check(back > 0.59 && back < 0.61, "Revert restores the SAVED value", detail);

        /* And the language is still the file's. It is not a knob and no reset touches it. */
        snprintf(detail, sizeof detail, "language = %d", nr_menu_bridge_language(b));
        check(nr_menu_bridge_language(b) == 1, "Revert leaves the language alone", detail);
    }

    /* ---- Reset gives the DEFAULTS, not the saved values ---- */
    {
        nr_menu_bridge_reset(b);
        nr_menu_bridge_update(b, 1.0f / 60.0f);
        double at = value_at(b, 0);
        snprintf(detail, sizeof detail, "after Reset render_scale = %.3f (default 1.000)", at);
        check(at > 0.99, "Reset restores the code default, not the saved value", detail);

        /* The saved FILE must be untouched: a reset that rewrote it would destroy what the user
         * saved, which is the thing Reset exists to be safe around. */
        int found = file_says(path, "render_scale", text, sizeof text);
        snprintf(detail, sizeof detail, "file still says %s", found ? text : "(missing)");
        check(found && atof(text) > 0.59 && atof(text) < 0.61,
              "Reset did not touch the saved file", detail);

        /* The effect's reset target is off. */
        snprintf(detail, sizeof detail, "nr_enabled_saved = %d", nr_menu_bridge_nr_saved(b));
        check(nr_menu_bridge_nr_saved(b) == 0, "Reset turns the effect's saved state off", detail);

        /* And the language survives it. */
        snprintf(detail, sizeof detail, "language = %d", nr_menu_bridge_language(b));
        check(nr_menu_bridge_language(b) == 1, "Reset leaves the language alone", detail);
    }

    /* ---- the layer keeps nr_enabled_saved honest against the trigger ----
     *
     * The trigger is the only truth about whether the effect runs: the Home key, the panel's
     * checkbox and the setup window's Enable NR button all move it. But a later Save writes
     * `nr_enabled_saved`, and only the first two told the bridge about their change - the window
     * went through the trigger alone, so a Save afterwards wrote "nr_enabled": 0 with NR running,
     * which is what was seen in MK11 and DoA5.
     *
     * The fix is in `nr_layer.c`'s per-frame path, which this fixture cannot drive - the bridge
     * here has no trigger file and no layer. So this reads the source the way
     * `test_menu_keys.cpp` does: the check is that the sync exists, and that it compares against
     * the live state rather than assuming. */
    {
        /* Both files: the menu's glue moved to `nr_menu_layer.c`. See test_menu_keys.cpp. */
        static char lsource[900000];
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
                ln += fread(lsource + ln, 1, sizeof lsource - 1 - ln, layer);
                fclose(layer);
                if (ln + 1 < sizeof lsource) lsource[ln++] = '\n';
            }
            lsource[ln] = '\0';
        }
        int syncs = strstr(lsource, "nr_menu_bridge_nr_saved(data->menu_bridge) ? 1 : 0) != live")
                    != NULL;
        check(syncs, "the layer syncs nr_enabled_saved against the live trigger",
              syncs ? "compared every frame"
                    : "MISSING - the window's Enable NR writes the opposite of the screen");
        /* And it is gated on there being a trigger path, so an install without one is not polled
         * every frame for a file that does not exist. */
        int gated = strstr(lsource, "if (trigger_path && *trigger_path && data->menu_bridge)") != NULL;
        check(gated, "and only when this install has a trigger path",
              gated ? "guarded" : "MISSING - polled every frame regardless");
    }

    /* ---- Save writes the effect's state beside the knobs ---- */
    {
        nr_menu_bridge_set_nr_saved(b, 1);
        int saved = nr_menu_bridge_save(b);
        snprintf(detail, sizeof detail, "save returned %d", saved);
        check(saved == 1, "Save succeeds", detail);

        int found = file_says(path, "nr_enabled", text, sizeof text);
        snprintf(detail, sizeof detail, "nr_enabled = %s", found ? text : "(missing)");
        check(found && atoi(text) == 1, "Save writes nr_enabled", detail);

        /* One key, not two: the substitute-in-place path and the append path must not both run. */
        FILE *g = fopen(path, "rb");
        int occurrences = 0;
        if (g) {
            static char all[8192];
            size_t n = fread(all, 1, sizeof all - 1, g);
            all[n] = '\0';
            fclose(g);
            for (const char *p = all; (p = strstr(p, "\"nr_enabled\"")) != NULL; ++p) ++occurrences;
        }
        snprintf(detail, sizeof detail, "%d occurrence(s)", occurrences);
        check(occurrences == 1, "and exactly once", detail);

        /* The language was not overwritten by any of it. */
        found = file_says(path, "menu_language", text, sizeof text);
        snprintf(detail, sizeof detail, "menu_language = %s", found ? text : "(missing)");
        check(found && atoi(text) == 1, "and the language is still the one that was there", detail);
    }

    /* ---- a fresh start with nr_enabled: 0 does not run the effect ---- */
    {
        nr_menu_bridge_destroy(b);
        f = fopen(path, "wb");
        if (!f) { printf("  FAIL  could not rewrite the fixture\n"); return 1; }
        fputs("{\n  \"render_scale\": 0.6,\n  \"nr_enabled\": 0\n}\n", f);
        fclose(f);
        b = nr_menu_bridge_create_headless(path, 0);
        if (!b) { printf("  FAIL  no bridge on the second read\n"); return 1; }
        snprintf(detail, sizeof detail, "nr_enabled_saved = %d", nr_menu_bridge_nr_saved(b));
        check(nr_menu_bridge_nr_saved(b) == 0, "a file that says 0 starts with the effect off", detail);

        /* An install that predates the key: absent means off, not "on". */
        nr_menu_bridge_destroy(b);
        f = fopen(path, "wb");
        if (!f) { printf("  FAIL  could not rewrite the fixture\n"); return 1; }
        fputs("{\n  \"render_scale\": 0.6\n}\n", f);
        fclose(f);
        b = nr_menu_bridge_create_headless(path, 0);
        if (!b) { printf("  FAIL  no bridge on the third read\n"); return 1; }
        snprintf(detail, sizeof detail, "nr_enabled_saved = %d", nr_menu_bridge_nr_saved(b));
        check(nr_menu_bridge_nr_saved(b) == 0, "a file without the key starts with the effect off",
              detail);
    }

    nr_menu_bridge_destroy(b);
    remove(path);
    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}

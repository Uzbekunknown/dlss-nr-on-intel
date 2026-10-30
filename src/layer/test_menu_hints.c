/* Does the bridge hand the overlay the right hint for each knob, in each language?
 *
 * The overlay's question-mark tooltip reads `nr_menu_bridge_row_hint`, which indexes the generated
 * `nr_menu_hints[language][knob]`. The array is positional, so a reordering of the knob table or a
 * language index off by one would silently pair every knob with the wrong explanation - and a
 * tooltip that confidently describes the wrong control is worse than none.
 *
 * No Vulkan and no game: the bridge runs headless, which is how the other persistence cases work.
 */
#include "nr_menu_bridge.h"
#include "nr_menu.h"
#include "nr_menu_strings.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(name, cond)                                              \
    do {                                                               \
        printf("  %s  %s\n", (cond) ? "PASS" : "FAIL", name);          \
        if (!(cond)) ++fails;                                          \
    } while (0)

int main(void)
{
    printf("test_menu_hints: is each knob explained, in every language?\n\n");

    nr_menu_bridge *b = nr_menu_bridge_create_headless("", NR_MENU_LANG_EN);
    assert(b);

    const int knobs = nr_menu_bridge_knob_count();
    CHECK("the table has every knob", knobs == 11);

    /* Every knob, every language: a hint that exists and is not blank. A knob with no explanation
     * is a tooltip that opens empty, which reads as a broken panel. */
    int missing = 0, blank = 0;
    for (int lang = 0; lang < NR_MENU_LANG_COUNT; ++lang) {
        for (int row = 0; row < knobs; ++row) {
            const char *h = nr_menu_bridge_row_hint(b, row, lang);
            if (!h) { ++missing; continue; }
            if (!h[0]) { ++blank; continue; }
        }
    }
    printf("     missing=%d blank=%d over %d knobs x %d languages\n",
           missing, blank, knobs, NR_MENU_LANG_COUNT);
    CHECK("every knob has a hint in every language", missing == 0 && blank == 0);

    /* The pairing. `render_scale` is about the fraction of each dimension; `hold` is about keeping
     * the previous result where the pixels did not change. If the array were off by one these would
     * come back as the neighbouring knob's text, which is what this catches. */
    const char *scale = nr_menu_bridge_row_hint(b, 0 /* render_scale */, NR_LANG_EN);
    const char *hold  = nr_menu_bridge_row_hint(b, 7 /* hold */, NR_LANG_EN);
    printf("     render_scale: %.60s\n", scale ? scale : "(null)");
    printf("     hold        : %.60s\n", hold ? hold : "(null)");
    CHECK("render_scale is explained as the fraction",
          scale && strstr(scale, "Fraction of each dimension") != NULL);
    CHECK("hold is explained as keeping unchanged pixels",
          hold && strstr(hold, "did not change") != NULL);

    /* The languages really differ: a lookup that ignored the language would return the English text
     * for all four, which is the failure a "not blank" check cannot see. */
    int identical = 0;
    for (int row = 0; row < knobs; ++row) {
        const char *en = nr_menu_bridge_row_hint(b, row, NR_LANG_EN);
        const char *zh = nr_menu_bridge_row_hint(b, row, NR_LANG_ZH);
        const char *ru = nr_menu_bridge_row_hint(b, row, NR_LANG_RU);
        const char *es = nr_menu_bridge_row_hint(b, row, NR_LANG_ES);
        if (en && zh && strcmp(en, zh) == 0) ++identical;
        if (en && ru && strcmp(en, ru) == 0) ++identical;
        if (en && es && strcmp(en, es) == 0) ++identical;
    }
    CHECK("no language falls back to English for any knob", identical == 0);

    /* Chinese must actually be Chinese, and Russian Cyrillic: a table copied from the wrong column
     * would pass every check above. */
    {
        int zh_cjk = 0, ru_cyr = 0;
        for (int row = 0; row < knobs; ++row) {
            const char *zh = nr_menu_bridge_row_hint(b, row, NR_LANG_ZH);
            const char *ru = nr_menu_bridge_row_hint(b, row, NR_LANG_RU);
            for (const char *p = zh; p && *p; ++p) {
                unsigned char c = (unsigned char)*p;
                if (c >= 0xE4 && c <= 0xE9) { zh_cjk++; break; }
            }
            for (const char *p = ru; p && *p; ++p) {
                unsigned char c = (unsigned char)*p;
                if (c == 0xD0 || c == 0xD1) { ru_cyr++; break; }
            }
        }
        printf("     zh hints containing CJK: %d/%d, ru containing Cyrillic: %d/%d\n",
               zh_cjk, knobs, ru_cyr, knobs);
        CHECK("the Chinese hints are Chinese", zh_cjk == knobs);
        CHECK("the Russian hints are Cyrillic", ru_cyr == knobs);
    }

    /* Out of range is NULL rather than a wild read: the overlay asks for a row index it computed. */
    CHECK("a row past the end returns NULL", nr_menu_bridge_row_hint(b, knobs, NR_LANG_EN) == NULL);
    CHECK("a negative row returns NULL", nr_menu_bridge_row_hint(b, -1, NR_LANG_EN) == NULL);

    nr_menu_bridge_destroy(b);
    printf("\n%s\n", fails ? "FAILED" : "ok");
    return fails;
}

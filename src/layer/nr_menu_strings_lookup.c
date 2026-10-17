/* Looking up a string, and mapping a knob name to its row. Hand-written; the tables it reads are
 * generated - see `nr_menu_strings.h`.
 *
 * Why this is a separate file from the generated data: the tables are written by
 * `tools/gen_menu_strings.py` and would lose any hand edit on the next run. The two functions here
 * are logic, not data, so they live where an edit stays.
 */
#include "nr_menu_strings.h"

#include <string.h>

const char *nr_menu_text(int language, int key)
{
    if (language < 0 || language >= NR_MENU_LANG_COUNT)
        language = NR_LANG_EN;
    if (key < 0 || key >= NR_MENU_KEY_COUNT)
        return "";

    const char *text = nr_menu_strings[language][key];
    if (text && *text)
        return text;

    /* A missing translation shows English rather than a blank. A blank row is one nobody can
     * identify, which is worse than a word in the wrong language - and the generator refuses to
     * write a table with a hole in it, so this is the second line of defence rather than the
     * first. */
    text = nr_menu_strings[NR_LANG_EN][key];
    return text ? text : "";
}

/* The knob names, in the order the tables list them. This is the one place C spells them, and it
 * is spelled to match `nr_knobs.py` exactly - the generator asserts that the two agree, so a knob
 * renamed there fails the build of the tables rather than silently vanishing from the menu. */
static const char *const knob_names[] = {
    "render_scale", "min_extent", "profile", "intensity", "detail_strength",
    "colour_strength", "temporal", "hold", "release", "cut_limit", "weights",
};

int nr_menu_key_for(const char *knob_name)
{
    if (!knob_name)
        return -1;
    int count = (int)(sizeof knob_names / sizeof knob_names[0]);
    for (int i = 0; i < count && i < NR_MENU_KEY_COUNT; ++i)
        if (strcmp(knob_names[i], knob_name) == 0)
            return i;
    return -1;
}

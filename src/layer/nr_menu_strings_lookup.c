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

/* The same lookup for the keys the knob names do not cover.
 *
 * `nr_menu_key_for` answers "which row is this KNOB" and its table is the first eleven keys, in
 * `nr_knobs.py`'s order - the row index is what the caller wants there. Every other key is further
 * down the table and needs its own list, because the two answer different questions: one is a row,
 * the other is a string among many.
 *
 * **The order here IS the table's order**, and it has to be: the return value is
 * `NR_MENU_KNOB_COUNT + position`, so a name in the wrong place returns the index of a different
 * string. That is not theoretical - the choice labels were first written at the end of this list
 * while the generator emits them in the middle, and the panel showed the GPU-blocker sentence as
 * the Network row's options. The build does not catch it: every entry is a valid string, so
 * nothing is missing, only wrong. `test_menu_pacing.cpp` walks the table and would catch a blank;
 * only a test that asks for a specific key by name catches this, which is what the choice-label
 * check in `test_menu_choices.cpp` now does.
 *
 * Hand-written for the same reason the other two functions are: the tables are generated and would
 * lose an edit, and this is logic. */
static const char *const extra_names[] = {
    "title", "saved", "reset", "hint", "toggle_nr", "compare", "compare_split", "compare_zoom",
    "language", "shortcut", "on", "off", "async", "async_hint", "gpu_image", "gpu_image_hint",
    "every", "every_hint", "every_frame", "every_nth_few", "every_nth_many",
    "choice_weights_ht", "choice_weights_cc_small_fp8",
    "gpu_blocked_both", "gpu_blocked_detail", "gpu_blocked_colour",
};

int nr_menu_extra_key_for(const char *name)
{
    if (!name)
        return -1;
    int count = (int)(sizeof extra_names / sizeof extra_names[0]);
    for (int i = 0; i < count; ++i)
        if (strcmp(extra_names[i], name) == 0)
            return NR_MENU_KNOB_COUNT + i;   /* the keys after the knobs, in the table's order */
    return -1;
}

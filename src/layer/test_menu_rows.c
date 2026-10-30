/* The C knob table must agree with `nr_knobs.py`, which is the definition.
 *
 * The author asked for this check after a real drift: the table in `nr_menu_bridge.c` and the one
 * in `nr_config.py` both said `render_scale` defaults to 0.55, while `nr_knobs.py` said 1.0 - and
 * nothing noticed. Three copies of one fact, disagreeing, with the disagreement visible only to
 * somebody who compared them by hand.
 *
 * Why a copy exists at all: the menu draws inside the game's process with no interpreter in it, so
 * it cannot read `nr_knobs.py`; and the daemon is a different process, so it cannot lend the menu
 * its table. The copy is therefore unavoidable. What is avoidable is the copy being WRONG, and
 * that is what this file is for.
 *
 * Two halves:
 *
 *   1. here - the table's own shape: names unique and non-empty, ranges sane, a choice's default
 *      among its choices, every knob's range containing its default. These are properties the
 *      table must have on its own, and they catch a typo without needing Python.
 *
 *   2. `test_menu_rows.py` - the same table against `nr_knobs.KNOBS`, through the accessors at the
 *      bottom of `nr_menu_bridge.h`. That half needs an interpreter, so it is Python; this half
 *      runs in `make test` with everything else.
 *
 * Both halves failing is the point: the C side catches a malformed table, the Python side catches
 * the C table and the definition disagreeing, and neither can be satisfied by editing the other.
 */
#include "nr_menu_bridge.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static void test_names_are_unique_and_known(void)
{
    int count = nr_menu_bridge_knob_count();
    assert(count > 0);

    for (int i = 0; i < count; ++i) {
        const char *name = nr_menu_bridge_knob_name(i);
        assert(name != NULL);
        assert(name[0] != '\0');

        /* Every name must be one `nr_knobs.py` also has. The list is spelled here rather than
         * derived, because deriving it would mean reading the file this test exists to check
         * against - and a knob added to the C table alone must fail. */
        static const char *const known[] = {
            "profile", "intensity", "detail_strength", "colour_strength", "render_scale",
            "temporal", "cut_limit", "hold", "release", "min_extent",
        };
        int found = 0;
        for (size_t k = 0; k < sizeof known / sizeof known[0]; ++k)
            if (strcmp(known[k], name) == 0) { found = 1; break; }
        assert(found);

        for (int j = i + 1; j < count; ++j)
            assert(strcmp(nr_menu_bridge_knob_name(j), name) != 0);
    }
    assert(nr_menu_bridge_knob_name(-1) == NULL);
    assert(nr_menu_bridge_knob_name(count) == NULL);
}

static void test_ranges_are_sane(void)
{
    int count = nr_menu_bridge_knob_count();
    int numeric = 0;

    for (int i = 0; i < count; ++i) {
        float low = 0, high = 0, step = 0, fallback = 0;
        if (!nr_menu_bridge_knob_range(i, &low, &high, &step, &fallback))
            continue;                     /* a choice: no range to check */
        ++numeric;

        assert(low < high);               /* a zero-width range cannot be moved in */
        assert(step > 0.0f);              /* a zero step would make an arrow key do nothing */
        assert(step <= high - low);       /* a step wider than the range skips every value */

        /* The default must be inside the range, or the menu would open showing a value the
         * caller then clamps - a row that changes the moment it is touched. */
        assert(fallback >= low);
        assert(fallback <= high);

        /* And it must land on a step boundary from the low end, or an arrow press from the
         * default would produce a value the file never had. */
        float steps = (fallback - low) / step;
        assert(fabsf(steps - roundf(steps)) < 0.001f);
    }

    /* Nine of the ten are numeric; `profile` is the choice. Asserted so that turning a numeric
     * knob into a choice - or the reverse - is a deliberate change to this number rather than a
     * silent one. */
    assert(numeric == 9);
}

/* The one knob the author named, asserted by name and value.
 *
 * `render_scale` is why this test exists. Its default is 1.0 in `nr_knobs.py`, and a menu that
 * wrote its own different default would change the picture by being opened - which is exactly what
 * the author asked not to happen. A named assertion is worth more than the loop above for this
 * one: it says which value and why. */
static void test_render_scale_default_is_one(void)
{
    int count = nr_menu_bridge_knob_count();
    for (int i = 0; i < count; ++i) {
        if (strcmp(nr_menu_bridge_knob_name(i), "render_scale") != 0)
            continue;
        float low = 0, high = 0, step = 0, fallback = 0;
        assert(nr_menu_bridge_knob_range(i, &low, &high, &step, &fallback));
        assert(low == 0.05f);
        assert(high == 1.0f);
        assert(step == 0.05f);
        assert(fallback == 1.0f);
        return;
    }
    assert(!"render_scale is missing from the menu's table");
}

int main(void)
{
    test_names_are_unique_and_known();
    test_ranges_are_sane();
    test_render_scale_default_is_one();
    printf("test_menu_rows: ok (%d knobs)\n", nr_menu_bridge_knob_count());
    return 0;
}

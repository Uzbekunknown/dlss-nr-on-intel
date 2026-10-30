/* The menu's strings, in four languages. GENERATED - do not edit by hand.
 *
 * Written by `tools/gen_menu_strings.py` from two sources:
 *   en, ru, es  `dist-tools/windows-wizard.ps1`, which already ships them for every knob.
 *               Extracted rather than retyped, because the wizard's keys are
 *               `nr_knobs.py`'s names - so the tables join without a mapping and a knob
 *               added there brings its label here.
 *   zh          written in the generator. The wizard has no Chinese, so this is the one
 *               column that is new work.
 *
 * Strings are UTF-8 bytes, octal-escaped, so the literal means the same thing whatever
 * codepage the compiler reads the file in. The menu draws them through the atlas, which
 * is built from these codepoints at start-up - see nr_menu_font.c.
 */
#ifndef NR_MENU_STRINGS_H
#define NR_MENU_STRINGS_H

#define NR_MENU_LANG_COUNT 4
#define NR_MENU_KEY_COUNT 37
#define NR_MENU_COMPARE_COUNT 3
/* The hints are one per KNOB, which is fewer than the keys: a key like `title` or `on` is
 * a string the menu shows, not a knob it explains. Declaring them with the key count left
 * the tail of each row zero-initialised, and the atlas builder walked into those NULLs -
 * which cost the menu its Chinese and Russian glyphs and faulted on the first real game. */
#define NR_MENU_KNOB_COUNT 11
enum { NR_LANG_EN = 0, NR_LANG_ZH, NR_LANG_RU, NR_LANG_ES };

/* C linkage, like nr_menu.h and nr_menu_bridge.h.
 *
 * The tables are defined in a .c file and the menu's own UI is C++. Without this guard a
 * C++ caller looks for a mangled name (`?nr_menu_text@@YAPEBDHH@Z`) that nothing defines, so
 * the link fails only once something in C++ calls into this table - which is what happened
 * to the first fixture that did, after these strings had been used from C alone. */
#ifdef __cplusplus
extern "C" {
#endif

enum {
    NR_STR_RENDER_SCALE = 0,
    NR_STR_MIN_EXTENT = 1,
    NR_STR_PROFILE = 2,
    NR_STR_INTENSITY = 3,
    NR_STR_DETAIL_STRENGTH = 4,
    NR_STR_COLOUR_STRENGTH = 5,
    NR_STR_TEMPORAL = 6,
    NR_STR_HOLD = 7,
    NR_STR_RELEASE = 8,
    NR_STR_CUT_LIMIT = 9,
    NR_STR_WEIGHTS = 10,
    NR_STR_TITLE = 11,
    NR_STR_SAVED = 12,
    NR_STR_RESET = 13,
    NR_STR_HINT = 14,
    NR_STR_TOGGLE_NR = 15,
    NR_STR_COMPARE = 16,
    NR_STR_COMPARE_SPLIT = 17,
    NR_STR_COMPARE_ZOOM = 18,
    NR_STR_LANGUAGE = 19,
    NR_STR_SHORTCUT = 20,
    NR_STR_ON = 21,
    NR_STR_OFF = 22,
    NR_STR_ASYNC = 23,
    NR_STR_ASYNC_HINT = 24,
    NR_STR_GPU_IMAGE = 25,
    NR_STR_GPU_IMAGE_HINT = 26,
    NR_STR_EVERY = 27,
    NR_STR_EVERY_HINT = 28,
    NR_STR_EVERY_FRAME = 29,
    NR_STR_EVERY_NTH_FEW = 30,
    NR_STR_EVERY_NTH_MANY = 31,
    NR_STR_CHOICE_WEIGHTS_HT = 32,
    NR_STR_CHOICE_WEIGHTS_CC_SMALL_FP8 = 33,
    NR_STR_GPU_BLOCKED_BOTH = 34,
    NR_STR_GPU_BLOCKED_DETAIL = 35,
    NR_STR_GPU_BLOCKED_COLOUR = 36,
};

/* [language][key] */
extern const char *const nr_menu_strings[NR_MENU_LANG_COUNT][NR_MENU_KEY_COUNT];

/* [language][mode] - the compare mode names, cycled by the compare key. */
extern const char *const nr_menu_compare_names[NR_MENU_LANG_COUNT][NR_MENU_COMPARE_COUNT];

/* [language][knob] - the one-line hint for a knob, indexed as nr_knobs.py lists them. */
extern const char *const nr_menu_hints[NR_MENU_LANG_COUNT][NR_MENU_KNOB_COUNT];

/* Implemented in nr_menu_strings_lookup.c, which is hand-written: these are logic, not
 * data, and would be lost on the next run of the generator. */

/* The string for one key in one language. Falls back to English, then to an empty string,
 * so a hole in a table is a word in the wrong language rather than a blank row. */
const char *nr_menu_text(int language, int key);

/* Which key index a knob name maps to, or -1. The names are nr_knobs.py's, so the knob
 * list joins to this table without a mapping that could itself drift. */
int nr_menu_key_for(const char *knob_name);

#ifdef __cplusplus
}
#endif

#endif /* NR_MENU_STRINGS_H */

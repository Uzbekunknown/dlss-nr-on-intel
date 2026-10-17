/* What the menu shows, and what a keypress means.
 *
 * The menu draws rows of text and reports which row moved; it does not know what a knob is. This
 * file is the other half: it reads the settings file the daemon already writes, turns it into
 * rows, and turns a step into a write back to that same file.
 *
 * **Why it reads the file rather than asking the daemon.** The menu runs inside the game's process
 * - it is the layer's DLL - and the daemon is a different process behind a socket that carries
 * frames. Sending "row 3 moved up" over that socket would put settings behind the frame protocol,
 * so the menu would stop responding exactly when the daemon is busy - which is when somebody opens
 * a settings menu. The settings file is the interface the tools, the daemon and the layer already
 * share; this reads and writes the same file.
 *
 * **What it must preserve.** Three rules, each from the author's review:
 *
 *   1. *Live JSON at `NR_SETTINGS`, not somewhere new.* The caller passes the same path the layer
 *      already resolves to spawn the daemon.
 *   2. *Unrelated fields survive.* The daemon's `Settings` accepts ten knobs today and may accept
 *      more, and the tools write the same file. A menu that rewrote the object with only its own
 *      keys would delete anything else, so the write rebuilds the object it found and substitutes
 *      only the values it owns.
 *   3. *Opening or saving must not change a value.* The defaults here are used only when a key is
 *      ABSENT from the JSON, and an absent key is not written back until the user moves it. That
 *      is the bug the author called out: a menu whose own default disagreed with the release
 *      preset would "reset render scale to 1.0" just by being opened.
 */
#include "nr_menu_bridge.h"
#include "nr_menu.h"
#include "nr_menu_strings.h"
#include "nr_file.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>   /* the JSON writer below is variadic */
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>   /* getpid, for the preview's owner stamp */
#endif

/* The most keys a settings object may have, and the longest value. The file is written by this
 * project's own tools; these bounds exist so a corrupt file cannot run the walk off the end. */
#define NR_SETTINGS_MAX_KEYS 64
#define NR_VALUE_MAX 128

/* ------------------------------------------------------------------ a flat JSON object */

/* The settings file is a flat object of numbers and short strings. A general JSON parser would be
 * a file to audit; this reads the one shape and refuses anything else, which is the honest
 * behaviour for a reader that is not a parser.
 *
 * `nr_settings_parse` fills two parallel arrays of keys and raw values - the value kept as the
 * exact text it had, so a number is written back with its own precision and a string with its own
 * spelling. That is what makes "preserve unrelated fields" a copy rather than a re-format: nothing
 * this file does not own is ever turned into a value and back again. */
typedef struct {
    char key[NR_SETTINGS_MAX_KEYS][64];
    char value[NR_SETTINGS_MAX_KEYS][NR_VALUE_MAX];
    int  quoted[NR_SETTINGS_MAX_KEYS];   /* the value was a JSON string */
    int  count;
} nr_settings;

static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

/* Parse a flat object. Returns 1 when the text was an object this function could walk. */
static int nr_settings_parse(const char *text, nr_settings *out)
{
    out->count = 0;
    const char *p = skip_space(text);
    if (*p != '{') return 0;
    ++p;

    for (;;) {
        p = skip_space(p);
        if (*p == '}' || *p == '\0') return 1;
        if (*p == ',') { ++p; continue; }
        if (*p != '"') return 0;              /* not a flat object of string keys */

        /* The key. */
        char key[64];
        size_t n = 0;
        ++p;
        while (*p && *p != '"' && n + 1 < sizeof key) {
            if (*p == '\\' && p[1]) ++p;
            key[n++] = *p++;
        }
        key[n] = '\0';
        if (*p != '"') return 0;
        ++p;

        p = skip_space(p);
        if (*p != ':') return 0;
        ++p;
        p = skip_space(p);

        /* The value, kept as text. */
        char value[NR_VALUE_MAX];
        size_t v = 0;
        int quoted = 0;
        if (*p == '"') {
            quoted = 1;
            ++p;
            while (*p && *p != '"' && v + 1 < sizeof value) {
                if (*p == '\\' && p[1]) ++p;
                value[v++] = *p++;
            }
            if (*p == '"') ++p;
        } else if (*p == '{' || *p == '[') {
            /* A nested object or array: not a shape this file uses, and not one it should silently
             * flatten. Refuse the whole parse so the caller leaves the file alone. */
            return 0;
        } else {
            while (*p && *p != ',' && *p != '}' && v + 1 < sizeof value)
                value[v++] = *p++;
            while (v > 0 && (value[v - 1] == ' ' || value[v - 1] == '\n' || value[v - 1] == '\r'))
                --v;
        }
        value[v] = '\0';

        if (out->count < NR_SETTINGS_MAX_KEYS) {
            snprintf(out->key[out->count], sizeof out->key[0], "%s", key);
            snprintf(out->value[out->count], sizeof out->value[0], "%s", value);
            out->quoted[out->count] = quoted;
            ++out->count;
        }

        p = skip_space(p);
        if (*p == ',') { ++p; continue; }
        if (*p == '}') return 1;
    }
}

static int nr_settings_find(const nr_settings *s, const char *key)
{
    for (int i = 0; i < s->count; ++i)
        if (strcmp(s->key[i], key) == 0) return i;
    return -1;
}

/* ------------------------------------------------------------------ the knob table */

/* The knobs, as the menu needs them: a name, a range, a step. The labels come from the generated
 * table, so there is no second list of them.
 *
 * THE RANGES AND STEPS HERE ARE NOT AUTHORITATIVE - `nr_knobs.py` is. They are mirrored because C
 * cannot import it and the menu has to move a value without an interpreter. `test_menu_rows.py`
 * compares this table against `nr_knobs.KNOBS` and fails when the two disagree, which is the guard
 * that was missing when the copies first drifted: `render_scale` defaulted to 0.55 in two of them
 * and 1.0 in `nr_knobs.py`, and nothing said so.
 *
 * `default_value` is used ONLY when the key is absent from the settings file. */
typedef struct {
    const char *key;
    int         kind;          /* 0 number, 1 choice */
    float       low, high, step, default_value;
    const char *choices;       /* comma separated, for a choice */
} nr_knob_def;

/* In `nr_knobs.py`'s order - the order the menu shows, and the order the generated tables use. */
static const nr_knob_def knobs[] = {
    { "render_scale",    0, 0.05f, 1.0f,   0.05f, 1.0f,  NULL },
    { "min_extent",      0, 128.0f, 320.0f, 64.0f, 320.0f, NULL },
    /* Three profiles, so the range is 0..2 - it was 0..3 while `neutral` was a fourth.
     *
     * `neutral` was `standard` with tone and structure at zero, which is the same thing the
     * `intensity` knob already says ("0 is the game's frame untouched") by another route: two
     * controls for one outcome, one of which looked like it did nothing. Removed from the list
     * everywhere it is written down, which is five files plus this one. */
    { "profile",         1, 0.0f, 2.0f,     1.0f,  0.0f,  "standard,natural,cinematic" },
    { "intensity",       0, 0.0f, 2.0f,     0.05f, 1.0f,  NULL },
    { "detail_strength", 0, 0.0f, 2.0f,     0.05f, 1.0f,  NULL },
    { "colour_strength", 0, 0.0f, 2.0f,     0.05f, 1.0f,  NULL },
    { "temporal",        0, 0.0f, 1.0f,     0.05f, 1.0f,  NULL },
    { "hold",            0, 0.0f, 1.0f,     0.05f, 1.0f,  NULL },
    { "release",         0, 0.0f, 64.0f,    1.0f,  24.0f, NULL },
    { "cut_limit",       0, 0.0f, 1.0f,     0.01f, 0.15f, NULL },
    /* Which weight model the daemon runs. The choices are `nr_knobs.py`'s CHOICES entry; the
     * daemon resolves the name against what the install actually carries and keeps the running
     * model when the name cannot be honoured, so the list here is what MAY be chosen, not what
     * this machine has. */
    { "weights",         1, 0.0f, 1.0f,     1.0f,  0.0f,  "ht,cc_small_fp8" },
};

#define NR_KNOBS_COUNT ((int)(sizeof knobs / sizeof knobs[0]))

/* ------------------------------------------------------------------ the state */

struct nr_menu_bridge {
    nr_menu *menu;
    char settings_path[1088];

    float values[NR_KNOBS_COUNT];
    int   moved[NR_KNOBS_COUNT];       /* the user changed it: only these are written back */
    char  choice[NR_KNOBS_COUNT][32];
    char  shown[NR_KNOBS_COUNT][96];
    nr_menu_row rows[NR_KNOBS_COUNT];

    int language;
    /* A language change asked for on a bridge with no menu, applied by the next update. -1 is
     * "nothing pending"; the real values are the NR_MENU_LANG_* indices, so 0 is a language and
     * cannot be the sentinel. */
    int language_pending;
    /* Whether a Save has confirmed what the live preview currently shows. 0 from the moment a knob
     * moves; 1 once Save has written the same values to the settings file. A preview left at 0 is
     * the user mid-edit, and is what the daemon must not apply on a later start. */
    int preview_confirmed;
    int dirty;
    long long stamp;
};

/* ------------------------------------------------------------------ label lookup */

static const char *label_for(const nr_knob_def *knob, int language)
{
    int key = nr_menu_key_for(knob->key);
    if (key < 0) return knob->key;
    return nr_menu_text(language, key);
}

static const char *hint_for(const nr_knob_def *knob, int language)
{
    int key = nr_menu_key_for(knob->key);
    if (key < 0 || key >= NR_MENU_KNOB_COUNT) return NULL;
    const char *hint = nr_menu_hints[language][key];
    if (hint && *hint) return hint;
    return nr_menu_hints[NR_LANG_EN][key];
}

/* ------------------------------------------------------------------ choices */

static int choice_index(const nr_knob_def *k, const char *name)
{
    if (!k->choices || !name || !*name) return -1;
    const char *p = k->choices;
    int index = 0;
    size_t n = strlen(name);
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == n && strncmp(p, name, len) == 0) return index;
        if (!comma) break;
        p = comma + 1;
        ++index;
    }
    return -1;
}

/* The name of the nth choice, written into `out`. Wraps, so a corrupt index still names a
 * profile rather than being empty. */
static void choice_name(const nr_knob_def *k, int index, char *out, size_t size)
{
    int count = 0;
    for (const char *p = k->choices ? k->choices : ""; *p; ++p) if (*p == ',') ++count;
    ++count;                                    /* no choices string means exactly one */
    if (index < 0) index = 0;
    index %= count;

    const char *p = k->choices ? k->choices : "";
    int i = 0;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (i == index) {
            if (len >= size) len = size - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            return;
        }
        if (!comma) break;
        p = comma + 1;
        ++i;
    }
    out[0] = '\0';
}

static int choice_count(const nr_knob_def *k)
{
    if (!k->choices) return 1;
    int count = 1;
    for (const char *p = k->choices; *p; ++p) if (*p == ',') ++count;
    return count;
}

static void format_row(nr_menu_bridge *b, int i)
{
    const nr_knob_def *k = &knobs[i];
    if (k->kind == 1) {
        if (!b->choice[i][0]) choice_name(k, 0, b->choice[i], sizeof b->choice[i]);
        snprintf(b->shown[i], sizeof b->shown[i], "%s", b->choice[i]);
    } else {
        snprintf(b->shown[i], sizeof b->shown[i], "%g", (double)b->values[i]);
    }
}

/* ------------------------------------------------------------------ reading */

static void load_defaults(nr_menu_bridge *b)
{
    for (int i = 0; i < NR_KNOBS_COUNT; ++i) {
        const nr_knob_def *k = &knobs[i];
        b->moved[i] = 0;
        if (k->kind == 1) {
            choice_name(k, 0, b->choice[i], sizeof b->choice[i]);
            b->values[i] = 0.0f;
        } else {
            b->values[i] = k->default_value;
        }
        format_row(b, i);
    }
}

static int load_settings(nr_menu_bridge *b)
{
    load_defaults(b);

    /* A file that is not there is not an error: the menu then shows the defaults and writes
     * nothing until the user moves something. Creating the file on open would be a menu that
     * changed the daemon's behaviour by being looked at. */
    char *text = NULL;
    size_t size = 0;
    if (!nr_read_file(b->settings_path, &text, &size)) {
        b->stamp = 0;
        return 0;
    }

    nr_settings settings;
    if (nr_settings_parse(text, &settings)) {
        /* The language, which is not a knob. `NR_MENU_LANG_EN` is index 0 and the fallback, so an
         * absent or unreadable value leaves the caller's choice alone rather than forcing English
         * over a language the command line set. */
        int at = nr_settings_find(&settings, "menu_language");
        if (at >= 0) {
            int lang = atoi(settings.value[at]);
            if (lang >= 0 && lang < NR_MENU_LANG_COUNT) b->language = lang;
        }

        for (int i = 0; i < NR_KNOBS_COUNT; ++i) {
            const nr_knob_def *k = &knobs[i];
            int at = nr_settings_find(&settings, k->key);
            if (at < 0) continue;                 /* absent: the default stands, and stays unwritten */

            if (k->kind == 1) {
                /* An unknown profile falls back to the default: the daemon would refuse it and
                 * keep its old value, so showing it would be a menu that lies. */
                if (choice_index(k, settings.value[at]) >= 0)
                    snprintf(b->choice[i], sizeof b->choice[i], "%s", settings.value[at]);
                b->values[i] = (float)(choice_index(k, b->choice[i]) < 0
                                       ? 0 : choice_index(k, b->choice[i]));
            } else {
                char *end = NULL;
                double value = strtod(settings.value[at], &end);
                if (end == settings.value[at]) continue;   /* not a number: leave the default */
                if (value < k->low) value = k->low;
                if (value > k->high) value = k->high;
                b->values[i] = (float)value;
            }
            format_row(b, i);
        }
    }

    b->stamp = nr_file_stamp(b->settings_path);
    free(text);
    return 1;
}

/* ------------------------------------------------------------------ writing */

/* Where the live preview is written, beside the real settings file.
 *
 * **Why a second file.** The daemon polls `NR_SETTINGS` to apply a knob to the picture, and the
 * menu has to reach it - but that same file is the user's SAVED settings, and writing it on every
 * arrow press and every Reset means "Save" saves nothing and Reset destroys what was saved. Two
 * purposes, one file, and the destructive one wins.
 *
 * So the preview goes somewhere else. The daemon reads `NR_SETTINGS` for what to show; the menu
 * writes what the user is TRYING here, and only `save_settings` writes the real file. Reset then
 * means "go back to the defaults on screen", which the user can look at and discard by closing the
 * panel - the saved values are never touched.
 *
 * A suffix rather than a second path from the caller: the live file is meaningless apart from the
 * settings file it previews, and deriving it keeps the two from drifting when NR_SETTINGS moves. */
static void live_settings_path(const nr_menu_bridge *b, char *out, size_t size)
{
    snprintf(out, size, "%s.live", b->settings_path);
}

/* Write only the knobs the user has touched, as the daemon's live view.
 *
 * Deliberately not a merge with the real file: this is not a settings document, it is a message
 * that says "for these knobs, use these values right now". The daemon applies the keys it finds
 * and ignores the rest, so a partial object is the correct shape rather than a shortcut.
 *
 * The writer's PID goes in with it. A preview is only meaningful while its author is alive, and the
 * file has to survive paths where nothing gets to clean up - a killed game, a crash, a driver that
 * never calls destroy. The PID is what the daemon checks to tell a live preview from a leftover,
 * and it is the difference between "these are the settings right now" and "these were the settings
 * the last time somebody pressed Reset". */
/* Append to a bounded buffer, keeping the running length honest.
 *
 * `snprintf` returns the length it WOULD have written, so a truncating call pushes `n` past the end
 * of the buffer - and the next call's `sizeof out - n` then underflows to a huge size_t and writes
 * past the end. It cannot happen with the current bounds (the worst case is 12 741 bytes into
 * 16 384, measured against the parser's own limits), but that is arithmetic on the present field
 * widths rather than a guarantee, and a knob with a longer key would turn a wrong note into an
 * out-of-bounds write in somebody else's process.
 *
 * Clamping costs one comparison per field and makes the truncation loud instead: `truncated` is
 * reported by the caller, which refuses to write a file it knows is short rather than handing the
 * daemon a half object. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int truncated;
} nr_json_out;

static void nr_json_init(nr_json_out *o, char *buf, size_t cap)
{
    o->buf = buf; o->cap = cap; o->len = 0; o->truncated = 0;
    if (cap) buf[0] = '\0';
}

/* No `__attribute__((format))` here: MSVC does not have it, and this file is built by cl on
 * Windows and gcc on Linux. The format string is checked by eye instead - every call site is a
 * literal in this file, so there is nowhere for a mismatch to come from. */
static void nr_json_put(nr_json_out *o, const char *fmt, ...)
{
    if (o->truncated) return;
    if (o->len >= o->cap) { o->truncated = 1; return; }
    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (wrote < 0) { o->truncated = 1; return; }
    size_t got = (size_t)wrote;
    if (got >= o->cap - o->len) { o->truncated = 1; o->len = o->cap - 1; return; }
    o->len += got;
}

static int write_live_settings(nr_menu_bridge *b)
{
    if (!b->settings_path[0]) return 0;
    char out[4096];
    char path[sizeof b->settings_path + 8];
    nr_json_out o;
    nr_json_init(&o, out, sizeof out);
    nr_json_put(&o, "{\n");
    int emitted = 0;
#define NR_EMIT(...)                                                        \
    do {                                                                    \
        if (emitted) nr_json_put(&o, ",\n");                                \
        nr_json_put(&o, __VA_ARGS__);                                       \
        emitted = 1;                                                        \
    } while (0)
#ifdef _WIN32
    NR_EMIT("  \"menu_pid\": %lu", (unsigned long)GetCurrentProcessId());
#else
    NR_EMIT("  \"menu_pid\": %lu", (unsigned long)getpid());
#endif
    /* Whether a Save has confirmed what the preview shows.
     *
     * A preview is written the moment a knob moves, so the daemon can move the picture - that is
     * what "changes apply immediately" means. But the file OUTLIVES the process: a game exits with
     * the panel open on every normal quit, and then the next launch reads a preview nobody
     * confirmed. That was the reported bug, and no timestamp rule can catch it, because "Reset ten
     * minutes after Save" and "a real preview ten minutes after Save" are the same timestamps.
     *
     * What separates them is intent, so it is recorded rather than inferred. `confirmed` is 1 only
     * after a Save has put the same values in the settings file; a preview that is not confirmed is
     * the user mid-edit, and the daemon ignores it on a fresh start. */
    NR_EMIT("  \"menu_confirmed\": %d", b->preview_confirmed);
    for (int i = 0; i < NR_KNOBS_COUNT; ++i) {
        if (!b->moved[i]) continue;
        const nr_knob_def *k = &knobs[i];
        if (k->kind == 1)
            NR_EMIT("  \"%s\": \"%s\"", k->key, b->choice[i][0] ? b->choice[i] : "standard");
        else
            NR_EMIT("  \"%s\": %g", k->key, (double)b->values[i]);
    }
#undef NR_EMIT
    nr_json_put(&o, "%s}\n", emitted ? "\n" : "");
    live_settings_path(b, path, sizeof path);
    /* Atomic, for the same reason the settings file is: the daemon polls by mtime and a reader
     * that caught a half-written object would log a parse error and keep the old values. A short
     * object is not written at all: the daemon would see malformed JSON and keep the values it has,
     * which looks like the menu stopped working. */
    if (o.truncated) return 0;
    return nr_write_file_atomic(path, out, o.len);
}

/* The settings file with only `menu_language` changed.
 *
 * The same shape as `save_settings` - the object that was found, re-emitted with its own keys and
 * its own value text - with one key substituted and every knob copied through untouched. That is
 * what keeps a language change from committing an unsaved edit, and it is why this cannot be
 * `save_settings` plus an argument. */
static int save_language_only(nr_menu_bridge *b)
{
    if (!b->settings_path[0]) return 0;

    char *existing = NULL;
    size_t existing_size = 0;
    int have = nr_read_file(b->settings_path, &existing, &existing_size);
    nr_settings settings;
    int parsed = have && nr_settings_parse(existing, &settings);

    char out[16384];
    nr_json_out o;
    nr_json_init(&o, out, sizeof out);
    nr_json_put(&o, "{\n");
    int emitted = 0;
    int wrote = 0;
#define NR_EMIT(...)                                                        \
    do {                                                                    \
        if (emitted) nr_json_put(&o, ",\n");                                \
        nr_json_put(&o, __VA_ARGS__);                                       \
        emitted = 1;                                                        \
    } while (0)

    if (parsed) {
        for (int i = 0; i < settings.count; ++i) {
            if (strcmp(settings.key[i], "menu_language") == 0) {
                NR_EMIT("  \"menu_language\": %d", b->language);
                wrote = 1;
            } else {
                NR_EMIT("  \"%s\": %s%s%s", settings.key[i], settings.quoted[i] ? "\"" : "",
                        settings.value[i], settings.quoted[i] ? "\"" : "");
            }
        }
    }
    if (!wrote) NR_EMIT("  \"menu_language\": %d", b->language);
#undef NR_EMIT
    nr_json_put(&o, "%s}\n", emitted ? "\n" : "");

    int ok = o.truncated ? 0 : nr_write_file_atomic(b->settings_path, out, o.len);
    free(existing);
    if (!ok) return 0;
    b->stamp = nr_file_stamp(b->settings_path);
    return 1;
}

static int save_settings(nr_menu_bridge *b)
{
    char *existing = NULL;
    size_t existing_size = 0;
    int have = nr_read_file(b->settings_path, &existing, &existing_size);
    nr_settings settings;
    int parsed = have && nr_settings_parse(existing, &settings);

    char out[16384];
    nr_json_out o;
    nr_json_init(&o, out, sizeof out);
    nr_json_put(&o, "{\n");

    int written[NR_KNOBS_COUNT];
    memset(written, 0, sizeof written);
    int wrote_language = 0;

    /* Whether a field has already been emitted, so the comma goes BEFORE a field rather than after
     * it. Writing `"key": value,\n` for every field and closing with `}` leaves a trailing comma
     * before the brace, which is not valid JSON - the daemon would log a parse error, keep its old
     * values, and the menu would look like it saved while nothing changed. That is worse than a
     * failed save, because it is silent. */
    int emitted = 0;
#define NR_EMIT(...)                                                        \
    do {                                                                    \
        if (emitted) nr_json_put(&o, ",\n");                                \
        nr_json_put(&o, __VA_ARGS__);                                       \
        emitted = 1;                                                        \
    } while (0)

    if (parsed) {
        for (int i = 0; i < settings.count; ++i) {
            int index = -1;
            for (int k = 0; k < NR_KNOBS_COUNT; ++k)
                if (strcmp(knobs[k].key, settings.key[i]) == 0) { index = k; break; }

            const char *name = settings.key[i];
            const char *value = settings.value[i];
            char moved_text[64];
            int quoted = settings.quoted[i];

            /* The language is not a knob, so it is not in the table the loop above searches. It is
             * substituted here instead, which is what keeps it to one key in the file: the loop
             * copies every existing key verbatim, so emitting it again at the end would leave two
             * `menu_language` entries and a JSON parser would take whichever it saw last. */
            if (strcmp(name, "menu_language") == 0) {
                snprintf(moved_text, sizeof moved_text, "%d", b->language);
                NR_EMIT("  \"%s\": %s", name, moved_text);
                wrote_language = 1;
                continue;
            }

            if (index >= 0 && b->moved[index]) {
                const nr_knob_def *k = &knobs[index];
                if (k->kind == 1) {
                    value = b->choice[index][0] ? b->choice[index] : "standard";
                    quoted = 1;
                } else {
                    snprintf(moved_text, sizeof moved_text, "%g", (double)b->values[index]);
                    value = moved_text;
                    quoted = 0;
                }
                written[index] = 1;
            } else if (index >= 0) {
                written[index] = 1;   /* unchanged, but it is in the file: do not append it */
            }

            NR_EMIT("  \"%s\": %s%s%s", name, quoted ? "\"" : "", value, quoted ? "\"" : "");
        }
    }

    /* The language, appended only if the file did not already carry it. */
    if (!wrote_language) NR_EMIT("  \"menu_language\": %d", b->language);

    /* Append the keys the user moved that were not already there. */
    for (int i = 0; i < NR_KNOBS_COUNT; ++i) {
        if (written[i] || !b->moved[i]) continue;
        const nr_knob_def *k = &knobs[i];
        if (k->kind == 1)
            NR_EMIT("  \"%s\": \"%s\"", k->key,
                    b->choice[i][0] ? b->choice[i] : "standard");
        else
            NR_EMIT("  \"%s\": %g", k->key, (double)b->values[i]);
    }
#undef NR_EMIT

    nr_json_put(&o, "%s}\n", emitted ? "\n" : "");

    /* Atomic: the daemon polls this file's stamp, so a reader that caught a half-written object
     * would log a parse error and keep its old values. `nr_write_file_atomic` writes beside it and
     * renames over it. A short object is not written at all - see `nr_json_put`. */
    int ok = o.truncated ? 0 : nr_write_file_atomic(b->settings_path, out, o.len);
    free(existing);
    if (!ok) return 0;

    b->stamp = nr_file_stamp(b->settings_path);
    return 1;
}

/* ------------------------------------------------------------------ the public face */

/* Defined below with the rest of the settings logic, and declared here because the headless
 * creators call it: a caller with no keyboard moves a knob through the same function a keypress
 * reaches, so there is one implementation of "what a step means" rather than two. */
static int step_row(nr_menu_bridge *b, int row, int direction);

nr_menu_bridge *nr_menu_bridge_create(nr_menu *menu, const char *settings_path, int language)
{
    if (!menu) return NULL;
    nr_menu_bridge *b = (nr_menu_bridge *)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->menu = menu;
    /* -1, not calloc's 0: 0 is English, a real language, and a pending value of 0 would fire a
     * spurious language write on the first update. */
    b->language_pending = -1;
    b->language = (language >= 0 && language < NR_MENU_LANG_COUNT) ? language : NR_MENU_LANG_EN;
    snprintf(b->settings_path, sizeof b->settings_path, "%s",
             settings_path && *settings_path ? settings_path : "");
    load_settings(b);
    nr_menu_bridge_refresh(b);
    return b;
}

/* The same bridge with no menu. See the header: the settings logic does not need a window, and
 * handing this function a hand-made stand-in for `nr_menu` is what a first attempt did - and it
 * crashed, because the bridge writes through fields whose real layout is private to nr_menu.c. */
nr_menu_bridge *nr_menu_bridge_create_headless(const char *settings_path, int language)
{
    nr_menu_bridge *b = (nr_menu_bridge *)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->menu = NULL;
    b->language_pending = -1;      /* 0 is a real language, so the sentinel cannot be 0 */
    b->language = (language >= 0 && language < NR_MENU_LANG_COUNT) ? language : NR_MENU_LANG_EN;
    snprintf(b->settings_path, sizeof b->settings_path, "%s",
             settings_path && *settings_path ? settings_path : "");
    load_settings(b);
    /* The rows still get built, because they are what the label and value accessors read; they are
     * just not handed to a menu. */
    for (int i = 0; i < NR_KNOBS_COUNT; ++i) {
        format_row(b, i);
        b->rows[i].label = label_for(&knobs[i], b->language);
        b->rows[i].value = b->shown[i];
        b->rows[i].hint = hint_for(&knobs[i], b->language);
        b->rows[i].is_choice = (knobs[i].kind == 1);
        b->rows[i].is_flag = 0;
    }
    return b;
}

/* Move a knob without a keyboard. The same `step_row` the menu's own path calls, so a headless
 * caller and a keypress do the same thing to the same table.
 *
 * Writes the PREVIEW, not the settings file: moving a knob is not saving it. See
 * `nr_menu_bridge_update` for the rule and `save_settings` for the only writer. */
int nr_menu_bridge_move(nr_menu_bridge *b, int row, int direction)
{
    if (!b) return 0;
    if (!step_row(b, row, direction)) return 0;
    for (int i = 0; i < NR_KNOBS_COUNT; ++i) format_row(b, i);
    b->dirty = 1;
    b->preview_confirmed = 0;
    return write_live_settings(b);
}

int nr_menu_bridge_set(nr_menu_bridge *b, int row, float value)
{
    if (!b) return 0;
    if (row < 0 || row >= NR_KNOBS_COUNT) return 0;
    const nr_knob_def *k = &knobs[row];

    if (k->kind == 1) {
        /* A choice knob: `value` is the index the combo selected. The index lives in
         * `b->values[row]` and the name in `b->choice[row]`, and both are written because the rest
         * of the file reads one or the other depending on what it is doing. */
        int count = choice_count(k);
        int want = (int)(value + (value >= 0.0f ? 0.5f : -0.5f));
        if (want < 0) want = 0;
        if (want >= count) want = count - 1;
        choice_name(k, want, b->choice[row], sizeof b->choice[row]);
        b->values[row] = (float)want;
    } else {
        /* A float knob, clamped and snapped by the same rules a step uses. Snapping arithmetically
         * rather than stepping towards the target keeps this O(1) and makes the slider land on
         * exactly the values the arrow keys would reach. */
        float v = value;
        if (v < k->low) v = k->low;
        if (v > k->high) v = k->high;
        if (k->step > 0.0f) {
            float steps = (v - k->low) / k->step;
            v = k->low + (float)(int)(steps + 0.5f) * k->step;
        }
        if (v < k->low) v = k->low;
        if (v > k->high) v = k->high;
        b->values[row] = v;
    }

    b->moved[row] = 1;
    for (int i = 0; i < NR_KNOBS_COUNT; ++i) format_row(b, i);
    b->dirty = 1;
    /* The preview, like a step. Setting a knob is trying a value, not committing one. */
    b->preview_confirmed = 0;
    return write_live_settings(b);
}

/* The one writer of the user's settings file, and the only entry point that reaches it besides the
 * menu's Save key. Everything else previews. */
int nr_menu_bridge_save(nr_menu_bridge *b)
{
    if (!b) return 0;
    if (!save_settings(b)) return 0;
    b->preview_confirmed = 1;
    write_live_settings(b);   /* the daemon's view and the file agree from here */
    return 1;
}

int nr_menu_bridge_row_count(const nr_menu_bridge *b)
{
    return b ? NR_KNOBS_COUNT : 0;
}

/* The bridge's menu, so a headless caller can inject a request into it. See the header. */
nr_menu *nr_menu_bridge_menu(nr_menu_bridge *b) { return b ? b->menu : NULL; }

/* Set the language on a bridge with no menu; applied by the next update. *
 * The same two steps the menu's own L key goes through - record the choice, then persist it as the
 * language alone - so a headless caller exercises the shipped rule rather than a copy of it. On a
 * bridge WITH a menu this is not the way in: that one has `nr_menu_inject_language`, and going
 * through the menu is what makes the key and the combo agree. */
void nr_menu_bridge_set_language(nr_menu_bridge *b, int language)
{
    if (!b || language < 0 || language >= NR_MENU_LANG_COUNT) return;
    b->language_pending = language;
}

/* Close the panel, on a bridge with no menu.
 *
 * The Close button's semantics, and the user's rule for them: close the panel, KEEP the unsaved
 * changes, do not reset, do not save. The values stay in the bridge and stay in the preview, so the
 * picture keeps running with what was on screen and the settings file keeps whatever was last saved.
 *
 * With a menu this routes through `nr_menu_inject_open`, so the button, the key and this call cannot
 * disagree about what closing means. */
void nr_menu_bridge_close(nr_menu_bridge *b)
{
    if (!b) return;
    if (b->menu) {
        nr_menu_inject_open(b->menu, 0);
        return;
    }
    /* Headless: no open/close transition to detect, so the known state is written here rather than
     * on a state change the next frame would report. */
    write_live_settings(b);
    b->dirty = 1;
}

/* Reset the knobs to their defaults, on a bridge with no menu.
 *
 * A preview-only action, exactly as it is from the menu: the values on screen go back to the
 * defaults and the settings file is not touched. See `nr_menu_bridge_update`'s reset branch for
 * why that matters - the version of this that called `save_settings` destroyed a user's saved
 * settings on one keypress. */
void nr_menu_bridge_reset(nr_menu_bridge *b)
{
    if (!b) return;
    if (b->menu) {
        /* With a menu the request goes through it, so the KEY and this call cannot diverge. */
        nr_menu_inject_reset(b->menu);
        return;
    }
    load_defaults(b);
    for (int i = 0; i < NR_KNOBS_COUNT; ++i) b->moved[i] = 1;
    for (int i = 0; i < NR_KNOBS_COUNT; ++i) format_row(b, i);
    write_live_settings(b);
    b->dirty = 1;
}

const char *nr_menu_bridge_row_label(const nr_menu_bridge *b, int row, int language){
    if (!b || row < 0 || row >= NR_KNOBS_COUNT) return NULL;
    return label_for(&knobs[row], language);
}

const char *nr_menu_bridge_row_value(const nr_menu_bridge *b, int row)
{
    if (!b || row < 0 || row >= NR_KNOBS_COUNT) return NULL;
    return b->shown[row];
}

/* The knob's explanation. `nr_menu_hints` is indexed by knob order, which is the same order the
 * rows are in, so this is a lookup rather than a search. */
const char *nr_menu_bridge_row_hint(const nr_menu_bridge *b, int row, int language)
{
    if (!b || row < 0 || row >= NR_KNOBS_COUNT) return NULL;
    if (language < 0 || language >= NR_MENU_LANG_COUNT) language = NR_LANG_EN;
    return hint_for(&knobs[row], language);
}

void nr_menu_bridge_destroy(nr_menu_bridge *b)
{
    if (!b) return;
    /* The preview dies with the process that owns it.
     *
     * This is the bug that made a Save look like it had not happened. `.live` means "what the user
     * is trying RIGHT NOW", and it outlived the game: Reset wrote the defaults there, the game
     * exited with the panel still open - the normal way to leave - and the file stayed behind. On
     * the next launch the daemon read the saved settings and then overrode them with that stale
     * preview, so the picture showed the reset defaults while the file held the user's save. Moving
     * any knob rewrote the file and the picture jumped to the truth, which is exactly the symptom
     * that was reported.
     *
     * A preview is a message to a running daemon, not a document. Nothing about it is meaningful
     * once the menu that produced it is gone, so the lifetime ends here - rather than at the close
     * transition, which misses every exit with the panel open. */
    {
        char path[sizeof b->settings_path + 8];
        live_settings_path(b, path, sizeof path);
        remove(path);
    }
    free(b);
}

void nr_menu_bridge_refresh(nr_menu_bridge *b)
{
    if (!b) return;
    for (int i = 0; i < NR_KNOBS_COUNT; ++i) {
        format_row(b, i);
        b->rows[i].label = label_for(&knobs[i], b->language);
        b->rows[i].value = b->shown[i];
        b->rows[i].hint = hint_for(&knobs[i], b->language);
        b->rows[i].is_choice = (knobs[i].kind == 1);
        b->rows[i].is_flag = 0;
    }
    /* A headless bridge has no menu to hand the rows to. The rows are still built, because the
     * label and value accessors read them - which is what lets a caller with no window show the
     * same text the overlay would. */
    if (!b->menu) return;
    nr_menu_set_rows(b->menu, b->rows, NR_KNOBS_COUNT);
    nr_menu_set_title(b->menu, nr_menu_text(b->language, NR_STR_TITLE));
    nr_menu_set_hint(b->menu, nr_menu_text(b->language, NR_STR_HINT));
}

static int step_row(nr_menu_bridge *b, int row, int direction)
{
    if (row < 0 || row >= NR_KNOBS_COUNT) return 0;
    const nr_knob_def *k = &knobs[row];

    if (k->kind == 1) {
        int count = choice_count(k);
        int here = choice_index(k, b->choice[row]);
        if (here < 0) here = 0;
        choice_name(k, (here + direction + count) % count, b->choice[row], sizeof b->choice[row]);
        b->values[row] = (float)((here + direction + count) % count);
    } else {
        float next = b->values[row] + direction * k->step;
        /* A float step lands slightly off the range ends, so the clamp is on the step and the
         * comparison is against the clamped value - otherwise 1.0 would step to 1.0000001 and
         * print as 1. */
        if (next < k->low) next = k->low;
        if (next > k->high) next = k->high;
        if (next == b->values[row]) return 0;
        b->values[row] = next;
    }
    b->moved[row] = 1;
    return 1;
}

int nr_menu_bridge_update(nr_menu_bridge *b, float frame_seconds)
{
    if (!b) return 0;

    /* A headless bridge has no keys to read and nothing to draw, so it does no per-frame work.
     *
     * The language is the exception, and it is one because a headless caller has no other way to
     * set it: `nr_menu_bridge_create_headless` takes a language for the rows it formats, but a
     * change afterwards is what the "the language must not commit an unsaved knob" case is about,
     * and reaching the file through a language switch is a path the real menu takes. Handling it
     * here means one implementation of the rule rather than a test-only second one. */
    if (!b->menu) {
        if (b->language_pending >= 0) {
            b->language = b->language_pending;
            b->language_pending = -1;
            save_language_only(b);
        }
        return 0;
    }

    int state = nr_menu_update(b->menu, frame_seconds);

    /* The toggle and the comparison act with the menu closed, so they are handled before the open
     * test - otherwise a player would have to open the menu to switch the effect off. */
    if (nr_menu_take_toggle_request(b->menu))
        b->dirty = 1;

    int mode;
    if (nr_menu_take_compare_change(b->menu, &mode))
        b->dirty = 1;

    float step = nr_menu_take_split_step(b->menu);
    if (step != 0.0f) {
        float split = nr_menu_split(b->menu) + step * 0.02f;
        if (split < 0.02f) split = 0.02f;
        if (split > 0.98f) split = 0.98f;
        nr_menu_set_split(b->menu, split);
    }

    if (!nr_menu_is_open(b->menu)) return state;

    /* An edit from outside while the game runs - Notepad, or a tool - is picked up here. The stamp
     * only moves when somebody else wrote it, so this does not fight our own saves. */
    long long now = nr_file_stamp(b->settings_path);
    if (now && now != b->stamp) {
        if (load_settings(b)) nr_menu_bridge_refresh(b);
    }

    int language;
    if (nr_menu_take_language_change(b->menu, &language)) {
        b->language = language;
        nr_menu_bridge_refresh(b);
        /* The language alone is persisted, and only the language.
         *
         * The language is not a knob: it changes what the menu draws and nothing about the picture,
         * so it is not something to preview or discard and pressing L should keep it. What it must
         * NOT do is drag unsaved knob edits along with it - `save_settings` rewrites every knob the
         * user has touched, so calling it here would make pressing L quietly commit a half-finished
         * edit. That is the same class of bug as Reset overwriting a save, one step less obvious.
         *
         * So the file is edited in place: the existing object is read, `menu_language` is replaced
         * or appended, and everything else is copied through as it was. */
        save_language_only(b);
    }

    int direction = nr_menu_take_step(b->menu);
    if (direction != 0 && step_row(b, nr_menu_selected(b->menu), direction)) {
        nr_menu_bridge_refresh(b);
        b->dirty = 1;
        /* The PREVIEW, not the settings file.
         *
         * The daemon has to see the value to move the picture, and this is what it sees. What it
         * must not see is the user's saved settings being rewritten by an arrow press - a menu
         * where dragging a slider is indistinguishable from pressing Save has a Save button that
         * does nothing, and no way to try a value and back out. The saved file changes on Save. */
        /* Unconfirmed from here: the user is trying something and has not accepted it. */
        b->preview_confirmed = 0;
        write_live_settings(b);
    }

    if (nr_menu_take_save_request(b->menu)) {
        /* Save writes BOTH: the settings file, which is the record, and the preview, so the
         * daemon's live view and the saved one agree at the moment of saving. Without the second
         * write a value the user saved but never previewed - a typed one committed on Enter - would
         * sit in the file while the picture kept the old setting until the next restart. */
        if (save_settings(b)) {
            b->preview_confirmed = 1;
            write_live_settings(b);
            nr_menu_set_note(b->menu, nr_menu_text(b->language, NR_STR_SAVED));
        }
    }

    if (nr_menu_take_reset(b->menu)) {
        /* Reset returns the menu to the knob defaults - on screen and in the preview.
         *
         * It deliberately does NOT write the settings file. Reset used to call `save_settings`,
         * which meant one keypress destroyed every saved value with no confirmation and no undo;
         * the user's own saved settings are exactly what a Reset must not be able to reach. After
         * this the panel shows the defaults and the file still holds what was saved, so closing the
         * panel without saving discards the reset - which is what makes it safe to press. */
        load_defaults(b);
        for (int i = 0; i < NR_KNOBS_COUNT; ++i) b->moved[i] = 1;
        nr_menu_bridge_refresh(b);
        /* Unconfirmed, and this is the case that caused the report. The defaults go to the preview
         * so the picture changes - Reset has to be visible - but they are not what the user saved,
         * so a preview left behind by a Reset must not be applied on the next start. A Save after
         * the Reset confirms them; nothing else does. */
        b->preview_confirmed = 0;
        write_live_settings(b);
        b->dirty = 1;
    }

    /* Closing the panel keeps what the user has changed.
     *
     * The user's rule, in their words: "close the panel, keep the unsaved changes, do not reset, do
     * not save". So closing is a plain dismissal of the panel - the picture keeps running with the
     * values that are on screen, and the settings file keeps whatever was last saved. Nothing is
     * written and nothing is undone.
     *
     * This replaces the earlier behaviour, which removed the preview and reloaded the saved values
     * on close. That read as "Close threw my changes away", and it was also the only way back to
     * the saved state from an unsaved edit - so the two things were tangled. They are not any more:
     * the preview stays, and the values stay with it.
     *
     * The preview is a record of what is on screen, not of what was saved, which is why the daemon
     * applies it while the process that wrote it is alive and ignores it once it is not. Closing
     * does not end the process, so the edits live on for the rest of the session and are gone at
     * the next start unless Save was pressed. That is the whole of the contract. */
    if (state == 2 && !nr_menu_is_open(b->menu)) {
        /* The known state is written once so the daemon's view matches what the panel last showed.
         * Not `save_settings`: that is the file, and this is the picture. */
        write_live_settings(b);
    }

    return state;
}

int nr_menu_bridge_dirty(const nr_menu_bridge *b) { return b ? b->dirty : 0; }
void nr_menu_bridge_clear_dirty(nr_menu_bridge *b) { if (b) b->dirty = 0; }
int nr_menu_bridge_language(const nr_menu_bridge *b) { return b ? b->language : 0; }

const char *nr_menu_bridge_selected_key(const nr_menu_bridge *b)
{
    if (!b) return NULL;
    int row = nr_menu_selected(b->menu);
    if (row < 0 || row >= NR_KNOBS_COUNT) return NULL;
    return knobs[row].key;
}

/* The knobs this table owns, so a test can compare the list against `nr_knobs.KNOBS` without
 * parsing the C. */
int nr_menu_bridge_knob_count(void) { return NR_KNOBS_COUNT; }

const char *nr_menu_bridge_knob_name(int index)
{
    if (index < 0 || index >= NR_KNOBS_COUNT) return NULL;
    return knobs[index].key;
}

/* The range and step for one knob, so the same test can compare those too. Returns 0 when the
 * index is out of range or the knob is a choice. */
int nr_menu_bridge_knob_choice_count(int index)
{
    if (index < 0 || index >= NR_KNOBS_COUNT) return 0;
    /* A number has no options. `choice_count` is not used here: it returns 1 as a minimum because
     * the stepper takes a modulo by it, and reporting that to a UI turned every knob into a
     * single-entry dropdown. */
    const nr_knob_def *k = &knobs[index];
    if (k->kind != 1) return 0;
    return choice_count(k);
}

void nr_menu_bridge_knob_choice_name(int index, int which, char *out, size_t size)
{
    if (!out || size == 0) return;
    out[0] = '\0';
    if (index < 0 || index >= NR_KNOBS_COUNT) return;
    const nr_knob_def *k = &knobs[index];
    if (k->kind != 1) return;
    int count = choice_count(k);
    if (which < 0 || which >= count) return;
    /* Into the caller's buffer. A function-local static here would hand every caller the same
     * storage, so an array of names would end up holding the last option repeated - valid pointers,
     * all pointing at one string. */
    choice_name(k, which, out, size);
}

int nr_menu_bridge_knob_choice_index(const nr_menu_bridge *bridge, int index)
{
    if (!bridge) return 0;
    if (index < 0 || index >= NR_KNOBS_COUNT) return 0;
    const nr_knob_def *k = &knobs[index];
    if (k->kind != 1) return 0;
    int here = choice_index(k, bridge->choice[index]);
    return here < 0 ? 0 : here;
}

int nr_menu_bridge_knob_range(int index, float *low, float *high, float *step, float *default_value)
{
    if (index < 0 || index >= NR_KNOBS_COUNT) return 0;
    const nr_knob_def *k = &knobs[index];
    if (k->kind != 0) return 0;
    if (low) *low = k->low;
    if (high) *high = k->high;
    if (step) *step = k->step;
    if (default_value) *default_value = k->default_value;
    return 1;
}

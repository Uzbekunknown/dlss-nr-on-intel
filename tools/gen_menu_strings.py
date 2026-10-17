"""Generate the menu's four-language string tables for both C and Python, from one source.

Why generated rather than hand-written twice. The menu needs its labels in C, because it draws
inside the game's process with no interpreter; the tools need them in Python, because `nr_config`
writes the INI and the wizard renders the panel. Hand-writing both is how the knob *ranges*
already drifted once between three copies in this project.

**All four languages come from one file.** `dist-tools/windows-wizard.ps1` holds the setup window's
own strings, and it is now the single source for the overlay's too: the wizard's keys are
`nr_knobs.py`'s names, so the tables join without a mapping and a knob added there brings its
label to both. Chinese used to be a hand-written column in this file, which is exactly how the two
UIs drifted apart - the wizard had already dropped `Profile.neutral` while this generator still
emitted one, and the two disagreed about what a profile was. The wizard carries Chinese now, so
the split is gone.

Where the keys come from:
    Knob.<name>, Hint.<name>   the wizard's per-knob label and tooltip.
    everything else            the wizard's own keys, by name.

The menu's own keys - "compare", "menu key" - have no equivalent in a setup window with no
overlay, so they stay hand-written here for all four languages. See OWN below. They are the menu's
labels, not knob explanations: the explanations are the wizard's `Hint.*`, which every language
now carries, and which is what the overlay's tooltip shows.

The outputs are checked in. Re-run this when the wizard's strings change:

    py tools/gen_menu_strings.py
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WIZARD = REPO / "dist-tools" / "windows-wizard.ps1"
HEADER = REPO / "src" / "layer" / "nr_menu_strings.h"
MODULE = REPO / "src" / "layer" / "nr_menu_strings.py"

# The knobs, in `nr_knobs.py`'s order - which is the order the menu shows them.
KNOBS = ("render_scale", "min_extent", "profile", "intensity", "detail_strength",
         "colour_strength", "temporal", "hold", "release", "cut_limit", "weights")

# The wizard's key for each of the menu's non-knob strings. `None` means the wizard has no such
# key and the value comes from OWN below. The names are the wizard's, so a rename there fails
# loudly here rather than silently falling back to English.
EXTRA = {
    # our key:        (wizard key,      English fallback)
    "title":          ("Title",        "DLSS-NR on Intel - settings"),
    "saved":          ("Done.save",    "saved"),
    "reset":          ("ResetSettings", "reset to defaults"),
    "hint":           (None,           "Up/Down choose  Left/Right change  L language  "
                                       "C compare  N NR on/off  S save  INS close"),
    "toggle_nr":      (None,           "neural rendering"),
    "compare":        (None,           "compare"),
    "compare_split":  (None,           "split"),
    "compare_zoom":   (None,           "zoom"),
    "language":       (None,           "language"),
    "shortcut":       (None,           "menu key"),
    "on":             (None,           "on"),
    "off":            (None,           "off"),
}

# Compare modes, in the order the key cycles them. OptiScaler's own names are Off / Side by side /
# Wipe; the wipe is the one worth playing in, because it cuts one frame and resamples nothing.
COMPARE_MODES = ("off", "wipe", "side by side")

# The menu's own keys, which the wizard does not have: it is a setup window with no overlay, so
# there is no "compare" or "menu key" in it to translate. These are the ONLY strings not taken
# from the wizard, and they are the menu's labels rather than knob explanations - the
# explanations are the wizard's `Hint.*`, which every language now carries.
OWN = {
    "en": {
        "hint":          "Up/Down choose  Left/Right change  L language  C compare  "
                         "N NR on/off  S save  INS close",
        "toggle_nr":     "neural rendering",
        "compare":       "compare",
        "compare_split": "split",
        "compare_zoom":  "zoom",
        "language":      "language",
        "shortcut":      "menu key",
        "on":            "on",
        "off":           "off",
    },
    "zh": {
        "hint":          "\u4e0a/\u4e0b \u9009\u62e9  \u5de6/\u53f3 \u8c03\u6574  L \u8bed\u8a00  "
                         "C \u5bf9\u6bd4  N \u5f00\u5173  S \u4fdd\u5b58  INS \u5173\u95ed",
        "toggle_nr":     "\u795e\u7ecf\u6e32\u67d3",
        "compare":       "\u753b\u9762\u5bf9\u6bd4",
        "compare_split": "\u5206\u5272\u4f4d\u7f6e",
        "compare_zoom":  "\u653e\u5927",
        "language":      "\u8bed\u8a00",
        "shortcut":      "\u83dc\u5355\u952e",
        "on":            "\u5f00",
        "off":           "\u5173",
    },
    "ru": {
        "hint":          "\u0412\u0432\u0435\u0440\u0445/\u0412\u043d\u0438\u0437 \u0432\u044b\u0431\u043e\u0440  "
                         "\u0412\u043b\u0435\u0432\u043e/\u0412\u043f\u0440\u0430\u0432\u043e \u0438\u0437\u043c\u0435\u043d\u0438\u0442\u044c  "
                         "L \u044f\u0437\u044b\u043a  C \u0441\u0440\u0430\u0432\u043d\u0435\u043d\u0438\u0435  "
                         "N \u0432\u043a\u043b/\u0432\u044b\u043a\u043b  S \u0441\u043e\u0445\u0440\u0430\u043d\u0438\u0442\u044c  INS \u0437\u0430\u043a\u0440\u044b\u0442\u044c",
        "toggle_nr":     "\u043d\u0435\u0439\u0440\u043e\u043d\u043d\u044b\u0439 \u0440\u0435\u043d\u0434\u0435\u0440\u0438\u043d\u0433",
        "compare":       "\u0441\u0440\u0430\u0432\u043d\u0435\u043d\u0438\u0435",
        "compare_split": "\u0433\u0440\u0430\u043d\u0438\u0446\u0430",
        "compare_zoom":  "\u0443\u0432\u0435\u043b\u0438\u0447\u0435\u043d\u0438\u0435",
        "language":      "\u044f\u0437\u044b\u043a",
        "shortcut":      "\u043a\u043b\u0430\u0432\u0438\u0448\u0430 \u043c\u0435\u043d\u044e",
        "on":            "\u0432\u043a\u043b",
        "off":           "\u0432\u044b\u043a\u043b",
    },
    "es": {
        "hint":          "Arriba/Abajo elegir  Izq/Der cambiar  L idioma  C comparar  "
                         "N activar  S guardar  INS cerrar",
        "toggle_nr":     "renderizado neuronal",
        "compare":       "comparaci\u00f3n",
        "compare_split": "divisi\u00f3n",
        "compare_zoom":  "aumento",
        "language":      "idioma",
        "shortcut":      "tecla del men\u00fa",
        "on":            "activado",
        "off":           "desactivado",
    },
}

# The compare-mode names. The wizard has no compare feature, so these stay here for all four
# languages; OptiScaler's own order is Off / Side by side / Wipe, which this keeps.
COMPARE = {
    "en": ("Off", "Wipe", "Side by side"),
    "zh": ("\u5173\u95ed", "\u64e6\u9664", "\u5e76\u6392"),
    "ru": ("\u0412\u044b\u043a\u043b", "\u0428\u0442\u043e\u0440\u043a\u0430", "\u0420\u044f\u0434\u043e\u043c"),
    "es": ("Apagado", "Cortina", "Lado a lado"),
}


def wizard_tables():
    """The wizard's `en`, `ru` and `es` hashtables, by line range into the file.

    The keys are matched with or without quotes, because the wizard uses both: a key with a dot in
    it is written `'Knob.render_scale'` and a plain one is written `Title`. Matching only the
    quoted form silently dropped every plain key, which showed up as "ru is missing title" - so the
    pattern takes both and the check below catches it if the wizard changes shape again.
    """
    lines = WIZARD.read_text(encoding="utf-8", errors="replace").splitlines()
    starts = {}
    for i, line in enumerate(lines):
        m = re.match(r"\s*(en|ru|es|zh)\s*=\s*@\{\s*$", line)
        if m:
            starts[m.group(1)] = i

    tables = {}
    pair = re.compile(r"(?:'([\w.\-]+)'|([A-Za-z_][\w.\-]*))\s*=\s*'((?:[^']|'')*)'")
    for lang, i in starts.items():
        depth, body = 0, []
        for line in lines[i:]:
            depth += line.count("{") - line.count("}")
            body.append(line)
            if depth <= 0 and len(body) > 1:
                break
        found = {}
        for quoted, plain, value in pair.findall("\n".join(body)):
            found[quoted or plain] = value.replace("''", "'")
        tables[lang] = found
    return tables


def build():
    tables = wizard_tables()
    if sorted(tables) != ["en", "es", "ru", "zh"]:
        raise SystemExit(f"expected en/es/ru/zh in the wizard, got {sorted(tables)}")

    # Labels: the wizard's for every language it carries, and OWN's for the keys it has no
    # equivalent for. English is the fallback only when a language names nothing at all.
    labels = {}
    for lang, table in tables.items():
        got = {}
        for knob in KNOBS:
            if f"Knob.{knob}" in table:
                got[knob] = table[f"Knob.{knob}"]
        for our, (wizard_key, fallback) in EXTRA.items():
            if wizard_key and wizard_key in table:
                got[our] = table[wizard_key]
            elif our in OWN.get(lang, {}):
                got[our] = OWN[lang][our]      # a key the wizard has no equivalent for
            elif lang == "en":
                got[our] = fallback
        labels[lang] = got

    # Hints: the wizard's `Hint.*` for every language it carries. A hint is what a tooltip shows,
    # and this is the array the overlay's question-mark reads - so it has to be complete for all
    # four, which is what the check below enforces.
    hints = {}
    for lang, table in tables.items():
        hints[lang] = {k: table[f"Hint.{k}"] for k in KNOBS if f"Hint.{k}" in table}

    # Every language must carry every key, or one of them shows a blank in a menu nobody can read.
    keys = set(labels["en"])
    for lang in ("zh", "ru", "es"):
        missing = sorted(keys - set(labels[lang]))
        if missing:
            raise SystemExit(f"{lang} is missing labels: {missing}")

    # And every language must have a hint for every knob: a knob with no explanation is a
    # question mark that shows nothing.
    for lang in ("zh", "ru", "es"):
        missing = sorted(set(KNOBS) - set(hints[lang]))
        if missing:
            raise SystemExit(f"{lang} is missing hints: {missing}")

    for lang in ("zh", "ru", "es"):
        if len(COMPARE[lang]) != len(COMPARE_MODES):
            raise SystemExit(f"{lang} has {len(COMPARE[lang])} compare modes, expected "
                             f"{len(COMPARE_MODES)}")

    write_header(labels, hints, COMPARE)
    write_module(labels, hints, COMPARE)
    print(f"  {HEADER.relative_to(REPO)}")
    print(f"  {MODULE.relative_to(REPO)}")
    print(f"  {len(keys)} keys x 4 languages, {len(KNOBS)} hints each, "
          f"{len(COMPARE_MODES)} compare modes")


def c_string(text):
    """A C string literal, UTF-8 with the bytes escaped as octal.

    Escaped rather than written raw because the layer compiles with /utf-8 on MSVC and a bare
    multibyte literal in a .c file is fine there but is not fine for every compiler that builds
    this tree, and the escape form is unambiguous in all of them.
    """
    out = []
    for byte in text.encode("utf-8"):
        if byte == 0x22:
            out.append('\\"')
        elif byte == 0x5C:
            out.append("\\\\")
        elif 0x20 <= byte < 0x7F:
            out.append(chr(byte))
        else:
            out.append(f"\\{byte:03o}")
    return '"' + "".join(out) + '"'


def write_header(labels, hints, compare):
    langs = ("en", "zh", "ru", "es")
    keys = list(labels["en"])
    lines = [
        "/* The menu's strings, in four languages. GENERATED - do not edit by hand.",
        " *",
        " * Written by `tools/gen_menu_strings.py` from two sources:",
        " *   en, ru, es  `dist-tools/windows-wizard.ps1`, which already ships them for every knob.",
        " *               Extracted rather than retyped, because the wizard's keys are",
        " *               `nr_knobs.py`'s names - so the tables join without a mapping and a knob",
        " *               added there brings its label here.",
        " *   zh          written in the generator. The wizard has no Chinese, so this is the one",
        " *               column that is new work.",
        " *",
        " * Strings are UTF-8 bytes, octal-escaped, so the literal means the same thing whatever",
        " * codepage the compiler reads the file in. The menu draws them through the atlas, which",
        " * is built from these codepoints at start-up - see nr_menu_font.c.",
        " */",
        "#ifndef NR_MENU_STRINGS_H",
        "#define NR_MENU_STRINGS_H",
        "",
        "#define NR_MENU_LANG_COUNT 4",
        "#define NR_MENU_KEY_COUNT " + str(len(keys)),
        "#define NR_MENU_COMPARE_COUNT " + str(len(compare["en"])),
        "/* The hints are one per KNOB, which is fewer than the keys: a key like `title` or `on` is",
        " * a string the menu shows, not a knob it explains. Declaring them with the key count left",
        " * the tail of each row zero-initialised, and the atlas builder walked into those NULLs -",
        " * which cost the menu its Chinese and Russian glyphs and faulted on the first real game. */",
        "#define NR_MENU_KNOB_COUNT " + str(len(KNOBS)),
        "enum { NR_LANG_EN = 0, NR_LANG_ZH, NR_LANG_RU, NR_LANG_ES };",
        "",
        "enum {",
    ]
    for i, k in enumerate(keys):
        lines.append(f"    NR_STR_{k.upper()} = {i},")
    lines += [
        "};",
        "",
        "/* [language][key] */",
        "extern const char *const nr_menu_strings[NR_MENU_LANG_COUNT][NR_MENU_KEY_COUNT];",
        "",
        "/* [language][mode] - the compare mode names, cycled by the compare key. */",
        "extern const char *const nr_menu_compare_names[NR_MENU_LANG_COUNT][NR_MENU_COMPARE_COUNT];",
        "",
        "/* [language][knob] - the one-line hint for a knob, indexed as nr_knobs.py lists them. */",
        "extern const char *const nr_menu_hints[NR_MENU_LANG_COUNT][NR_MENU_KNOB_COUNT];",
        "",
        "/* Implemented in nr_menu_strings_lookup.c, which is hand-written: these are logic, not",
        " * data, and would be lost on the next run of the generator. */",
        "",
        "/* The string for one key in one language. Falls back to English, then to an empty string,",
        " * so a hole in a table is a word in the wrong language rather than a blank row. */",
        "const char *nr_menu_text(int language, int key);",
        "",
        "/* Which key index a knob name maps to, or -1. The names are nr_knobs.py's, so the knob",
        " * list joins to this table without a mapping that could itself drift. */",
        "int nr_menu_key_for(const char *knob_name);",
        "",
        "#endif /* NR_MENU_STRINGS_H */",
        "",
    ]
    (REPO / "src" / "layer" / "nr_menu_strings.h").write_text("\n".join(lines),
                                                              encoding="utf-8", newline="\n")

    body = ['/* GENERATED by tools/gen_menu_strings.py - do not edit by hand. See the header for',
            ' * where each language comes from. */',
            '#include "nr_menu_strings.h"',
            '',
            'const char *const nr_menu_strings[NR_MENU_LANG_COUNT][NR_MENU_KEY_COUNT] = {']
    for lang in langs:
        body.append(f"    /* {lang} */ {{")
        for k in keys:
            body.append(f"        {c_string(labels[lang][k])},")
        body.append("    },")
    body += ['};', '', 'const char *const nr_menu_compare_names[NR_MENU_LANG_COUNT]'
                       '[NR_MENU_COMPARE_COUNT] = {']
    for lang in langs:
        names = ", ".join(c_string(n) for n in compare[lang])
        body.append(f"    /* {lang} */ {{ {names} }},")
    body += ['};', '', 'const char *const nr_menu_hints[NR_MENU_LANG_COUNT][NR_MENU_KNOB_COUNT] = {']
    for lang in langs:
        body.append(f"    /* {lang} */ {{")
        for k in KNOBS:
            body.append(f"        {c_string(hints[lang].get(k, ''))},")
        body.append("    },")
    body += ['};', '']
    (REPO / "src" / "layer" / "nr_menu_strings.c").write_text("\n".join(body),
                                                              encoding="utf-8", newline="\n")
    print(f"  src/layer/nr_menu_strings.c")


def write_module(labels, hints, compare):
    langs = ("en", "zh", "ru", "es")
    lines = [
        '"""The menu\'s strings, in four languages. GENERATED - do not edit by hand.',
        "",
        "Written by `tools/gen_menu_strings.py`; see the generated header for where each language",
        "comes from. Kept in step with the C table by construction: both are written from the same",
        "run, so the menu and the INI cannot disagree about a label.",
        '"""',
        "",
        "LANGUAGES = ('en', 'zh', 'ru', 'es')",
        "",
        "LABELS = {",
    ]
    for lang in langs:
        lines.append(f"    {lang!r}: {{")
        for k, v in labels[lang].items():
            lines.append(f"        {k!r}: {v!r},")
        lines.append("    },")
    lines += ["}", "", "HINTS = {"]
    for lang in langs:
        lines.append(f"    {lang!r}: {{")
        for k, v in hints[lang].items():
            lines.append(f"        {k!r}: {v!r},")
        lines.append("    },")
    lines += ["}", "", "COMPARE = {"]
    for lang in langs:
        lines.append(f"    {lang!r}: {list(compare[lang])!r},")
    lines += ["}", "", "COMPARE_MODES = ('off', 'wipe', 'side_by_side')", ""]
    MODULE.write_text("\n".join(lines), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    build()

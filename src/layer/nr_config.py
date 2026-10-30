"""The settings file the menu and the `nr-ctl` family share: an INI with comments.

Why an INI and not the JSON the daemon already reads (`nr_paths.SETTINGS`): the JSON is a wire
format between tools — flat, machine-written, no room for an explanation. This file is the one a
person opens, so a line has to be able to say what it does. The daemon still reads the JSON; the
menu writes both, and this module is the only writer of the INI.

Layout, beside the layer next to the game (`nr_layer.ini`), because that is the folder the user
is already told about and the one `Remove NR` cleans. The section names and the key spellings are
OptiScaler's where a knob is the same knob (`Upscalers`, `Sharpness`, `Menu`, `ShortcutKey`), so
someone who has edited its ini finds the same shape here.

Two things it does that a plain INI reader does not:

  * **Comments survive a save.** Rewriting the file would delete every `;` line the user added,
    so the writer re-emits the generated comment block and preserves the values, and the menu
    only ever sets what it owns.
  * **A language per file and per process.** Both languages' strings live here side by side, so
    switching does not need the file rewritten.

Hot reload: `revision()` returns an opaque stamp; the caller compares it against the last one it
saw and re-reads only when it moved. That is what makes an edit in Notepad take effect without
restarting the game, and it is a stat() per frame rather than a read.
"""
import os
import pathlib
import tempfile
import time

# ------------------------------------------------------------------ the knobs

# Kept in the order the menu shows them. `kind` drives both the widget and the parser:
#   number -> float with low/high, choice -> one of `choices`, flag -> bool.
SETTINGS = (
    # name,          kind,     low,  high, step,  default, section,      key
    ("render_scale", "number", 0.05, 1.0,  0.05,  1.0,  "Upscalers", "RenderScale"),
    ("min_extent",   "number", 128.0, 320.0, 64.0, 320.0, "Upscalers", "MinExtent"),
    ("profile",      "choice", None, None, None,  "standard", "Upscalers", "VulkanUpscaler"),
    ("intensity",    "number", 0.0,  2.0,  0.05,  1.0,  "Sharpness", "Intensity"),
    ("detail_strength", "number", 0.0, 2.0, 0.05, 1.0, "Sharpness", "DetailStrength"),
    ("colour_strength", "number", 0.0, 2.0, 0.05, 1.0, "Sharpness", "ColourStrength"),
    ("temporal",     "number", 0.0,  1.0,  0.05,  1.0,  "Temporal",  "History"),
    ("hold",         "number", 0.0,  1.0,  0.05,  1.0,  "Temporal",  "Hold"),
    ("release",      "number", 0.0,  64.0, 1.0,   24.0, "Temporal",  "Release"),
    ("cut_limit",    "number", 0.0,  1.0,  0.01,  0.15, "Temporal",  "CutLimit"),
)

PROFILES = ("standard", "natural", "cinematic")

# What the daemon's Settings class accepts, so what is written here is what it reads.
DAEMON_KEYS = tuple(name for name, *_ in SETTINGS)

FILE_NAME = "nr_layer.ini"


def _default_path():
    """Beside the layer, which is beside the game.

    `NR_LAYER_INI` overrides it, as the rest of this project's paths are overridable; without
    either, the user's temp folder, which is where every other default in `nr_paths` lands when
    nothing says otherwise.
    """
    explicit = os.environ.get("NR_LAYER_INI")
    if explicit:
        return pathlib.Path(explicit)
    directory = os.environ.get("NR_LAYER_DIR")
    if directory:
        return pathlib.Path(directory) / FILE_NAME
    return pathlib.Path(tempfile.gettempdir()) / FILE_NAME


# ------------------------------------------------------------------ the file's own text

# English only, as asked. The format is the one every INI reader takes: `;` comments, `[section]`,
# `key = value`. The menu prints the same strings in Chinese from LANG below.
HEADER = """; DLSS-NR on Intel — settings
;
; Written by the in-game menu (INS). Edit it here or there; both work, and a change
; takes effect on the next frame without restarting the game.
;
; Numbers are the only thing that matters to the program; everything after a ';' is for you.
; The ranges in the comments are what the menu allows, not what the engine accepts.

"""

COMMENTS = {
    "profile": ('VulkanUpscaler', 'Upscalers',
                'standard, natural, cinematic',
                'standard',
                'Which way to trade skin texture against highlights. Not a quality ladder:\n'
                '; what one profile adds to texture it takes from highlights.\n'
                ';   standard  - the default, adds the most texture\n'
                ';   natural   - keeps more highlights, slightly darker and flatter\n'
                ';   cinematic - keeps the most highlights, less saturated'),
    "intensity": ('Intensity', 'Sharpness', '0.0 to 2.0', '1.0',
                  'How far to go towards the model\'s picture. 1 is the model, 0 is the game\n'
                  '; untouched, above 1 exaggerates what it changed and can look overdone.'),
    "detail_strength": ('DetailStrength', 'Sharpness', '0.0 to 2.0', '1.0',
                        'How strongly the fine detail is applied - pores, hair, grain.\n'
                        '; 0 keeps only the change in tone. Away from 1 costs one blur a frame.'),
    "colour_strength": ('ColourStrength', 'Sharpness', '0.0 to 2.0', '1.0',
                        'How strongly the change in tone and colour is applied.\n'
                        '; It cannot add colour back: the pass tends to calm bright saturated\n'
                        '; areas, so above 1 they look more washed out, not richer.'),
    "render_scale": ('RenderScale', 'Upscalers', '0.05 to 1.0', '1.0',
                     'The fraction of each side the network runs on - the only setting that\n'
                     '; changes the frame rate. Lower is faster and draws coarser detail.\n'
                     '; 0.35-0.6 is the useful range for play; 1.0 is for screenshots.'),
    "min_extent": ('MinExtent', 'Upscalers', '128 to 320', '320',
                   'The smallest side the network\'s frame is padded to, by mirroring.\n'
                   '; 320 is what the vendor\'s driver uses. Lower is faster at small windows\n'
                   '; and draws a somewhat different picture, because the network sees less of\n'
                   '; the scene around it.'),
    "temporal": ('History', 'Temporal', '0.0 to 1.0', '1.0',
                 'How much of the previous frame to carry over. The network decides per pixel;\n'
                 '; this scales how much it keeps. 0 draws every frame on its own.'),
    "hold": ('Hold', 'Temporal', '0.0 to 1.0', '1.0',
             'How firmly to keep areas the game did not change. This is what stops still\n'
             '; backgrounds shimmering while something else moves. Cannot leave a trail: the\n'
             '; first frame in which the game changes a pixel lets go of it.'),
    "release": ('Release', 'Temporal', '0 to 64', '24',
                'Change, in levels of 255, at which the previous result is dropped where\n'
                '; something moved - the other side of Hold. 16-24 is the useful range; lower\n'
                '; drops more and the picture shimmers over moving things.'),
    "cut_limit": ('CutLimit', 'Temporal', '0.0 to 1.0', '0.15',
                  'How big a frame-to-frame change counts as a new scene, so the carried\n'
                  '; history is dropped instead of smeared across the cut.'),
}

MENU_COMMENT = """[Menu]
; The key that opens this menu, as a Windows virtual-key code. 45 is INSERT.
ShortcutKey = 45
; 1 shows the menu in Chinese, 0 in English. Also switchable inside the menu (L).
Language = 0
; The key that saves the settings to this file. 0 disables it.
; 116 is F5.
SaveKey = 116

"""


def _fmt(value, kind):
    if kind == "flag":
        return "1" if value else "0"
    if kind == "choice":
        return str(value)
    number = float(value)
    if number == int(number):
        return str(int(number))
    return f"{number:g}"


def generated_comment_lines():
    """Every comment line `render` can emit, as a set, so `parse` can tell ours from the user's.

    The generated comments do not depend on the values — only the `; Key: range (default X)` line
    does, and that is fixed too — so one pass over the defaults catches all of them. This is what
    makes it safe to preserve "any comment that is not ours" without guessing at indentation or
    proximity to a key.
    """
    lines = set()
    defaults = {name: default for name, kind, low, high, step, default, sec, key in SETTINGS}
    for block in (HEADER, MENU_COMMENT, render(defaults)):
        for raw in block.splitlines():
            stripped = raw.strip()
            if stripped.startswith(";") or stripped.startswith("#"):
                lines.add(stripped)
    return lines


def render(values, menu=None):
    """The whole file as text: header, commented sections, the menu block, then unowned keys.

    `values` is a name -> value mapping for the knobs above; anything missing falls back to the
    documented default, so a fresh file is written from the defaults rather than being empty.
    """
    menu = menu or {}
    out = [HEADER]
    sections = []
    for name, kind, low, high, step, default, section, key in SETTINGS:
        if section not in sections:
            sections.append(section)
    for section in sections:
        out.append(f"[{section}]\n")
        for name, kind, low, high, step, default, sec, key in SETTINGS:
            if sec != section:
                continue
            _, _, allowed, fallback, note = COMMENTS[name]
            value = values.get(name, default)
            out.append(f"; {note}\n")
            out.append(f"; {key}: {allowed}  (default {fallback})\n")
            out.append(f"{key} = {_fmt(value, kind)}\n\n")
        # the profile's allowed values belong with the key that takes them
        if section == "Upscalers":
            out.append("; VulkanUpscaler values: " + ", ".join(PROFILES) + "\n\n")
    out.append(MENU_COMMENT)
    # anything the user added that we do not own is carried through untouched
    extra = values.get("__extra__")
    if extra:
        out.append(extra.rstrip() + "\n")
    return "".join(out)


def parse(text):
    """Read it back. Unknown keys and unknown sections are kept, and so are the user's comments.

    Returns (values, menu, extra): the knobs, the `[Menu]` block, and every line this module does
    not own, as text, so a round trip through the menu does not lose a user's own annotations.

    A comment is kept when it is not one `render` generates. That test is exact rather than
    heuristic: the generated set is finite and known, so a `;` line that is not in it can only
    have been typed by a person, wherever in the file it sits.
    """
    ours = generated_comment_lines()
    values, menu = {}, {}

    section = ""
    extra_lines, seen_sections = [], set()

    def keep(raw, section_name):
        marker = f"[{section_name}]" if section_name else ""
        if marker and marker not in seen_sections:
            extra_lines.append(marker)
            seen_sections.add(marker)
        extra_lines.append(raw)

    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith(";") or line.startswith("#"):
            if line not in ours:
                keep(raw, section)
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            continue
        if "=" not in line:
            keep(raw, section)
            continue
        key, _, value = line.partition("=")
        key, value = key.strip(), value.strip()

        if section == "Menu":
            if key == "ShortcutKey":
                menu["shortcut"] = _int(value, 45)
                continue
            if key == "Language":
                menu["language"] = 1 if _int(value, 0) else 0
                continue
            if key == "SaveKey":
                menu["save_key"] = _int(value, 116)
                continue
        matched = False
        for name, kind, low, high, step, default, sec, ini_key in SETTINGS:
            if key != ini_key:
                continue
            matched = True
            if kind == "choice":
                values[name] = value if value in PROFILES else default
            else:
                values[name] = _clamp(_float(value, default), low, high)
            break
        if not matched:
            keep(raw, section)

    menu.setdefault("shortcut", 45)
    menu.setdefault("language", 0)
    menu.setdefault("save_key", 116)
    return values, menu, ("\n".join(extra_lines).strip() + "\n" if extra_lines else "")


def _float(value, fallback):
    try:
        return float(value)
    except (TypeError, ValueError):
        return float(fallback)


def _int(value, fallback):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return int(fallback)


def _clamp(value, low, high):
    if low is None or high is None:
        return value
    return max(float(low), min(float(high), value))


# ------------------------------------------------------------------ the file on disk


class Config:
    """The file, with the change stamp the caller polls.

    Deliberately not a watcher thread: the layer runs inside somebody else's process and adding a
    thread there is a new failure mode for a settings file. One `stat()` a frame costs nothing and
    has no way to go wrong.
    """

    def __init__(self, path=None, migrate_from=None):
        self.path = pathlib.Path(path) if path else _default_path()
        self.migrate_from = pathlib.Path(migrate_from) if migrate_from else None
        self.values = {}
        self.menu = {}
        self.extra = ""
        self._stamp = None
        self._loaded = False
        self.load(force=True)

    # -- reading ---------------------------------------------------------------

    def _current_stamp(self):
        try:
            st = self.path.stat()
        except OSError:
            return None
        return (st.st_mtime_ns, st.st_size)

    def changed(self):
        """True once, after the file has been written by anyone but us."""
        now = self._current_stamp()
        if now is None:
            return False
        if now == self._stamp:
            return False
        self._stamp = now
        return True

    def load(self, force=False):
        """Read it. Creates it from the defaults when it is not there yet."""
        if not force and not self.changed():
            return False
        try:
            text = self.path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            text = ""
        if not text.strip():
            text = self._seed()
        self.values, self.menu, self.extra = parse(text)
        self._stamp = self._current_stamp()
        self._loaded = True
        return True

    def _seed(self):
        """First run: write the file, and take the daemon's JSON settings as the starting point.

        The JSON is where a user's existing `.nr-ctl` choices live, so a fresh INI inherits them
        rather than resetting the picture. Read once, never written back.
        """
        values = {name: default for name, kind, low, high, step, default, sec, key in SETTINGS}
        source = self.migrate_from
        if source and source.exists():
            try:
                import json
                for name, value in json.loads(source.read_text(encoding="utf-8")).items():
                    if name in values:
                        values[name] = value
            except (OSError, ValueError):
                pass
        text = render(values)
        self._write(text)
        return text

    # -- writing ---------------------------------------------------------------

    def _write(self, text):
        """Whole file, then rename. A half-written INI would be read as defaults by the next
        load, and the daemon polls this path."""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_suffix(self.path.suffix + ".new")
        temporary.write_text(text, encoding="utf-8", newline="\n")
        os.replace(temporary, self.path)
        self._stamp = self._current_stamp()

    def save(self):
        """Re-emit the file from the current values, keeping the user's own extra lines."""
        values = dict(self.values)
        if self.extra:
            values["__extra__"] = self.extra
        self._write(render(values, self.menu))
        return self.path

    def set(self, name, value):
        """Set one knob. Returns True when it moved, so the caller only does work on a change."""
        if name not in DAEMON_KEYS:
            return False
        for entry in SETTINGS:
            if entry[0] != name:
                continue
            kind, low, high = entry[1], entry[2], entry[3]
            break
        else:
            return False
        new = value if kind == "choice" else _clamp(_float(value, self.values.get(name, 0.0)),
                                                    low, high)
        if self.values.get(name) == new:
            return False
        self.values[name] = new
        return True

    def bump(self, name, direction, step=None):
        """Move a knob by one step, for the arrow keys. Returns True when it moved."""
        for entry in SETTINGS:
            if entry[0] == name:
                kind, low, high, own_step = entry[1], entry[2], entry[3], entry[4]
                break
        else:
            return False
        if kind == "choice":
            order = list(PROFILES)
            current = self.values.get(name, PROFILES[0])
            index = order.index(current) if current in order else 0
            return self.set(name, order[(index + direction) % len(order)])
        step = step or own_step or 0.05
        current = _float(self.values.get(name, low), low)
        return self.set(name, current + direction * step)

    # -- the daemon's side -----------------------------------------------------

    def daemon_values(self):
        """Only the keys the daemon's `Settings` class takes, so writing its JSON cannot add a
        field it would reject."""
        return {name: self.values[name] for name in DAEMON_KEYS if name in self.values}

    def write_daemon_json(self, path):
        """The daemon re-reads this file when its stamp moves, which is the hot-reload path that
        already exists. The menu is a second writer to the same file, not a new mechanism."""
        import json
        path = pathlib.Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".new")
        merged = {}
        try:
            merged = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            pass
        merged.update(self.daemon_values())
        temporary.write_text(json.dumps(merged, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
        os.replace(temporary, path)


# ------------------------------------------------------------------ what the menu prints

# The menu is bilingual. Keys are the same in both; only the text differs, so a knob is added in
# one place. English is index 0 because the file and the log are English.
LANG = {
    "title":        ("DLSS-NR on Intel — settings", "DLSS-NR on Intel — 设置"),
    "profile":      ("profile", "配置风格"),
    "intensity":    ("intensity", "强度"),
    "detail_strength": ("detail strength", "细节强度"),
    "colour_strength": ("colour strength", "色彩强度"),
    "render_scale": ("render scale", "渲染比例"),
    "min_extent":   ("min extent", "最小边长"),
    "temporal":     ("temporal history", "时域历史"),
    "hold":         ("hold", "保持"),
    "release":      ("release", "释放"),
    "cut_limit":    ("cut limit", "转场阈值"),
    "menu_section": ("menu", "菜单"),
    "shortcut":     ("menu key", "菜单键"),
    "language":     ("language (L)", "语言 (L)"),
    "save_key":     ("save key", "保存键"),
    "save":         ("save to file", "保存到文件"),
    "hint":         ("Up/Down choose  Left/Right change  L language  S save  INS close",
                     "上/下选择  左/右调整  L 切换语言  S 保存  INS 关闭"),
    "saved":        ("saved", "已保存"),
    "reset":        ("reset to defaults", "恢复默认"),
    "on":           ("on", "开"),
    "off":          ("off", "关"),
}


def text(key, language=0):
    entry = LANG.get(key)
    if not entry:
        return key
    return entry[1] if language else entry[0]


# The window can draw the knobs from this without knowing their ranges, so the menu and the file
# cannot disagree about what exists.
def rows(config, language=0):
    out = []
    for name, kind, low, high, step, default, sec, key in SETTINGS:
        value = config.values.get(name, default)
        if kind == "choice":
            shown = str(value)
        elif kind == "flag":
            shown = text("on" if value else "off", language)
        else:
            shown = f"{float(value):g}"
        out.append((name, text(name, language), shown, kind, low, high, step))
    return out

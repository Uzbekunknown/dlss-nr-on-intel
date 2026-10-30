#!/usr/bin/env python3
"""The menu's own two native fixtures, built by tools\\build_win.bat into work/.

    python src/layer/test_menu_ui_fixtures.py

What each fixture answers, and why it is a separate program:

  test_menu_panel  what the PANEL reports when the user acts on it - one tap is one step, a
                   held arrow ramps at most 20 steps/s, Save/Reset/Close are reachable by
                   Tab+Space and report their action bits, and the panel keeps the language
                   it was handed. Drives the REAL nr_menu_ui.cpp against a plain ImGui
                   context, so the widgets, the nav and the hold-to-repeat are the shipped
                   ones and not a copy.

  test_menu_side   whether an edit reaches the BRIDGE - the knob table, the settings file
                   and the language switch - driven the way nr_layer.c drives them: keys,
                   panel frame, action bits, bridge update. Links the real nr_menu.c,
                   nr_menu_bridge.c and the generated strings.

  test_menu_language
                   whether the panel's selector shows the language its ROWS are drawn in. A
                   settings file saying `menu_language: 1` gave Chinese rows beside an "English"
                   selector: the bridge held the value and the menu held a second copy that
                   nothing updated after start-up.

  test_menu_keys   the panel's hotkeys, by structure. S, R and the effect toggle were dead
                   because their reads sat inside `if (m->open && m->read_keys)` and the ImGui
                   build turns `read_keys` off; two other shapes of test passed with that bug in
                   place, so this one asserts where the reads live in the source. It runs from
                   the repository root because that is where it reads them from.

  test_menu_choices
                    a choice list is only as long as its count. The cadence row declared four
                    options with a store of three names and the combo drew `*Unknown item*` as
                    its fourth entry - the second time that shape cost something, the first being
                    the profiles combo. It walks the layer's cadence row and every choice knob
                    the bridge produces.

All are Windows-only and none needs a GPU: they create no Vulkan device, only an ImGui
context. On another platform, or with no compiler to have built them, this reports a FAILURE
rather than a skip - a check that cannot run is not a check that passed, and the two fixtures
are the only thing that watches the panel's reporting at all.
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "work"
# The panel's frames sleep to make their hold timings real, so the budget is generous; a
# fixture that hangs is a failure, not a timeout nobody reports.
FIXTURES = ("test_menu_panel", "test_menu_side", "test_menu_keys", "test_menu_language",
            "test_menu_actions", "test_menu_pacing", "test_menu_choices")
# The input-path fixtures. They link user32 and drive real registrations and windows, so they are
# Windows-only by construction - off Windows the runner says so rather than skipping, because
# nothing else watches what they cover.
INPUT_FIXTURES = ("test_focus_gate", "test_raw_mouse")
# The ones that must run from the repository root, because they read the sources there rather
# than driving a linked copy of them. `test_menu_choices` joined them when it gained a check that
# `nr_layer.c` still copies the first cadence option instead of numbering it - a source assertion,
# because the builder is a static function in that file with no seam to call from a fixture.
FROM_ROOT = frozenset({"test_menu_keys", "test_menu_language", "test_menu_choices",
                       "test_menu_actions"})
TIMEOUT = 600


def check(name: str, ok: bool, detail: str = "") -> bool:
    print(f"  [{'ok  ' if ok else 'FAIL'}] {name}{'  ' + detail if detail else ''}", flush=True)
    return ok


def run(fixture: str) -> tuple[bool, str]:
    """Run one fixture in a scratch working directory and report what it said.

    The CWD is a fresh temporary folder because `test_menu_side` writes and removes its own
    settings file by a relative name, and the panel's ImGui context is happy anywhere. Running
    it from the repository root would leave a stray `nr-menu-side-test-settings.json` behind
    on a crash, which is the kind of file that turns up in a later `git status`.

    The scratch directory is under `work/`, NOT under the user's `%TEMP%`, and that is not
    tidiness. A process started from one drive can be refused the right to create files in the
    user's temporary folder - `ERROR_ACCESS_DENIED` on the fixture's very first write, with the
    path looking writable and every other program able to write there. Two fixtures failed here
    for exactly that reason and neither assertion was wrong. Copying the executable into `%TEMP%`
    first is NOT the fix either: on a machine that also restricts what may be *executed* from
    there, that turns a write failure into `PermissionError` from `CreateProcess`. A scratch
    directory beside the build output is writable, executable, and cleaned up afterwards.
    """
    executable = WORK / f"{fixture}.exe"
    if not executable.is_file():
        return False, f"{executable} is missing - tools\\build_win.bat builds it"
    if fixture in FROM_ROOT:
        # Reads src/layer/nr_menu.c and nr_layer.c by a relative path, so it runs where they
        # are. A scratch folder would make it report "not readable" and look like a red test.
        room = ROOT
        target = executable
    else:
        room = WORK / f"nr-menu-fixture-{os.getpid()}"
        room.mkdir(parents=True, exist_ok=True)
        target = executable
    try:
        done = subprocess.run([str(target)], cwd=room, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return False, f"no answer within {TIMEOUT} s"
    finally:
        if fixture not in FROM_ROOT:
            try:
                for leftover in room.iterdir():
                    leftover.unlink()
                room.rmdir()
            except OSError:
                pass
    output = (done.stdout + done.stderr).strip()
    if done.returncode != 0:
        return False, f"exit {done.returncode}\n{output[-3000:]}"
    if not output:
        return False, "ran and said nothing"
    return True, output.splitlines()[-1]


def main() -> int:
    print("the menu's native fixtures, headless:")
    if os.name != "nt":
        print(f"  [FAIL] not Windows: the fixtures link ImGui, GetAsyncKeyState and user32")
        return 1
    failures = 0
    total = len(FIXTURES) + len(INPUT_FIXTURES)
    for fixture in FIXTURES + INPUT_FIXTURES:
        ok, detail = run(fixture)
        failures += 0 if check(fixture, ok, detail) else 1
    if failures:
        print(f"\n{total - failures}/{total} passed")
        return 1
    print(f"\n{total}/{total} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

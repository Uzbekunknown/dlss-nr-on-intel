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

Both are Windows-only and both need no GPU: they create no Vulkan device, only an ImGui
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
FIXTURES = ("test_menu_panel", "test_menu_side")
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
    """
    executable = WORK / f"{fixture}.exe"
    if not executable.is_file():
        return False, f"{executable} is missing - tools\\build_win.bat builds it"
    room = Path(os.environ.get("TEMP", ".")) / f"nr-menu-fixture-{os.getpid()}"
    room.mkdir(parents=True, exist_ok=True)
    try:
        done = subprocess.run([str(executable)], cwd=room, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return False, f"no answer within {TIMEOUT} s"
    finally:
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
    for fixture in FIXTURES:
        ok, detail = run(fixture)
        failures += 0 if check(fixture, ok, detail) else 1
    if failures:
        print(f"\n{len(FIXTURES) - failures}/{len(FIXTURES)} passed")
        return 1
    print(f"\n{len(FIXTURES)}/{len(FIXTURES)} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

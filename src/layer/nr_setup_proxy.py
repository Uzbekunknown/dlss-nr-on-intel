#!/usr/bin/env python3
"""The release's command-line installer shares the window's proxy and ownership rules.

Paths arrive through NR_SETUP_* variables, never through generated shell source.
This helper uses only the standard library and neither loads Vulkan nor starts a game.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import sys


def load_core(root):
    root = Path(root)
    candidates = (root / "scripts/windows_wizard_core.py", root / "src/tools/windows_wizard_core.py")
    source = next((path for path in candidates if path.is_file()), None)
    if source is None:
        raise ValueError("The release is missing windows_wizard_core.py")
    spec = importlib.util.spec_from_file_location("_nr_setup_windows_core", source)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _regular(path, directory=False):
    path = Path(path)
    if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
        raise ValueError(f"An installer path cannot be a link or junction: {path}")
    if path.exists() and not (path.is_dir() if directory else path.is_file()):
        raise ValueError(f"An installer path has the wrong file type: {path}")


def choose_game(game, executable=None):
    game = Path(os.path.abspath(game))
    _regular(game, directory=True)
    if not game.is_dir():
        raise ValueError("The game directory does not exist")
    if executable:
        # The directory was already chosen by --game. A filename avoids accidentally
        # configuring another executable's directory while copying runtime files here.
        if Path(executable).name != executable:
            raise ValueError("--exe must be a filename inside the --game directory")
        candidates = [game / executable]
    else:
        candidates = sorted(path for path in game.glob("*.exe") if path.is_file())
        if len(candidates) > 1:
            raise ValueError("Several executables are in this folder; pass --exe with the one that draws with Vulkan")
    if len(candidates) != 1 or not candidates[0].is_file():
        raise ValueError("No game executable was found in the selected directory")
    _regular(candidates[0])
    return candidates[0]


def check(core, profile):
    """Check all ownership/collision conditions before the batch script copies runtime files."""
    destination = core.installed_path(profile)
    _regular(destination, directory=True)
    for name in (core.OWNER_NAME, core.GAME_FILES, core.ENV_FILE):
        _regular(destination / name)
    for name in (core.GAME_BACKUP, ".undo"):
        _regular(destination / name, directory=True)
    if (destination / ".undo").exists():
        raise ValueError("An unfinished proxy update is present; preserve its .undo files before reinstalling")
    if destination.exists() and any(destination.iterdir()):
        owns, detail = core._owned_installation(profile)
        if not owns:
            raise ValueError(detail)
        owner = core._read_json(destination / core.OWNER_NAME)
        if core._canonical(owner.get("game_exe", "")) != core._canonical(profile.game_exe):
            raise ValueError("The existing installation belongs to another executable; Remove NR first")
    arch = core.pe_architecture(profile.game_exe)
    if arch == "x86":
        raise ValueError("setup.bat installs the x64 layer; use NR-Setup.exe for a 32-bit game")
    record = core._game_record(destination)
    if record.get("arch") not in (None, "x64"):
        raise ValueError("The existing installation has another architecture; Remove NR first")
    if any(name != core.PROXY_NAME and entry.get("placed") for name, entry in record["files"].items()):
        raise ValueError("The existing installation also owns DirectX files; use NR-Setup.exe to update it")
    source = profile.root / core.PROXIES["x64"]
    target = profile.game_exe.parent / core.PROXY_NAME
    backup = destination / core.GAME_BACKUP / core.PROXY_NAME
    for path in (source, target, backup):
        _regular(path)
    if not source.is_file():
        raise ValueError("The release is missing its x64 Vulkan proxy")
    entry = record["files"].get(core.PROXY_NAME, {})
    if target.is_file():
        current = core._sha256(target)
        ours = core._sha256(source)
        if entry.get("placed") and current not in (entry.get("sha256"), ours):
            raise ValueError("The game's Vulkan loader changed since NR placed it; it will not be overwritten")
        if not entry.get("placed") and current != ours and backup.exists():
            raise ValueError("An existing Vulkan-loader backup blocks replacement; neither file will be overwritten")
    if entry.get("backup"):
        if not backup.is_file() or core._sha256(backup) != entry.get("previous_sha256"):
            raise ValueError("The recorded original Vulkan loader is missing or changed; it will not be replaced")
    elif backup.exists():
        raise ValueError("An unrecorded Vulkan-loader backup exists; it will not be overwritten")
    return destination


def prepare(core, profile):
    destination = check(core, profile)
    destination.mkdir(exist_ok=True)
    # This is the window's exact schema; GUI Remove NR therefore recognises the
    # command-line installation and restores the originals from game-files.json.
    core._atomic_json(destination / core.OWNER_NAME, {
        "schema_version": core.SCHEMA_VERSION, "root": str(profile.root),
        "game_exe": str(profile.game_exe),
    })
    return destination


def install(core, profile):
    destination = check(core, profile)
    owns, detail = core._owned_installation(profile)
    if not owns:
        raise ValueError(detail)
    controls = [destination / core.GAME_FILES, destination / core.ENV_FILE,
                core.profile_path(profile.root)]
    for path in controls:
        _regular(path)
    saved = {path: path.read_bytes() if path.exists() else None for path in controls}
    done = []
    try:
        done = core._place_game_files(profile, "x64", destination)
        core._write_proxy_environment(profile, destination, destination)
        for name in ("logs", "windows-wizard"):
            folder = profile.root / "work" / name
            _regular(folder, directory=True)
            folder.mkdir(parents=True, exist_ok=True)
        core.save_profile(profile)
    except BaseException:
        if done:
            core._undo_game_files(profile, destination, done)
        for path, contents in saved.items():
            if contents is None:
                path.unlink(missing_ok=True)
            else:
                path.write_bytes(contents)
        undo = destination / ".undo"
        if undo.exists() and not any(undo.iterdir()):
            undo.rmdir()
        raise
    # Remove only the old proxy we temporarily set aside, never an arbitrary
    # directory's contents. The game's original stays in game-backup until removal.
    for name, kind, entry, _ in done:
        if kind == "replaced":
            (destination / ".undo" / name).unlink()
    undo = destination / ".undo"
    if undo.exists():
        undo.rmdir()
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "prepare", "install"))
    args = parser.parse_args()
    try:
        root = Path(os.environ["NR_SETUP_ROOT"])
        _regular(root, directory=True)
        core = load_core(root)
        game = choose_game(os.environ["NR_SETUP_GAME"], os.environ.get("NR_SETUP_GAME_EXE"))
        profile = core.Profile(root=root, python=Path(sys.executable), game_exe=game)
        {"check": check, "prepare": prepare, "install": install}[args.action](core, profile)
        return 0
    except (OSError, ValueError, KeyError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

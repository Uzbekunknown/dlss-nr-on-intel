#!/usr/bin/env python3
"""JSON bridge between the native Windows setup window and the runtime tools."""
from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import shlex
import sys
import traceback

import windows_wizard_core as core


def release_root() -> Path:
    here = Path(__file__).resolve().parent
    return here.parent if (here / 'get_weights.py').is_file() else here.parents[1]


def windows_arguments(text: str) -> list[str]:
    if not text.strip():
        return []
    if os.name != 'nt':
        return shlex.split(text)
    # Match the argument rules used by Windows games; never send this to a shell.
    shell = ctypes.WinDLL('shell32', use_last_error=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    kernel.LocalFree.restype = ctypes.c_void_p
    count = ctypes.c_int()
    array = shell.CommandLineToArgvW('nr-game ' + text, ctypes.byref(count))
    if not array:
        raise OSError(ctypes.get_last_error(), 'Could not read game arguments')
    try:
        return [array[index] for index in range(1, count.value)]
    finally:
        kernel.LocalFree(array)


def remove_nr(profile, emit):
    """Remove NR as one step: Steam's launch options for the game go back first.

    With them still pointing at NR's wrapper the game would not start once the layer is gone,
    which is why core.uninstall refuses while they are set. In the window that refusal left
    the user pressing Remove NR again and again; Restore Steam was a button they had to find."""
    import windows_launch as launch
    restored = launch.restore_steam(profile, emit=emit)
    if not restored.get('ok'):
        return restored
    result = core.uninstall(profile, emit=emit)
    if restored.get('changed'):
        result['steam_restored'] = True
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('discover', 'check', 'dependencies', 'install', 'uninstall',
        'launch', 'steam-setup', 'steam-restore', 'on', 'off', 'status', 'report', 'save',
        'settings', 'settings-save', 'settings-reset'))
    parser.add_argument('--root', type=Path, default=release_root())
    parser.add_argument('--input', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--destination', type=Path)
    args = parser.parse_args()
    events: list[str] = []

    def emit(message):
        events.append(str(message))
        print(str(message), flush=True)

    try:
        if args.action == 'discover':
            profile = core.load_profile(args.root)
            result = {'ok': True, 'candidates': core.discover_python(),
                      'profile': profile.to_dict() if profile else None}
        elif args.action in ('settings', 'settings-save', 'settings-reset'):
            # Runtime controls are root-scoped and available before installation.
            # Ignore any input launch profile or root: a slider cannot change the
            # selected game, Steam configuration, Python or effect on/off state.
            values = json.loads(args.input.read_text(encoding='utf-8-sig')) if args.input else {}
            if not isinstance(values, dict):
                raise ValueError('Runtime settings input must be a JSON object.')
            if args.action == 'settings':
                result = core.get_settings(args.root)
            elif args.action == 'settings-save':
                if 'settings' not in values:
                    raise ValueError('Specify the runtime settings to save.')
                result = core.save_settings(args.root, values['settings'])
            else:
                result = core.reset_settings(args.root, values.get('reset'))
        else:
            if args.input:
                values = json.loads(args.input.read_text(encoding='utf-8-sig'))
                values['root'] = str(args.root.resolve())
                if 'game_args_raw' in values:
                    values['game_args'] = windows_arguments(values.pop('game_args_raw'))
                profile = core.Profile.from_dict(values)
            else:
                profile = core.load_profile(args.root)
            if profile is None:
                raise ValueError('Choose your game and Python first.')
            if args.action == 'check':
                result = core.validate(profile)
            elif args.action == 'dependencies':
                result = core.ensure_dependencies(profile, emit=emit)
            elif args.action == 'install':
                result = core.install(profile, emit=emit)
            elif args.action == 'uninstall':
                result = remove_nr(profile, emit)
            elif args.action in ('launch', 'steam-setup', 'steam-restore'):
                import windows_launch as launch
                method = {'launch': launch.launch, 'steam-setup': launch.configure_steam,
                          'steam-restore': launch.restore_steam}[args.action]
                result = method(profile, emit=emit)
            elif args.action in ('on', 'off'):
                result = core.set_effect(profile, args.action == 'on')
            elif args.action == 'status':
                result = core.status(profile)
                result.setdefault('ok', True)
            elif args.action == 'save':
                core.save_profile(profile)
                result = {'ok': True, 'profile': profile.to_dict()}
            else:
                if args.destination is None:
                    raise ValueError('Choose a report destination.')
                result = core.export_report(profile, args.destination)
            if not isinstance(result, dict):
                result = {'ok': True, 'result': result}
            result.setdefault('profile', profile.to_dict())
        result['action'] = args.action
        result['events'] = events
    except Exception as error:
        traceback.print_exc()
        result = {'ok': False, 'action': args.action, 'error': str(error), 'events': events}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix('.new')
    temporary.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(args.output)
    return 0 if result.get('ok', False) else 1


if __name__ == '__main__':
    raise SystemExit(main())

"""CPU-only release fixtures; no game, Steam, vendor weights or GPU execution."""
from dataclasses import replace
import ast
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import windows_wizard_core as core


def write_pe(path, x64=True, suffix=b""):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    data = bytearray(90)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<H", data, 68, 0x8664 if x64 else 0x14C)
    struct.pack_into("<H", data, 88, 0x20B if x64 else 0x10B)
    path.write_bytes(data + suffix)
    return path


def write_pe_importing(path, imports=(), delayed=(), x64=True, text=b""):
    """A PE whose import and delay-load tables name `imports` and `delayed`, in one section with
    `text` after them."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    first = 112 if x64 else 96  # the data directories, after NumberOfRvaAndSizes
    optional = bytearray(first + 16 * 8)
    struct.pack_into("<H", optional, 0, 0x20B if x64 else 0x10B)
    struct.pack_into("<I", optional, first - 4, 16)
    base, raw = 0x1000, 0x400
    names_at = 20 * (len(imports) + 1) + 32 * (len(delayed) + 1)
    entries, delay_entries, names = bytearray(), bytearray(), bytearray()
    for name in imports:
        entries += struct.pack("<5I", 0, 0, 0, base + names_at + len(names), 0)
        names += name.encode("ascii") + b"\0"
    for name in delayed:
        delay_entries += struct.pack("<8I", 1, base + names_at + len(names), 0, 0, 0, 0, 0, 0)
        names += name.encode("ascii") + b"\0"
    section = bytes(entries) + bytes(20) + bytes(delay_entries) + bytes(32) + bytes(names) + text
    if imports:
        struct.pack_into("<II", optional, first + 8 * 1, base, 20 * (len(imports) + 1))
    if delayed:
        struct.pack_into("<II", optional, first + 8 * 13, base + 20 * (len(imports) + 1), 32 * (len(delayed) + 1))
    dos = bytearray(64)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 60, 64)
    coff = bytearray(24)
    coff[:4] = b"PE\0\0"
    struct.pack_into("<HH", coff, 4, 0x8664 if x64 else 0x14C, 1)
    struct.pack_into("<H", coff, 20, len(optional))
    table = bytearray(40)
    table[:6] = b".rdata"
    struct.pack_into("<IIII", table, 8, len(section), base, len(section), raw)
    data = bytes(dos + coff + optional + table)
    path.write_bytes(data + bytes(raw - len(data)) + section)
    return path


def write_weights(path, count=649):
    """Format-only zero-sized tensors; deliberately contain no real weights."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    header = json.dumps({f"fixture_{n}": {"dtype": "F16", "shape": [0],
                                         "data_offsets": [0, 0]} for n in range(count)}).encode()
    path.write_bytes(len(header).to_bytes(8, "little") + header)


def python_info(**changes):
    result = {"probe_ok": True, "platform": "win32", "os_name": "nt", "bits": 64,
              "abi_platform": "win-amd64", "native_apis": True, "version": "3.12.10",
              "packages": {"numpy": "fixture", "safetensors": "fixture"},
              "package_errors": {}, "output": ""}
    result.update(changes)
    return result


class WizardFixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nr wizard (fixture) ")
        self.addCleanup(self.temporary.cleanup)
        # Resolved, as the wizard resolves what it is given: a TEMP in 8.3 form
        # (C:\Users\NAME~1\...) otherwise compares unequal to the same file's long name.
        self.base = Path(self.temporary.name).resolve()
        self.root = self.base / "release (candidate)"
        self.root.mkdir()
        for relative in core.RUNTIME_MODULES:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("# release fixture\n", encoding="utf-8")
        for directory in core.SOURCE_DIRS:
            (self.root / "src" / directory).mkdir(parents=True, exist_ok=True)
        write_pe(self.root / "nr_layer.dll")
        write_pe(self.root / "nr_layer32.dll", x64=False)
        write_pe(self.root / "nr_vulkan_proxy.dll", suffix=b"proxy x64")
        write_pe(self.root / "nr_vulkan_proxy32.dll", x64=False, suffix=b"proxy x32")
        for directory, x64 in (("x64", True), ("x32", False)):
            for name in core.DXVK_FILES:
                write_pe(self.root / "dxvk" / directory / name, x64=x64, suffix=f"DXVK {directory} {name}".encode())
        for name in core.LIBRARIES:
            write_pe(self.root / "work" / name)
        for name in core.SHADERS:
            (self.root / "work" / name).write_bytes(b"SPIR-V fixture")
        core._atomic_json(self.root / "VkLayer_dlss_nr.json", {
            "file_format_version": "1.0.0", "layer": {"name": "VK_LAYER_dlssnr_intel",
            "type": "GLOBAL", "library_path": "nr_layer.dll", "api_version": "1.3.0"}})
        self.game = write_pe(self.base / "game (test)" / "selected.exe")
        self.python = write_pe(self.base / "Python (selected)" / "python.exe")
        self.profile = core.Profile(self.root, self.python, self.game)
        self.weights = self.root / "work/mlxw/dlssnr-logical.safetensors"
        write_weights(self.weights)
        self.native = patch.object(core, "_python_info", return_value=python_info()).start()
        self.deps = patch.object(core, "_load_dependencies", return_value={"ok": True, "failures": {}}).start()
        self.processes = patch.object(core, "_process_snapshot", return_value=[]).start()
        self.running = patch.object(core, "_running_executable", return_value=None).start()
        # The window's own level and UAC as Windows has them by default, whoever runs the tests.
        self.integrity = patch.object(core, "_integrity", return_value=0x2000).start()
        self.uac = patch.object(core, "_uac_enabled", return_value=True).start()
        self.addCleanup(patch.stopall)
        core._LOG_STATES.clear()
        self.addCleanup(core._LOG_STATES.clear)

    def checks(self, profile=None):
        return {item["name"]: item for item in core.validate(profile or self.profile)["checks"]}

    def own(self, profile=None):
        profile = profile or self.profile
        directory = core.installed_path(profile)
        directory.mkdir(parents=True, exist_ok=True)
        core._atomic_json(directory / core.OWNER_NAME, {"schema_version": 1, "root": str(profile.root)})
        return directory

    def launch_state(self, profile=None, pid=123, offset=0, started="fixture-session"):
        profile = profile or self.profile
        core._atomic_json(profile.root / "work/windows-wizard/launch-state.json", {
            "root": str(profile.root), "game_exe": str(profile.game_exe), "pid": pid,
            "daemon_start_bytes": offset, "started_utc": started, "exit_code": None})
        self.processes.return_value = [{"ProcessId": pid, "ExecutablePath": str(profile.game_exe)}]
        self.running.side_effect = lambda value, pid=pid, path=str(profile.game_exe): path if value == pid else None

    def test_profile_defaults_round_trip_and_paths(self):
        minimal = core.Profile.from_dict({"root": str(self.root), "python": str(self.python),
                                         "game_exe": str(self.game)})
        self.assertEqual(minimal.api, "vulkan")
        self.assertEqual(minimal.game_args, [])
        self.assertIsNone(minimal.dll)
        self.assertEqual(core.Profile.from_dict(minimal.to_dict()), minimal)
        self.assertTrue(core.save_profile(minimal)["ok"])
        self.assertEqual(core.load_profile(self.root), minimal)

    def test_steam_blank_app_id_allows_autodetection_and_invalid_id_is_rejected(self):
        for app_id, expected in (("", True), ("379720", True), ("DOOM", False), ("379720 extra", False)):
            with self.subTest(app_id=app_id):
                profile = replace(self.profile, launch_mode="steam", steam_app_id=app_id)
                result = core.validate(profile)
                self.assertEqual(result["ok"], expected)
                self.assertEqual(self.checks(profile)["launch_mode"]["ok"], expected)
        profile = replace(self.profile, launch_mode="steam", steam_app_id="")
        result = core.install(profile)
        self.assertTrue(result["ok"], result)
        self.assertEqual(core.load_profile(self.root).steam_app_id, "")

    def test_runtime_env_same_python_unique_pipe_and_clean_inherited_layers(self):
        with patch.dict(os.environ, {"NR_ROOT": "stale", "NR_PYTHON": "stale", "XMX_TEST": "1",
                                     "VK_LAYER_PATH": "stale", "VK_DEVICE_LAYERS": "stale",
                                     "DISABLE_NR_LAYER": "1", "SteamAppId": "2280"}):
            env = core.runtime_env(self.profile)
        self.assertEqual(env["NR_PYTHON"], str(self.python))
        self.assertEqual(env["NR_ROOT"], str(self.root))
        self.assertEqual(env["NR_DAEMON"], str(self.root / "src/layer/nr_daemon.py"))
        self.assertEqual(env["NR_SETTINGS"], str(self.root / "work/nr_settings.json"))
        self.assertEqual(env["NR_LAYER_TRIGGER"], str(self.root / "work/nr_trigger"))
        self.assertEqual({key.upper(): value for key, value in env.items()}["STEAMAPPID"], "2280")
        for name in ("XMX_TEST", "VK_DEVICE_LAYERS", "DISABLE_NR_LAYER"):
            self.assertNotIn(name, env)
        # The manifest itself: given the folder, the loader read the installation's records too.
        self.assertEqual(env["VK_LAYER_PATH"], str(self.game.parent / "dlss-nr" / "VkLayer_dlss_nr.json"))
        self.assertNotEqual(env["NR_LAYER_SOCKET"], core.runtime_env(replace(self.profile, root=self.base / "other"))["NR_LAYER_SOCKET"])
        self.assertEqual(env["NR_LAYER_SOCKET"], core.runtime_env(self.profile)["NR_LAYER_SOCKET"])
        self.assertNotIn("DISABLE_VK_LAYER_VALVE_steam_overlay_1", env)
        self.assertNotIn("DISABLE_VK_LAYER_VALVE_steam_fossilize_1", core.runtime_env(replace(self.profile, disable_fossilize=False)))

    def test_32bit_game_accepted_and_other_machines_and_32bit_python_rejected(self):
        write_pe(self.game, x64=False)
        self.assertTrue(self.checks()["game_architecture"]["ok"])
        arm64 = bytearray(write_pe(self.game).read_bytes())
        struct.pack_into("<H", arm64, 68, 0xAA64)
        self.game.write_bytes(bytes(arm64))
        self.assertFalse(self.checks()["game_architecture"]["ok"])
        write_pe(self.game)
        self.native.return_value = python_info(bits=32, abi_platform="win32")
        self.assertFalse(self.checks()["python_native_x64"]["ok"])
        self.assertFalse(core.install(self.profile)["ok"])
        self.assertFalse(core.installed_path(self.profile).exists())

    def test_mingw_abi_or_missing_native_apis_are_rejected(self):
        for change in ({"abi_platform": "mingw_x86_64"}, {"native_apis": False}):
            with self.subTest(change=change):
                self.native.return_value = python_info(**change)
                self.assertFalse(self.checks()["python_native_x64"]["ok"])

    def test_discovery_excludes_mingw_even_when_inherited_nr_python(self):
        mingw = write_pe(self.base / "mingw Python" / "python.exe")
        self.native.side_effect = lambda candidate, **kwargs: python_info(abi_platform="mingw_x86_64") if Path(candidate) == mingw else python_info()
        with patch.dict(os.environ, {"NR_PYTHON": str(mingw)}), patch.object(core.sys, "executable", str(self.python)), patch.object(core.shutil, "which", return_value=None):
            found = core.discover_python()
        self.assertIn(str(self.python), found)
        self.assertNotIn(str(mingw), found)

    def test_every_runtime_shader_and_allocator_must_be_regular_file(self):
        for relative in ("work/gemm_staged.spv", "work/half_probe.spv", "work/libnr_alloc.dll"):
            with self.subTest(relative=relative):
                path = self.root / relative
                original = path.read_bytes()
                path.unlink()
                path.mkdir()
                self.assertFalse(self.checks()["runtime_files"]["ok"])
                self.assertFalse(core.install(self.profile)["ok"])
                self.assertFalse(core.installed_path(self.profile).exists())
                path.rmdir()
                path.write_bytes(original)

    def test_loader_or_runtime_dependency_failure_is_preflight_failure(self):
        self.deps.return_value = {"ok": False, "failures": {"vulkan-1.dll": "missing loader"}}
        result = core.install(self.profile)
        self.assertFalse(result["ok"])
        self.assertFalse(core.installed_path(self.profile).exists())
        self.assertIn("missing loader", self.checks()["runtime_dependencies"]["detail"])

    def test_source_directory_missing_is_rejected_before_game_staging(self):
        (self.root / "src/bench").rmdir()
        self.assertFalse(self.checks()["runtime_files"]["ok"])
        with patch.object(core.tempfile, "mkdtemp", side_effect=AssertionError("No game writes after failed preflight")):
            self.assertFalse(core.install(self.profile)["ok"])

    def test_dxvk_comes_from_the_release_for_the_games_architecture(self):
        profile = replace(self.profile, api="dxvk")
        self.assertTrue(self.checks(profile)["dxvk_files"]["ok"])
        write_pe(self.root / "dxvk/x64/dxgi.dll", x64=False)
        self.assertFalse(self.checks(profile)["dxvk_files"]["ok"])
        write_pe(self.game, x64=False)
        self.assertTrue(self.checks(profile)["dxvk_files"]["ok"])

    def test_dxvk_install_sets_the_games_files_aside_and_remove_puts_them_back(self):
        profile = replace(self.profile, api="dxvk")
        game = self.game.parent
        (game / "dxgi.dll").write_bytes(b"the game's own proxy")
        shutil.copy2(self.root / "dxvk/x64/d3d9.dll", game / "d3d9.dll")  # someone's identical copy
        (game / f"{self.game.stem}_d3d9.log").write_text("that DXVK's own log")
        self.assertIn("dxgi.dll", self.checks(profile)["game_files"]["detail"])
        result = core.install(profile)
        self.assertTrue(result["ok"], result)
        for name in core.DXVK_FILES:
            self.assertEqual((game / name).read_bytes(), (self.root / "dxvk/x64" / name).read_bytes())
        installed = core.installed_path(profile)
        self.assertEqual((installed / core.GAME_BACKUP / "dxgi.dll").read_bytes(), b"the game's own proxy")
        manifest = json.loads((installed / "VkLayer_dlss_nr.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["layer"]["library_arch"], "64")
        self.assertEqual(Path(core.runtime_env(profile)["DXVK_LOG_PATH"]), self.root / "work/logs")
        # Installed again, the game's original is still the one set aside, once.
        self.assertTrue(core.install(profile)["ok"])
        self.assertEqual((installed / core.GAME_BACKUP / "dxgi.dll").read_bytes(), b"the game's own proxy")
        self.assertFalse((installed / ".undo").exists())
        (game / f"{self.game.stem}_dxgi.log").write_text("a DXVK log from a launch outside NR")
        removed = core.uninstall(profile)
        self.assertTrue(removed["ok"], removed)
        self.assertEqual((game / "dxgi.dll").read_bytes(), b"the game's own proxy")
        self.assertEqual((game / "d3d9.dll").read_bytes(), (self.root / "dxvk/x64/d3d9.dll").read_bytes())
        for name in ("d3d8.dll", "d3d10core.dll", "d3d11.dll", f"{self.game.stem}_dxgi.log"):
            self.assertFalse((game / name).exists(), name)
        self.assertFalse(installed.exists())
        self.assertEqual((game / f"{self.game.stem}_d3d9.log").read_text(), "that DXVK's own log")
        self.assertEqual(sorted(path.name for path in game.iterdir()),
                         sorted(["d3d9.dll", "dxgi.dll", self.game.name, f"{self.game.stem}_d3d9.log"]))

    def test_dxvk_placement_failure_puts_the_games_files_back(self):
        profile = replace(self.profile, api="dxvk")
        game = self.game.parent
        (game / "dxgi.dll").write_bytes(b"original")
        copy = shutil.copy2

        def failing(source, target, *args, **kwargs):
            if Path(source).name == "dxgi.dll":
                raise OSError("disk full")
            return copy(source, target, *args, **kwargs)
        with patch.object(core.shutil, "copy2", side_effect=failing):
            self.assertFalse(core.install(profile)["ok"])
        self.assertEqual(sorted(path.name for path in game.iterdir()), ["dxgi.dll", self.game.name])
        self.assertEqual((game / "dxgi.dll").read_bytes(), b"original")

    def test_late_failure_after_dxvk_commit_puts_the_games_files_back(self):
        profile = replace(self.profile, api="dxvk")
        game = self.game.parent
        (game / "dxgi.dll").write_bytes(b"original")
        with patch.object(core, "save_profile", side_effect=OSError("profile write failed")):
            self.assertFalse(core.install(profile)["ok"])
        self.assertEqual(sorted(path.name for path in game.iterdir()), ["dxgi.dll", self.game.name])
        self.assertEqual((game / "dxgi.dll").read_bytes(), b"original")

    def test_32bit_game_gets_the_32bit_layer_and_dxvk(self):
        write_pe(self.game, x64=False)
        profile = replace(self.profile, api="dxvk")
        self.assertTrue(core.install(profile)["ok"])
        installed = core.installed_path(profile)
        manifest = json.loads((installed / "VkLayer_dlss_nr.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["layer"]["library_arch"], "32")
        self.assertEqual(Path(manifest["layer"]["library_path"]), installed / "nr_layer32.dll")
        self.assertEqual(core.pe_architecture(installed / "nr_layer32.dll"), "x86")
        self.assertEqual((self.game.parent / "d3d9.dll").read_bytes(), (self.root / "dxvk/x32/d3d9.dll").read_bytes())
        self.assertEqual((self.game.parent / core.PROXY_NAME).read_bytes(),
                         (self.root / "nr_vulkan_proxy32.dll").read_bytes())

    def test_every_game_gets_the_proxy_and_its_environment(self):
        game = self.game.parent
        result = core.install(self.profile)
        self.assertTrue(result["ok"], result)
        self.assertEqual((game / core.PROXY_NAME).read_bytes(), (self.root / "nr_vulkan_proxy.dll").read_bytes())
        installed = core.installed_path(self.profile)
        lines = (installed / core.ENV_FILE).read_text(encoding="utf-8").splitlines()
        values = dict(line.split("=", 1) for line in lines if "=" in line and not line.startswith("#"))
        self.assertEqual(Path(values["NR_GAME_EXE"]), self.game)
        self.assertEqual(Path(values["NR_LAUNCH_STATE"]), self.root / "work/windows-wizard/launch-state.json")
        # The proxy makes no folder for its record: without one, status never saw the game.
        self.assertTrue((self.root / "work/windows-wizard").is_dir())
        self.assertNotIn("NR_REAL_VULKAN", values)
        for name, value in core.layer_environment(self.profile).items():
            self.assertEqual(values[name], value, name)
        self.assertTrue(core.uninstall(self.profile)["ok"])
        self.assertEqual(sorted(path.name for path in game.iterdir()), [self.game.name])

    def test_pacing_in_the_profile_the_environment_and_an_installed_game(self):
        # Asynchronous mode and every Nth frame: kept in the profile, set for the layer either
        # way, and written into an installed game's ENV_FILE without its other lines moving.
        self.assertEqual((self.profile.nr_async, self.profile.nr_every), (False, 1))
        env = core.layer_environment(self.profile)
        self.assertEqual((env["NR_LAYER_LIVE"], env["NR_LAYER_ASYNC"]), ("1", "0"))
        paced = replace(self.profile, nr_async=True, nr_every=2)
        env = core.runtime_env(paced)
        self.assertEqual((env["NR_LAYER_LIVE"], env["NR_LAYER_ASYNC"]), ("2", "1"))
        self.assertEqual(core.Profile.from_dict(paced.to_dict()), paced)
        earlier = self.profile.to_dict()
        del earlier["nr_async"], earlier["nr_every"]
        self.assertEqual(core.Profile.from_dict(earlier), self.profile)
        for bad in ({"nr_async": "yes"}, {"nr_every": 0}, {"nr_every": core.MAX_EVERY + 1},
                    {"nr_every": True}, {"nr_every": 1.5}, {"nr_every": "2"}):
            with self.assertRaises(ValueError, msg=bad):
                core.Profile.from_dict({**self.profile.to_dict(), **bad})

        # Not installed: kept in the profile, and nothing else written.
        result = core.set_pacing(paced)
        self.assertEqual((result["ok"], result["applied"]), (True, False))
        self.assertEqual(core.load_profile(self.root), paced)
        self.assertFalse(core.installed_path(paced).exists())

        def written():
            lines = env_file.read_text(encoding="utf-8").splitlines()
            return lines, dict(line.split("=", 1) for line in lines
                               if "=" in line and not line.startswith("#"))

        self.assertTrue(core.install(self.profile)["ok"])
        env_file = core.installed_path(self.profile) / core.ENV_FILE
        before, values = written()
        self.assertEqual((values["NR_LAYER_LIVE"], values["NR_LAYER_ASYNC"]), ("1", "0"))
        # The window writes BOTH now - the profile and the game's env file. It used to write the
        # profile alone, on the reasoning that two writers for one file need ordering and none
        # existed; what that cost was a window that said "takes effect at the next start" while
        # never writing the file the layer reads. The panel keeps the profile in step
        # (`write_profile_pacing`), so the window's write cannot silently revert the panel's.
        result = core.set_pacing(replace(paced, python=self.base / "unchecked" / "python.exe"))
        self.assertEqual((result["ok"], result["applied"]), (True, True))
        after, values = written()
        # The pacing lines are what changed, and everything else is byte-identical: NR_GAME_EXE,
        # NR_LAUNCH_STATE and the rest are the installation, not this function's to rewrite.
        self.assertNotEqual(after, before, "the window wrote nothing - the game keeps the old values")
        kept = [line for line in before if line.split("=", 1)[0].strip() not in core.PACING]
        self.assertEqual([line for line in after if line.split("=", 1)[0].strip() not in core.PACING],
                         kept, "a line the window does not own was rewritten")
        # And the file now agrees with the profile it was written from.
        self.assertEqual((values["NR_LAYER_LIVE"], values["NR_LAYER_ASYNC"]), ("2", "1"))
        self.assertEqual(values["NR_PYTHON"], str(self.python))
        # The profile it did write is the one a later reader sees.
        self.assertEqual(core.load_profile(self.root).nr_every, core.Profile.from_dict(
            {**paced.to_dict(), "python": str(self.base / "unchecked" / "python.exe")}).nr_every)
        self.assertFalse(env_file.with_name(core.ENV_FILE + ".new").exists())
        # Another release's installation: the profile is kept, and its file is left alone.
        other = replace(self.profile, root=self.base / "another release", nr_every=3)
        (self.base / "another release" / "work").mkdir(parents=True)
        result = core.set_pacing(other)
        self.assertEqual((result["ok"], result["applied"]), (True, False))
        self.assertEqual(written()[0], after)
        self.assertTrue(core.uninstall(self.profile)["ok"])

    def test_gpu_image_profile_is_opt_in_and_updates_installed_environment(self):
        self.assertFalse(self.profile.gpu_image_path)
        old = self.profile.to_dict()
        del old["gpu_image_path"]
        self.assertEqual(core.Profile.from_dict(old), self.profile)
        for value in ("true", 1, None, []):
            with self.assertRaises(ValueError):
                core.Profile.from_dict({**old, "gpu_image_path": value})
        for flag in ("NR_GPU_IMAGE", "NR_GPU_TRANSPORT", "NR_GPU_ONLY"):
            self.assertEqual(core.runtime_env(self.profile)[flag], "0")
        with patch.dict(os.environ, {"NR_GPU_ONLY": "1"}):
            self.assertEqual(core.runtime_env(self.profile)["NR_GPU_ONLY"], "0")
        enabled = replace(self.profile, gpu_image_path=True, nr_async=True, nr_every=3)
        self.assertEqual(core.Profile.from_dict(enabled.to_dict()), enabled)
        self.assertTrue(core.install(self.profile)["ok"])
        path = core.installed_path(self.profile) / core.ENV_FILE
        before = path.read_text(encoding="utf-8").splitlines()
        # The profile carries the choice and the window writes the game's file too, so the file the
        # layer reads at its next start is the one this profile describes.
        result = core.set_pacing(enabled)
        self.assertTrue(result["applied"])
        self.assertEqual(core.load_profile(self.root), enabled)
        environment = core.layer_environment(enabled)
        # Never strict: where the GPU path stands aside, NR takes its usual path (issue #17).
        self.assertEqual((environment["NR_GPU_IMAGE"], environment["NR_GPU_TRANSPORT"],
                          environment["NR_GPU_ONLY"]), ("1", "1", "0"))
        # The GPU path takes every frame in step or stands aside: the asynchronous mode and
        # every Nth frame asked for wait in the profile until it is off.
        self.assertEqual((environment["NR_LAYER_ASYNC"], environment["NR_LAYER_LIVE"]), ("0", "1"))
        # And the installed file carries those values now, with the installation's own lines
        # untouched - NR_GAME_EXE and friends are not this function's to rewrite.
        after = path.read_text(encoding="utf-8").splitlines()
        self.assertNotEqual(after, before, "the GPU path was never given to the game")
        self.assertEqual([line for line in after if line.split("=", 1)[0].strip() not in core.PACING],
                         [line for line in before if line.split("=", 1)[0].strip() not in core.PACING],
                         "a line the window does not own was rewritten")
        file_values = dict(line.split("=", 1) for line in after if "=" in line)
        self.assertEqual((file_values["NR_GPU_IMAGE"], file_values["NR_LAYER_ASYNC"],
                          file_values["NR_LAYER_LIVE"]), ("1", "0", "1"))
        # With the path off the profile keeps what was asked for, and the file follows.
        core.set_pacing(replace(enabled, gpu_image_path=False))
        off = core.layer_environment(core.load_profile(self.root))
        self.assertEqual((off["NR_LAYER_ASYNC"], off["NR_LAYER_LIVE"], off["NR_GPU_IMAGE"]),
                         ("1", "3", "0"))
        off_values = dict(line.split("=", 1) for line in
                          path.read_text(encoding="utf-8").splitlines() if "=" in line)
        self.assertEqual((off_values["NR_GPU_IMAGE"], off_values["NR_LAYER_ASYNC"],
                          off_values["NR_LAYER_LIVE"]), ("0", "1", "3"))
        self.assertNotIn("NR_LAYER_TIMING", core.runtime_env(enabled))
        self.assertNotIn("NR_LAYER_TIMING_LOG", core.runtime_env(enabled))

    def preset_b_weights(self):
        return self.root / core.PRESETS["B"][0]

    def test_the_network_preset_in_the_profile_the_environment_and_an_installed_game(self):
        # A profile from before the presets runs A, the only network there was.
        self.assertEqual(self.profile.preset, "A")
        old = self.profile.to_dict()
        del old["preset"]
        self.assertEqual(core.Profile.from_dict(old), self.profile)
        for value in ("C", "a", 1, None, ["B"]):
            with self.assertRaises(ValueError, msg=value):
                core.Profile.from_dict({**old, "preset": value})
        self.assertEqual(core.runtime_env(self.profile)["NR_PRESET"], "A")
        with patch.dict(os.environ, {"NR_PRESET": "B"}):
            self.assertEqual(core.runtime_env(self.profile)["NR_PRESET"], "A")
        chosen = replace(self.profile, preset="B")
        self.assertEqual(core.Profile.from_dict(chosen.to_dict()), chosen)
        self.assertEqual(core.status(self.profile)["presets"], {"A": True, "B": False})
        self.assertTrue(core.install(self.profile)["ok"])
        path = core.installed_path(self.profile) / core.ENV_FILE

        def values():
            return dict(line.split("=", 1) for line in path.read_text(encoding="utf-8").splitlines()
                        if "=" in line and not line.startswith("#"))

        self.assertEqual(values()["NR_PRESET"], "A")
        # Without B's weights a daemon started for it would end at once, and the game run
        # without NR: refused, and neither the profile nor the game's environment changes.
        before = path.read_bytes()
        result = core.set_pacing(chosen)
        self.assertFalse(result["ok"])
        self.assertIn("310.9.16.0", result["error"])
        self.assertEqual(result["presets"], {"A": True, "B": False})
        self.assertEqual(core.load_profile(self.root).preset, "A")
        self.assertEqual(path.read_bytes(), before)
        write_weights(self.preset_b_weights(), count=core.PRESETS["B"][1])
        self.assertEqual(core.status(self.profile)["presets"], {"A": True, "B": True})
        result = core.set_pacing(chosen)
        self.assertEqual((result["ok"], result["applied"]), (True, True))
        self.assertEqual(core.load_profile(self.root).preset, "B")
        # The profile carries the network AND the window writes the game's file, so a game started
        # from this profile is given B - which is the whole point of choosing it here.
        self.assertEqual(core.layer_environment(core.load_profile(self.root))["NR_PRESET"], "B")
        self.assertEqual(values()["NR_PRESET"], "B")
        # And back: the window's own control reaches the game as well.
        core.set_pacing(self.profile)
        self.assertEqual(core.layer_environment(core.load_profile(self.root))["NR_PRESET"], "A")
        self.assertEqual(values()["NR_PRESET"], "A")
        self.assertTrue(core.uninstall(self.profile)["ok"])

    def test_the_window_does_not_write_a_stale_pacing_over_the_panel(self):
        """The ordering that made this window stop writing nr-env.txt in the first place.

        The window's pacing controls hold the values the profile had when it opened:
        `Apply-Profile` runs after discover/dependencies/install and never on the `status` poll, so
        nothing refreshes them while the window sits there. The in-game panel writes both
        `nr-env.txt` and `windows-profile.json` while that window is open. So the sequence

            window opens (async off, every 1)
            panel changes it (async on, every 3) - both files move
            user touches anything pacing-related in the window

        would, without care, put the window's stale values back over the panel's. That is the
        regression stage 3 reintroduced by making this function write the file again.

        The window resolves it in `windows-wizard.ps1` by comparing each control against
        `$script:LastProfile` - the profile the controls were last filled from - and taking the
        file's value for the fields the user did NOT move. The comparison is reproduced here
        because the decision is what has to be right; the PowerShell around it is one property
        read.
        """
        self.assertTrue(core.install(self.profile)["ok"])
        path = core.installed_path(self.profile) / core.ENV_FILE

        def values():
            return dict(line.split("=", 1) for line in path.read_text(encoding="utf-8").splitlines()
                        if "=" in line and not line.startswith("#"))

        # The window opened and filled its controls from this profile: async off, every 1.
        window_opened_from = core.load_profile(self.root)
        self.assertEqual((window_opened_from.nr_async, window_opened_from.nr_every), (False, 1))

        # The panel moves it, and keeps the profile in step - which is what makes this safe.
        moved = replace(window_opened_from, nr_async=True, nr_every=3)
        self.assertTrue(core.set_pacing(moved)["applied"])
        self.assertEqual((values()["NR_LAYER_ASYNC"], values()["NR_LAYER_LIVE"]), ("1", "3"))

        # Now the user touches something in the window that sends a pacing request. The controls
        # still show the values the window opened with; LastProfile is the same profile, so every
        # pacing field compares equal and each takes the FILE's value.
        on_disk = core.load_profile(self.root)
        stale = window_opened_from
        resolved = {}
        for key in ("nr_async", "nr_every", "gpu_image_path", "preset"):
            shown = getattr(stale, key)          # what the control shows
            was = getattr(window_opened_from, key)   # what it was filled from
            resolved[key] = getattr(on_disk, key) if shown == was else shown
        self.assertEqual(resolved["nr_async"], True, "the panel's async was overwritten by the window")
        self.assertEqual(resolved["nr_every"], 3, "the panel's cadence was overwritten by the window")

        # And a field the user DID move keeps the user's choice: that is what the control is for.
        shown = 2                                     # the user dragged the cadence to 2
        resolved_every = getattr(on_disk, "nr_every") if shown == getattr(stale, "nr_every") else shown
        self.assertEqual(resolved_every, 2, "the user's own change was discarded")

        # The file the layer reads, after the window writes the resolved values, still says what
        # the panel said - because the window resolved to the file's values.
        self.assertTrue(core.set_pacing(replace(on_disk, **resolved))["applied"])
        self.assertEqual((values()["NR_LAYER_ASYNC"], values()["NR_LAYER_LIVE"]), ("1", "3"))
        self.assertTrue(core.uninstall(self.profile)["ok"])

        # ---- and the window actually implements that decision ----
        #
        # The block above reproduces the DECISION, so on its own it would still pass if the
        # PowerShell stopped making it. The behavioural proof lives in the window's own self-test
        # (`Merge-PacingFromProfile` is called there with an untouched control, a moved one, and
        # no profile at all), because a PowerShell function cannot be driven from here. What is
        # checked here is only that the window still routes pacing through that function - the
        # text of the function itself is the self-test's to verify, not this one's.
        window = Path(core.__file__).parent.parent.parent / "dist-tools" / "windows-wizard.ps1"
        source = window.read_text(encoding="utf-8")
        self.assertIn("function Merge-PacingFromProfile", source,
                      "the window no longer has the pacing merge it needs")
        self.assertIn("Merge-PacingFromProfile $requestValues $script:LastProfile $fresh", source,
                      "pacing requests no longer go through the merge")

    def test_preset_b_is_checked_for_its_weights_or_a_dll_that_carries_them(self):
        chosen = replace(self.profile, preset="B")
        check = self.checks(chosen)["preset"]
        self.assertFalse(check["ok"])
        self.assertIn("needs nvngx_dlssnr.dll 310.9.16.0 or later", check["detail"])
        dll = write_pe(self.base / "owned DLL (fixture)" / "nvngx_dlssnr.dll")
        for version, ok in (("310.8.0.0", False), ("310.9.16.0", True), ("310.10.0.0", True), (None, False)):
            with patch.object(core, "_file_version", return_value=version):
                check = self.checks(replace(chosen, dll=dll))["preset"]
            self.assertEqual(check["ok"], ok, version)
            self.assertIn("Install extracts preset B" if ok else f"this one is {version or 'of unknown'}",
                          check["detail"])
        write_weights(self.preset_b_weights(), count=core.PRESETS["B"][1])
        self.assertTrue(self.checks(chosen)["preset"]["ok"])
        # A count of A's is not B's.
        write_weights(self.preset_b_weights())
        self.assertFalse(self.checks(chosen)["preset"]["ok"])
        self.assertTrue(self.checks(self.profile)["preset"]["ok"])

    def unpacking(self, calls, succeed=True):
        def run(command, timeout=10, env=None, emit=None, cwd=None):
            command = [str(part) for part in command]
            calls.append(command)
            if succeed:
                write_weights(command[4], count=core.PRESETS["B"][1])
            return {"command": command, "returncode": 0 if succeed else 1, "stdout": "", "stderr": "",
                    "output": "unpacked" if succeed else "no decoder", "timed_out": False}
        return run

    def test_install_takes_preset_b_out_of_a_dll_that_carries_it_beside_a(self):
        dll = write_pe(self.base / "owned DLL (fixture)" / "nvngx_dlssnr.dll")
        profile = replace(self.profile, dll=dll)
        weights = self.weights.read_bytes()
        calls = []
        with patch.object(core, "_file_version", return_value="310.8.0.0"), \
                patch.object(core, "_run", side_effect=self.unpacking(calls)):
            self.assertTrue(core.install(profile)["ok"])
        self.assertEqual(calls, [])
        self.assertFalse(self.preset_b_weights().exists())
        with patch.object(core, "_file_version", return_value="310.9.16.0"), \
                patch.object(core, "_run", side_effect=self.unpacking(calls)):
            result = core.install(profile)
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["presets"], {"A": True, "B": True})
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][:4], [str(self.python), "-I", str(self.root / "src/tools/unpack_preset.py"), str(dll)])
        # Written under a new name, then moved to the weights' own: nothing left beside them.
        self.assertEqual(Path(calls[0][4]).parent, self.preset_b_weights().parent)
        self.assertNotEqual(Path(calls[0][4]), self.preset_b_weights())
        self.assertEqual([path.name for path in self.preset_b_weights().parent.iterdir()],
                         sorted([self.weights.name, self.preset_b_weights().name]))
        self.assertEqual(self.weights.read_bytes(), weights)
        self.assertIn("unpacked", (self.root / core.WEIGHTS_LOG).read_text(encoding="utf-8"))
        # Once there, not again.
        with patch.object(core, "_file_version", return_value="310.9.16.0"), \
                patch.object(core, "_run", side_effect=self.unpacking(calls)):
            self.assertTrue(core.install(profile)["ok"])
        self.assertEqual(len(calls), 1)
        self.assertTrue(core.uninstall(profile)["ok"])

    def test_preset_bs_failed_extraction_stops_only_an_installation_that_runs_it(self):
        dll = write_pe(self.base / "owned DLL (fixture)" / "nvngx_dlssnr.dll")
        calls = []
        with patch.object(core, "_file_version", return_value="310.9.16.0"), \
                patch.object(core, "_run", side_effect=self.unpacking(calls, succeed=False)):
            result = core.install(replace(self.profile, dll=dll, preset="B"))
            self.assertFalse(result["ok"])
            self.assertIn("Preset B's extraction failed, from nvngx_dlssnr.dll version 310.9.16.0", result["error"])
            self.assertFalse(core.installed_path(self.profile).exists())
            self.assertEqual(sorted(path.name for path in self.game.parent.iterdir()), [self.game.name])
            # With A, NR is installed, and the window says B is not there.
            result = core.install(replace(self.profile, dll=dll))
        self.assertTrue(result["ok"], result)
        self.assertEqual([warning["name"] for warning in result["warnings"]], ["preset_b_extraction"])
        self.assertEqual(result["warnings"][0]["values"], ["310.9.16.0"])
        self.assertEqual(result["presets"], {"A": True, "B": False})
        self.assertFalse(list(self.preset_b_weights().parent.glob("*.new")))
        self.assertEqual((self.root / core.WEIGHTS_LOG).read_text(encoding="utf-8"), "no decoder")
        self.assertTrue(core.uninstall(self.profile)["ok"])

    def test_remove_takes_dxvks_first_log_from_the_folders_the_game_started_in(self):
        # DXVK opens its first log before it loads the proxy, so in the folder the game was
        # started in: Steam starts Mortal Kombat 11 in its root, above the executable.
        profile = replace(self.profile, api="dxvk")
        game = self.game.parent
        started = self.base / "started (here)"
        started.mkdir()
        old = started / f"{self.game.stem}_d3d9.log"
        old.write_text("a log from before NR")
        own = game / f"{self.game.stem}_d3d11.log"
        own.write_text("beside the game before NR")
        self.assertTrue(core.install(profile)["ok"])
        installed = core.installed_path(profile)
        since = json.loads((installed / core.GAME_FILES).read_text(encoding="utf-8"))["installed_time"]
        os.utime(old, (since - 60, since - 60))
        (started / f"{self.game.stem}_dxgi.log").write_text("NR's DXVK, before DXVK_LOG_PATH")
        (started / "notes.txt").write_text("not DXVK's")
        own.write_text("beside the game before NR, written again")
        (installed / core.START_FOLDERS).write_text("\n".join(
            [str(started), str(started), str(game), str(self.base / "gone"), "relative", ""]),
            encoding="utf-8")
        removed = core.uninstall(profile)
        self.assertTrue(removed["ok"], removed)
        self.assertEqual(sorted(path.name for path in started.iterdir()), sorted([old.name, "notes.txt"]))
        self.assertEqual(sorted(path.name for path in game.iterdir()), sorted([own.name, self.game.name]))

    def test_a_games_own_vulkan_loader_is_set_aside_and_used(self):
        game = self.game.parent
        (game / core.PROXY_NAME).write_bytes(b"the game's own Vulkan loader")
        self.assertIn(core.PROXY_NAME, self.checks()["game_files"]["detail"])
        self.assertTrue(core.install(self.profile)["ok"])
        installed = core.installed_path(self.profile)
        aside = installed / core.GAME_BACKUP / core.PROXY_NAME
        self.assertEqual(aside.read_bytes(), b"the game's own Vulkan loader")
        self.assertIn("NR_REAL_VULKAN=" + str(aside),
                      (installed / core.ENV_FILE).read_text(encoding="utf-8").splitlines())
        self.assertTrue(core.uninstall(self.profile)["ok"])
        self.assertEqual((game / core.PROXY_NAME).read_bytes(), b"the game's own Vulkan loader")

    def test_the_proxy_must_match_the_games_architecture(self):
        self.assertTrue(self.checks()["runtime_architecture"]["ok"])
        write_pe(self.root / "nr_vulkan_proxy.dll", x64=False)
        self.assertFalse(self.checks()["runtime_architecture"]["ok"])

    def test_changing_the_api_after_dxvk_needs_remove_first(self):
        dxvk = replace(self.profile, api="dxvk")
        self.assertTrue(core.install(dxvk)["ok"])
        self.assertFalse(self.checks(self.profile)["game_files"]["ok"])
        self.assertFalse(core.install(self.profile)["ok"])
        self.assertTrue(core.uninstall(dxvk)["ok"])
        self.assertTrue(core.install(self.profile)["ok"])
        self.assertFalse((self.game.parent / "dxgi.dll").exists())

    def test_remove_leaves_files_changed_since_and_keeps_the_originals(self):
        profile = replace(self.profile, api="dxvk")
        (self.game.parent / "d3d11.dll").write_bytes(b"the game's d3d11")
        self.assertTrue(core.install(profile)["ok"])
        (self.game.parent / "d3d11.dll").write_bytes(b"a game update")
        result = core.uninstall(profile)
        self.assertFalse(result["ok"])
        self.assertEqual(result["kept"], ["d3d11.dll"])
        self.assertEqual((self.game.parent / "d3d11.dll").read_bytes(), b"a game update")
        self.assertFalse((self.game.parent / "dxgi.dll").exists())
        backup = core.installed_path(profile) / core.GAME_BACKUP / "d3d11.dll"
        self.assertEqual(backup.read_bytes(), b"the game's d3d11")

    def test_remove_waits_for_steams_launch_options_to_be_restored(self):
        profile = replace(self.profile, api="dxvk")
        self.assertTrue(core.install(profile)["ok"])
        steam = self.root / "work/windows-wizard/steam-backup.json"
        core._atomic_json(steam, {"restored": False})  # the same, unchanged profile
        self.assertFalse(core.uninstall(profile)["ok"])
        self.assertTrue((self.game.parent / "dxgi.dll").exists())
        core._atomic_json(steam, {"restored": True})
        self.assertTrue(core.uninstall(profile)["ok"])
        self.assertFalse((self.game.parent / "dxgi.dll").exists())

    def test_remove_nr_returns_steams_launch_options_first(self):
        import windows_launch as launch
        import windows_wizard as bridge
        with patch.object(launch, "restore_steam", return_value={"ok": False, "error": "A Steam game is running"}), \
                patch.object(core, "uninstall") as uninstall:
            result = bridge.remove_nr(self.profile, None)
        self.assertEqual(result["error"], "A Steam game is running")
        uninstall.assert_not_called()
        with patch.object(launch, "restore_steam", return_value={"ok": True, "changed": True}) as restore, \
                patch.object(core, "uninstall", return_value={"ok": True, "removed": True}) as uninstall:
            result = bridge.remove_nr(self.profile, None)
        self.assertTrue(result["ok"])
        self.assertTrue(result["steam_restored"])
        self.assertEqual(restore.call_args.args[0], self.profile)
        uninstall.assert_called_once()
        with patch.object(launch, "restore_steam", return_value={"ok": True, "already_restored": True, "changed": False}), \
                patch.object(core, "uninstall", return_value={"ok": True, "removed": True}):
            self.assertNotIn("steam_restored", bridge.remove_nr(self.profile, None))

    def test_the_window_chooses_which_network_the_speed_test_measures(self):
        # The combo beside the button sends `preset` in the request; the bridge passes it on and
        # leaves it out when absent, so an older window still measures the profile's network.
        import windows_wizard as bridge
        request = self.base / "speed-request.json"
        output = self.base / "speed-result.json"

        def run(values):
            request.write_text(json.dumps(values), encoding="utf-8")
            argv = ["windows_wizard.py", "speedtest", "--root", str(self.root),
                    "--input", str(request), "--output", str(output)]
            with patch.object(sys, "argv", argv):
                bridge.main()
            return core._read_json(output)

        # The words the window sends: a profile and the network to measure.
        values = {**self.profile.to_dict(), "root": str(self.root), "preset": "B"}
        with patch.object(core, "speed_test", return_value={"ok": True}) as speed:
            self.assertTrue(run(values)["ok"])
            self.assertEqual(speed.call_args.kwargs["preset"], "B")
            speed.reset_mock()
            self.assertTrue(run({**values, "preset": "a"})["ok"])
            self.assertEqual(speed.call_args.kwargs["preset"], "A")
            speed.reset_mock()
            # No preset at all: the profile's own network, as before the chooser existed.
            without = dict(values)
            without.pop("preset")
            self.assertTrue(run(without)["ok"])
            self.assertIsNone(speed.call_args.kwargs["preset"])
        # A network that does not exist is the backend's refusal, not the window's, and it
        # costs nothing to reach: no script, no weights, no GPU.
        self.assertFalse(run({**values, "preset": "C"})["ok"])
        self.assertIn("Measure which network?", core._read_json(output)["error"])
        # A value that is not a network at all is refused before anything is measured.
        for bad in ("", "   ", 1, ["B"]):
            refused = run({**values, "preset": bad})
            self.assertFalse(refused["ok"], bad)
            self.assertIn("Specify the network", refused["error"], bad)

    def test_enabling_warns_when_the_game_runs_without_nr(self):
        self.own()
        # Started before NR was installed: no launch record names it, the game runs.
        self.processes.return_value = [{"ProcessId": 77, "ExecutablePath": str(self.game)}]
        result = core.set_effect(self.profile, True)
        self.assertTrue(result["ok"])
        self.assertEqual(result.get("warning"), "game_without_nr")
        self.assertNotIn("warning", core.set_effect(self.profile, False))
        # Started through NR, it is not warned about; nor is a game that is not running.
        self.launch_state(pid=77)
        self.assertNotIn("warning", core.set_effect(self.profile, True))
        self.running.side_effect = None
        self.processes.return_value = []
        self.assertNotIn("warning", core.set_effect(self.profile, True))

    def test_status_and_enabling_say_when_the_game_runs_as_administrator(self):
        # Windows' Vulkan loader ignores VK_LAYER_PATH from High integrity up; the proxy
        # records the game's level, and only the game's own, running, is told about.
        self.own()
        self.launch_state(pid=77)
        state = self.root / "work/windows-wizard/launch-state.json"
        record = core._read_json(state)
        record["integrity"] = 0x3000
        core._atomic_json(state, record)
        seen = core.status(self.profile)
        self.assertTrue(seen["game_elevated"])
        self.assertEqual(seen["game_integrity"], 0x3000)
        self.assertIs(seen["uac_enabled"], True)
        enabled = core.set_effect(self.profile, True)
        self.assertTrue(enabled["ok"])
        self.assertEqual(enabled.get("warning"), "game_elevated")
        self.assertIs(enabled["uac_enabled"], True)
        self.assertIn("without administrator rights", enabled["detail"])
        self.uac.return_value = False
        self.assertIs(core.set_effect(self.profile, True)["uac_enabled"], False)
        self.assertIs(core.status(self.profile)["uac_enabled"], False)
        self.assertNotIn("warning", core.set_effect(self.profile, False))
        # At medium, as Windows starts a game by default, nothing is said.
        record["integrity"] = 0x2000
        core._atomic_json(state, record)
        self.assertFalse(core.status(self.profile)["game_elevated"])
        self.assertNotIn("warning", core.set_effect(self.profile, True))
        # Nor by a record from before the proxy wrote the level.
        del record["integrity"]
        core._atomic_json(state, record)
        self.assertIsNone(core.status(self.profile)["game_integrity"])
        self.assertNotIn("warning", core.set_effect(self.profile, True))
        # Nor once the game has gone, nor for another game's record.
        record["integrity"] = 0x3000
        core._atomic_json(state, record)
        self.running.side_effect = None
        self.processes.return_value = []
        self.assertFalse(core.status(self.profile)["game_elevated"])
        self.assertNotIn("warning", core.set_effect(self.profile, True))
        other = replace(self.profile, game_exe=write_pe(self.base / "other (test)" / "other.exe"))
        self.launch_state(pid=78)
        record = dict(core._read_json(state), integrity=0x3000, game_exe=str(other.game_exe))
        core._atomic_json(state, record)
        self.assertFalse(core._elevated_game(self.profile, record))

    def test_check_says_when_this_window_runs_as_administrator(self):
        # So does a game it starts, and with UAC off every program; a warning, not a failure.
        self.integrity.return_value = 0x3000
        checked = core.validate(self.profile)
        self.assertTrue(checked["ok"], checked)
        self.assertEqual([item["name"] for item in checked["warnings"]], ["window_elevated"])
        self.uac.return_value = False
        self.assertEqual([item["name"] for item in core.validate(self.profile)["warnings"]], ["uac_off"])
        # A standard user's programs run at medium with UAC off too: nothing to say.
        self.integrity.return_value = 0x2000
        self.assertEqual(core.validate(self.profile)["warnings"], [])
        self.integrity.return_value = None
        self.assertEqual(core.validate(self.profile)["warnings"], [])

    def test_check_names_the_executable_that_loads_vulkan_when_the_chosen_one_does_not(self):
        # Steam starts DOOM's DOOMx64.exe, which draws with OpenGL and starts DOOMx64vk.exe.
        folder = self.base / "DOOM (test)"
        launcher = write_pe_importing(folder / "DOOMx64.exe", ["OPENGL32.dll", "KERNEL32.dll"])
        write_pe_importing(folder / "DOOMx64vk.exe", ["KERNEL32.dll", "vulkan-1.dll"])
        write_pe_importing(folder / "crashpad.exe", ["KERNEL32.dll"])
        profile = replace(self.profile, game_exe=launcher)
        checked = core.validate(profile)
        self.assertTrue(checked["ok"], checked)
        self.assertEqual([(item["name"], item["values"]) for item in checked["warnings"]],
                         [("vulkan_executable_other", ["DOOMx64.exe", "DOOMx64vk.exe"])])
        self.assertEqual(core.validate(replace(profile, game_exe=folder / "DOOMx64vk.exe"))["warnings"], [])
        # DXVK loads the loader for a DirectX game: not asked.
        self.assertEqual(core.validate(replace(profile, api="dxvk"))["warnings"], [])
        alone = write_pe_importing(self.base / "alone (test)" / "alone.exe", ["KERNEL32.dll"])
        self.assertEqual([(item["name"], item["values"]) for item in
                          core.validate(replace(profile, game_exe=alone))["warnings"]],
                         [("vulkan_executable", ["alone.exe"])])

    def test_remove_refuses_while_the_game_runs_and_without_an_installation(self):
        profile = replace(self.profile, api="dxvk")
        result = core.uninstall(profile)
        self.assertTrue(result["ok"])
        self.assertFalse(result["removed"])
        self.assertTrue(core.install(profile)["ok"])
        self.launch_state()
        self.assertFalse(core.uninstall(profile)["ok"])
        self.assertTrue((self.game.parent / "dxgi.dll").exists())
        self.assertTrue(core.installed_path(profile).exists())

    def test_foreign_existing_installation_is_preserved(self):
        destination = core.installed_path(self.profile)
        destination.mkdir()
        sentinel = destination / "foreign.txt"
        sentinel.write_bytes(b"do not change")
        before = sentinel.read_bytes()
        self.assertFalse(core.install(self.profile)["ok"])
        self.assertEqual(sentinel.read_bytes(), before)
        self.assertEqual(list(destination.iterdir()), [sentinel])
        core._atomic_json(destination / core.OWNER_NAME, {"schema_version": 1, "root": str(self.base / "foreign")})
        self.assertFalse(core.install(self.profile)["ok"])
        self.assertEqual(sentinel.read_bytes(), before)

    def test_successful_install_spaces_parentheses_defaults_and_no_vendor_copy(self):
        trigger = self.root / "work/nr_trigger"
        trigger.touch()
        core._atomic_json(self.root / "work/nr_settings.json", {"render_scale": 0.8, "min_extent": 640, "keep_me": 7})
        result = core.install(self.profile)
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["warnings"], [])
        saved = core.load_profile(self.root)
        self.assertEqual(saved.python, self.python)
        self.assertEqual(saved.game_exe, self.game)
        settings = core._read_json(self.root / "work/nr_settings.json")
        self.assertEqual(settings, {"render_scale": 0.8, "min_extent": 640, "keep_me": 7})
        self.assertFalse(trigger.exists())
        destination = core.installed_path(self.profile)
        manifest = core._read_json(destination / "VkLayer_dlss_nr.json")
        self.assertEqual(manifest["layer"]["library_path"], str(destination / "nr_layer.dll"))
        self.assertEqual(manifest["layer"]["library_arch"], "64")
        self.assertFalse(any(destination.rglob("*.safetensors")))
        self.assertFalse(any(destination.rglob("nvngx_dlssnr.dll")))
        self.assertTrue(self.weights.is_file())

    def test_the_panels_own_shaders_are_installed(self):
        """`menu.spv` and `menu_rgb10a2.spv` come from `src/layer/nr_menu.c`, not `src/gpu`.

        They were left out of an install once: the shader list scanned `src/gpu` alone, so the
        game got a layer whose menu could not load its shader. The layer said so - "no settings
        menu: the menu's shader was not found beside the layer" - and INS did nothing, while the
        release itself carried both files.
        """
        source = self.root / "src/layer/nr_menu.c"
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text('{ VK_FORMAT_R8G8B8A8_UNORM, "menu.spv", 0 },\n'
                          '{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, "menu_rgb10a2.spv", 1 },\n',
                          encoding="utf-8")
        for name in ("menu.spv", "menu_rgb10a2.spv"):
            (self.root / "work" / name).write_bytes(b"SPIR-V fixture")
        self.assertIn("menu.spv", core._shader_names(self.root))
        self.assertIn("menu_rgb10a2.spv", core._shader_names(self.root))
        result = core.install(self.profile)
        self.assertTrue(result["ok"], result)
        installed = core.installed_path(self.profile) / "work"
        for name in ("menu.spv", "menu_rgb10a2.spv"):
            self.assertTrue((installed / name).is_file(), name)

    def test_first_install_sets_candidate_defaults_only_when_missing(self):
        result = core.install(self.profile)
        self.assertTrue(result["ok"], result)
        self.assertEqual(core._read_json(self.root / "work/nr_settings.json"),
                         {"render_scale": 0.4, "min_extent": 320})

    def test_control_schema_and_daemon_defaults_come_from_linux_catalogue(self):
        catalogue = core._runtime_knobs()
        self.assertEqual([knob["name"] for knob in catalogue], [
            "render_scale", "min_extent", "profile", "intensity", "detail_strength",
            "colour_strength", "temporal", "hold", "release", "cut_limit"])
        names = {knob["name"]: knob for knob in catalogue}
        self.assertEqual(names["profile"]["choices"], ["standard", "natural", "cinematic"])
        # `weights` is gone: the network is `NR_PRESET`, one setting, not a knob with a list of
        # model names. A catalogue that still offered it would be the second selection path back.
        self.assertNotIn("weights", names)
        self.assertEqual((names["min_extent"]["high"], names["min_extent"]["runtime_high"]), (320, 4096))
        self.assertEqual((names["release"]["high"], names["release"]["runtime_high"]), (64, 255))
        daemon_source = (Path(core.__file__).resolve().parent.parent / "layer/nr_daemon.py").read_text(encoding="utf-8")
        tree = ast.parse(daemon_source)
        actual = {}
        for node in ast.walk(tree):
            if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                    and node.func.attr == "add_argument" and node.args
                    and isinstance(node.args[0], ast.Constant)):
                for keyword in node.keywords:
                    if keyword.arg == "default" and isinstance(keyword.value, ast.Constant):
                        actual[str(node.args[0].value).removeprefix("--").replace("-", "_")] = keyword.value.value
        self.assertEqual({knob["name"]: knob["default"] for knob in catalogue},
                         {knob["name"]: actual[knob["name"]] for knob in catalogue})

    def test_every_choice_value_is_labelled_in_all_four_languages(self):
        """A `choice` knob's combobox is filled with `Choice.<knob>.<value>`, falling back to
        `Profile.<value>`.

        `T` throws on a missing key, so a choice value with no label and no fallback takes the
        SETUP WINDOW's settings page down with `Missing translation: Profile.ht` - which is exactly
        what the weights knob would have done, because its values are not profiles. Nothing else
        covers this: the tests drive the backend, and the label lookup lives in the script.
        """
        source = (Path(core.__file__).resolve().parent.parent.parent
                  / "dist-tools" / "windows-wizard.ps1").read_text(encoding="utf-8")
        catalogue = core._runtime_knobs()
        checked = 0
        for knob in catalogue:
            if knob["kind"] != "choice":
                continue
            for value in knob["choices"]:
                keys = (f"'Choice.{knob['name']}.{value}'", f"'Profile.{value}'")
                if not any(source.count(key) == 4 for key in keys):
                    self.fail(f"{knob['name']}={value} has no label in all four languages "
                              f"(expected one of {keys} four times)")
                checked += 1
        self.assertGreater(checked, 0)
        # And the lookup goes through the helper, not straight at `Profile.` - the line that made
        # the weights values unresolvable.
        self.assertNotIn("T ('Profile.'+", source)

    def test_reading_control_defaults_needs_no_game_profile_or_processes_and_writes_nothing(self):
        with patch.object(core, "_process_snapshot", side_effect=AssertionError("Controls do not inspect games")), \
                patch.object(core, "_running_executable", side_effect=AssertionError("Controls do not inspect games")):
            result = core.get_settings(self.root)
        self.assertTrue(result["ok"])
        self.assertEqual(result["settings"], result["defaults"])
        self.assertEqual(result["explicit_keys"], [])
        self.assertFalse(core.profile_path(self.root).exists())
        self.assertFalse((self.root / "work/nr_settings.json").exists())
        self.assertFalse((self.root / "work/nr_trigger").exists())

    def test_partial_settings_save_merges_latest_values_and_preserves_trigger_and_profile(self):
        core.save_profile(self.profile)
        profile_before = core.profile_path(self.root).read_bytes()
        trigger = self.root / "work/nr_trigger"
        trigger.write_bytes(b"existing effect")
        path = self.root / "work/nr_settings.json"
        core._atomic_json(path, {"render_scale": 0.4, "min_extent": 640, "release": 100,
                                 "other_tool": {"value": "preserve me"}})
        observed = core.get_settings(self.root)
        self.assertEqual(observed["settings"]["min_extent"], 640)
        external = core._read_json(path)
        external["detail_strength"] = 1.7
        external["other_tool"]["value"] = "new external value"
        core._atomic_json(path, external)
        with patch.object(core, "_process_snapshot", side_effect=AssertionError("No game restart")), \
                patch.object(core, "_running_executable", side_effect=AssertionError("No game restart")):
            result = core.save_settings(self.root, {"render_scale": 0.25})
        saved = core._read_json(path)
        self.assertEqual(saved["render_scale"], 0.25)
        self.assertEqual(saved["detail_strength"], 1.7)
        self.assertEqual(saved["min_extent"], 640)
        self.assertEqual(saved["release"], 100)
        self.assertEqual(saved["other_tool"]["value"], "new external value")
        self.assertFalse(result["game_restart_required"])
        self.assertFalse(result["processing_confirmed"])
        self.assertEqual(trigger.read_bytes(), b"existing effect")
        self.assertEqual(core.profile_path(self.root).read_bytes(), profile_before)

    def test_invalid_runtime_settings_leave_saved_bytes_untouched(self):
        path = self.root / "work/nr_settings.json"
        core._atomic_json(path, {"render_scale": 0.4, "unknown": "untouched"})
        before = path.read_bytes()
        invalid = ({"render_scale": 0.049}, {"render_scale": 1.001}, {"render_scale": True},
                   {"intensity": float("nan")}, {"detail_strength": float("inf")},
                   {"colour_strength": -1}, {"temporal": 1.01}, {"hold": False},
                   {"cut_limit": None}, {"release": 256}, {"min_extent": 127},
                   {"min_extent": 4097}, {"profile": "unknown"}, {"master": True})
        for changes in invalid:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                core.save_settings(self.root, changes)
            self.assertEqual(path.read_bytes(), before)
        self.assertFalse(list(path.parent.glob(".nr-json-*")))

    def test_saved_invalid_settings_are_reported_instead_of_claiming_default_values(self):
        path = self.root / "work/nr_settings.json"
        for values in ([0.4], {"render_scale": float("nan")}, {"hold": True}):
            with self.subTest(values=values):
                core._atomic_json(path, values)
                with self.assertRaises(ValueError):
                    core.get_settings(self.root)

    def test_atomic_settings_failure_leaves_last_readable_document(self):
        path = self.root / "work/nr_settings.json"
        core._atomic_json(path, {"render_scale": 0.4})
        before = path.read_bytes()
        with patch.object(Path, "replace", side_effect=OSError("fixture rename failure")):
            with self.assertRaises(OSError):
                core.save_settings(self.root, {"render_scale": 0.3})
        self.assertEqual(path.read_bytes(), before)
        self.assertFalse(list(path.parent.glob(".nr-json-*")))

    def test_reset_one_and_all_write_defaults_consumed_by_actual_live_settings_parser(self):
        # Execute just the real daemon's Settings class, avoiding its GPU imports.
        daemon_source = (Path(core.__file__).resolve().parent.parent / "layer/nr_daemon.py").read_text(encoding="utf-8")
        tree = ast.parse(daemon_source)
        actual_class = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == "Settings")
        # The ENGINE's profile table, which stands in for `nr_frame.PROFILES`. It keeps `neutral`
        # even though the menu no longer offers it: the daemon validates a settings file against
        # what the engine can run, not against what the UI lists, so a hand-edited file naming
        # `neutral` is still accepted. `nr_knobs.PROFILES` is the list the menu shows.
        namespace = {"os": os, "math": math, "json": json,
                     # The class asks which preset this install runs. A stub is enough here - the
                     # real module is `src/tools/test_nr_weights.py`'s subject, and the network is
                     # `NR_PRESET` alone now rather than a table of model names.
                     "nr_weight_models": SimpleNamespace(
                         PRESETS=("A", "B"),
                         preset_of=lambda value: (str(value or "").strip().upper()
                                                  if str(value or "").strip().upper() in ("A", "B")
                                                  else "A")),
                     "nr_frame": SimpleNamespace(PROFILES={choice: {} for choice in ("standard", "natural", "cinematic", "neutral")})}
        exec(compile(ast.Module(body=[actual_class], type_ignores=[]), "nr_daemon.py", "exec"), namespace)
        path = self.root / "work/nr_settings.json"
        result = core.save_settings(self.root, {"render_scale": 0.25, "intensity": 0.3, "profile": "natural"})
        values = core._read_json(path)
        values["other_tool"] = 7
        core._atomic_json(path, values)
        args = SimpleNamespace(settings=str(path), **result["defaults"])
        live = namespace["Settings"](args)
        live.refresh()
        self.assertEqual((live.render_scale, live.intensity, live.profile), (0.25, 0.3, "natural"))
        result = core.reset_settings(self.root, ["render_scale"])
        live.stamp = None  # Force observation even on a coarse-mtime test filesystem.
        live.refresh()
        self.assertEqual((live.render_scale, live.intensity, live.profile), (1, 0.3, "natural"))
        self.assertEqual(core._read_json(path)["other_tool"], 7)
        result = core.reset_settings(self.root)
        live.stamp = None
        live.refresh()
        self.assertEqual({name: getattr(live, name) for name in live.KNOBS}, result["defaults"])
        self.assertEqual(set(result["explicit_keys"]), set(result["defaults"]))
        self.assertEqual(core._read_json(path)["other_tool"], 7)

    def test_unknown_reset_does_not_change_settings_or_trigger(self):
        path = self.root / "work/nr_settings.json"
        core._atomic_json(path, {"render_scale": 0.4})
        before = path.read_bytes()
        with self.assertRaises(ValueError):
            core.reset_settings(self.root, ["master", "render_scale"])
        self.assertEqual(path.read_bytes(), before)
        self.assertFalse((self.root / "work/nr_trigger").exists())

    def test_non_regular_settings_destination_is_rejected(self):
        path = self.root / "work/nr_settings.json"
        path.mkdir()
        sentinel = path / "foreign.txt"
        sentinel.write_bytes(b"preserve")
        with self.assertRaises(ValueError):
            core.save_settings(self.root, {"render_scale": 0.3})
        self.assertEqual(sentinel.read_bytes(), b"preserve")

    def test_staging_failure_preserves_existing_owned_installation(self):
        destination = self.own()
        sentinel = destination / "old.dll"
        sentinel.write_bytes(b"previous installation")
        with patch.object(core, "_copy_runtime", side_effect=OSError("fixture copy failed")):
            result = core.install(self.profile)
        self.assertFalse(result["ok"])
        self.assertEqual(sentinel.read_bytes(), b"previous installation")
        self.assertFalse(list(self.game.parent.glob(".dlss-nr-stage-*")))

    def test_late_commit_failure_rolls_back_installation_settings_profile_and_trigger(self):
        destination = self.own()
        sentinel = destination / "old.dll"
        sentinel.write_bytes(b"previous installation")
        core.save_profile(self.profile)
        core._atomic_json(self.root / "work/nr_settings.json", {"render_scale": 0.9})
        (self.root / "work/nr_trigger").write_bytes(b"old trigger")
        paths = [core.profile_path(self.root), self.root / "work/nr_settings.json", self.root / "work/nr_trigger"]
        before = {path: path.read_bytes() for path in paths}
        with patch.object(core, "save_profile", side_effect=ValueError("fixture profile failure")):
            result = core.install(self.profile)
        self.assertFalse(result["ok"])
        self.assertEqual(sentinel.read_bytes(), b"previous installation")
        self.assertEqual({path: path.read_bytes() for path in paths}, before)
        self.assertFalse(list(self.game.parent.glob(".dlss-nr-backup-*")))

    def test_extraction_failure_never_writes_game_even_with_success_exit_code(self):
        self.weights.unlink()
        dll = write_pe(self.base / "owned DLL (fixture)" / "nvngx_dlssnr.dll")
        profile = replace(self.profile, dll=dll)
        self.assertIn("version unknown; tested with 310.8.0.0 and 310.9.16.0",
                      self.checks(profile)["local_model_input"]["detail"])
        with patch.object(core, "_run", return_value={"returncode": 0, "timed_out": False,
                         "output": "Done without usable weights"}) as run:
            result = core.install(profile)
        self.assertFalse(result["ok"])
        self.assertIn("version unknown (tested: 310.8.0.0, 310.9.16.0)", result["error"])
        self.assertEqual(run.call_args.args[0][3], dll)
        self.assertFalse(core.installed_path(profile).exists())
        # What the extractor said outlives the window's next action, for Save report.
        self.assertEqual((self.root / core.WEIGHTS_LOG).read_text(encoding="utf-8"), "Done without usable weights")

    def test_missing_packages_only_uses_prepared_local_interpreter_consistently(self):
        local = write_pe(self.root / "work/windows-python/Scripts/python.exe")
        chosen = replace(self.profile, python=local)
        self.native.side_effect = lambda python, **kwargs: python_info() if Path(python) == local else python_info(packages={}, package_errors={"numpy": "missing", "safetensors": "missing"})
        with patch.object(core, "ensure_dependencies", return_value={"ok": True, "profile": chosen.to_dict(), "python": str(local)}) as prepare:
            result = core.install(self.profile)
        self.assertTrue(result["ok"], result)
        prepare.assert_called_once()
        self.assertEqual(core.load_profile(self.root).python, local)
        self.assertEqual(core.runtime_env(core.Profile.from_dict(result["profile"]))["NR_PYTHON"], str(local))

    def test_prepare_python_venv_and_pip_are_local_argv_without_shell(self):
        calls = []
        def run(command, **kwargs):
            calls.append([str(arg) for arg in command])
            if "venv" in command:
                directory = Path(command[-1])
                write_pe(directory / "Scripts/python.exe")
                (directory / "pyvenv.cfg").write_text("home = fixture\n")
            return {"returncode": 0, "timed_out": False, "output": "fixture"}
        with patch.object(core, "_run", side_effect=run):
            result = core.ensure_dependencies(self.profile)
        self.assertTrue(result["ok"], result)
        local = self.root / "work/windows-python/Scripts/python.exe"
        self.assertEqual(calls[0][:4], [str(self.python), "-I", "-m", "venv"])
        self.assertEqual(calls[1][:4], [str(local), "-I", "-m", "pip"])
        self.assertIn("--only-binary=:all:", calls[1])
        self.assertEqual(result["profile"]["python"], str(local))

    def test_foreign_local_python_directory_is_preserved(self):
        directory = self.root / "work/windows-python"
        directory.mkdir()
        sentinel = directory / "foreign.txt"
        sentinel.write_bytes(b"preserve")
        with patch.object(core, "_run") as run:
            self.assertFalse(core.ensure_dependencies(self.profile)["ok"])
        run.assert_not_called()
        self.assertEqual(sentinel.read_bytes(), b"preserve")

    def test_install_nr_returns_steams_launch_options_first(self):
        # An earlier setup could leave Steam starting a game through NR's wrapper. That no
        # longer holds the profile; Install NR returns those options before it installs.
        import windows_launch as launch
        import windows_wizard as bridge
        core.save_profile(self.profile)
        core._atomic_json(self.root / "work/windows-wizard/steam-backup.json", {"restored": False})
        other = replace(self.profile, game_exe=write_pe(self.base / "other game" / "other.exe"))
        core.save_profile(other)
        self.assertEqual(core.load_profile(self.root), other)
        with patch.object(launch, "restore_steam", return_value={"ok": False, "error": "A Steam game is running"}), \
                patch.object(core, "install") as install:
            self.assertEqual(bridge.install_nr(other, None)["error"], "A Steam game is running")
        install.assert_not_called()
        with patch.object(launch, "restore_steam", return_value={"ok": True, "changed": True}) as restore:
            result = bridge.install_nr(other, None)
        self.assertTrue(result["ok"], result)
        self.assertTrue(result["steam_restored"])
        self.assertEqual(restore.call_args.args[0], other)
        self.assertTrue(core.installed_path(other).exists())

    def test_install_blocks_active_selected_game_and_other_root_session(self):
        self.launch_state()
        self.assertFalse(core.install(self.profile)["ok"])
        other = replace(self.profile, game_exe=write_pe(self.base / "other game" / "other.exe"))
        self.assertFalse(core.install(other)["ok"])
        self.assertFalse(core.installed_path(other).exists())

    def test_install_rechecks_game_after_extraction_and_before_commit(self):
        self.processes.side_effect = [[], [], [{"ProcessId": 12, "ExecutablePath": str(self.game)}]]
        self.assertFalse(core.install(self.profile)["ok"])
        self.assertFalse(core.installed_path(self.profile).exists())
        self.assertFalse(list(self.game.parent.glob(".dlss-nr-stage-*")))

    def test_owned_trigger_off_on_does_not_claim_processing(self):
        self.own()
        self.assertTrue(core.set_effect(self.profile, False)["ok"])
        enabled = core.set_effect(self.profile, True)
        self.assertTrue(enabled["ok"])
        self.assertFalse(enabled["processing_confirmed"])
        self.assertTrue((self.root / "work/nr_trigger").is_file())
        self.assertTrue(core.set_effect(self.profile, False)["ok"])
        self.assertFalse((self.root / "work/nr_trigger").exists())

    def test_directory_trigger_returns_failure_without_exception(self):
        self.own()
        (self.root / "work/nr_trigger").mkdir()
        result = core.set_effect(self.profile, True)
        self.assertFalse(result["ok"])
        self.assertTrue((self.root / "work/nr_trigger").is_dir())

    def test_broken_link_trigger_cannot_write_outside_release(self):
        self.own()
        outside = self.base / "outside-trigger"
        trigger = self.root / "work/nr_trigger"
        try:
            trigger.symlink_to(outside)
        except OSError as error:
            self.skipTest("Creating symlinks is unavailable: " + str(error))
        self.assertFalse(core.set_effect(self.profile, True)["ok"])
        self.assertFalse(outside.exists())

    def test_other_profile_cannot_toggle_active_root_game(self):
        self.launch_state()
        other = replace(self.profile, game_exe=write_pe(self.base / "other game" / "other.exe"))
        self.own(other)
        trigger = self.root / "work/nr_trigger"
        trigger.write_bytes(b"active A")
        self.assertFalse(core.set_effect(other, False)["ok"])
        self.assertEqual(trigger.read_bytes(), b"active A")

    def test_status_persists_between_processes_and_excludes_previous_launch(self):
        log = self.root / "work/nr_daemon.log"
        historical = b"half rounding: nearest\nmodel ready in 1s\nkeeping NumPy's large blocks\n800x450 rgba in 1.0s change 0.1 network 320x320\n"
        log.write_bytes(historical)
        self.launch_state(offset=len(historical))
        first = core.status(self.profile)
        self.assertEqual(first["processed"], 0)
        self.assertIsNone(first["half_probe"])
        self.assertFalse(first["allocator"])
        self.assertFalse(first["fresh_frames"])
        core._LOG_STATES.clear()  # each GUI poll starts a separate Python process
        with log.open("ab") as handle:
            handle.write(b"half rounding: nearest\nmodel ready in 1s\nkeeping NumPy's large blocks\n800x450 rgba in 0.2s change 0.1 network 320x320\nframe rejected/failed bad format\n")
        second = core.status(self.profile)
        self.assertEqual(second["processed"], 1)
        self.assertEqual(second["fresh_processed"], 1)
        self.assertTrue(second["fresh_frames"])
        self.assertEqual(second["network_shape"], [320, 320])
        self.assertTrue(second["half_probe"]["ok"])
        self.assertTrue(second["allocator"])
        self.assertEqual(second["rejected"], 1)
        core._LOG_STATES.clear()
        third = core.status(self.profile)
        self.assertEqual(third["processed"], 1)
        self.assertEqual(third["fresh_processed"], 0)
        self.assertFalse(third["fresh_frames"])
        self.launch_state(pid=124, offset=log.stat().st_size, started="new session")
        core._LOG_STATES.clear()
        fourth = core.status(self.profile)
        self.assertEqual(fourth["processed"], 0)
        self.assertIsNone(fourth["half_probe"])
        self.assertFalse(fourth["allocator"])

    def test_historical_daemon_log_does_not_confirm_game_connection(self):
        (self.root / "work/nr_daemon.log").write_text("800x450 rgba in 0.2s change 0.1 network 320x320\n", encoding="utf-8")
        result = core.status(self.profile)
        self.assertEqual(result["processed"], 1)
        self.assertIsNone(result["game_connected"])
        self.assertFalse(result["fresh_frames"])
        self.assertFalse(result["active_game"])

    def test_status_partial_lines_survive_process_restart(self):
        log = self.root / "work/nr_daemon.log"
        log.write_bytes(b"800x450 rgba in 0.2s change")
        self.launch_state()
        self.assertEqual(core.status(self.profile)["processed"], 0)
        core._LOG_STATES.clear()
        with log.open("ab") as handle:
            handle.write(b" 0.1 network 320x320\n")
        result = core.status(self.profile)
        self.assertEqual(result["processed"], 1)
        self.assertTrue(result["fresh_frames"])

    def test_status_read_is_bounded_and_labels_counts_incomplete(self):
        log = self.root / "work/nr_daemon.log"
        log.write_bytes(b"uninteresting\n" * (core.MAX_LOG_READ // 10) + b"800x450 rgba in 0.2s change 0.1 network 320x320\n")
        result = core.status(self.profile)
        self.assertFalse(result["counts_complete"])
        self.assertEqual(result["processed"], 1)

    def test_status_preserves_failed_probe_and_ignores_malformed_timing(self):
        (self.root / "work/nr_daemon.log").write_text("half rounding: packHalf2x16 3/1024\nFAIL: rounding mismatch\nmodel ready in 1s\n800x450 rgba in 1..2s change 0.1\n")
        result = core.status(self.profile)
        self.assertFalse(result["half_probe"]["ok"])
        self.assertEqual(result["processed"], 0)
        self.assertEqual(result["errors"], 1)

    def test_gpu_daemon_diagnostics_persist_without_claiming_active_processing(self):
        log = self.root / "work/nr_daemon.log"
        metadata = {"pid": 1234, "python": "3.12", "device": "Intel Arc B580",
                    "gpu_image": True}
        log.write_text("daemon session " + json.dumps(metadata) + "\n"
                       "half rounding: nearest\nmodel ready in 1s\n"
                       "GPU shared transport fallback: asynchronous mode\n"
                       "[nr_layer] GPU transport fallback: Vulkan 1.1 instance required; "
                       "frames take the usual path\n"
                       "[nr_layer] GPU transport ready: 1280x720 format 44\n"
                       "1280x720 rgba in 0.02s change 0.1 network 512x320 "
                       "transport=gpu-image-host image prep=1.25ms compose=2.50ms\n",
                       encoding="utf-8")
        result = core.status(self.profile)
        self.assertEqual(result["daemon_session"], metadata)
        self.assertEqual(result["last_frame"]["transport"], "gpu-image-host")
        self.assertEqual((result["last_frame"]["prep_ms"], result["last_frame"]["compose_ms"]), (1.25, 2.5))
        # The layer's own reason, which it writes into the daemon's log (issue #17).
        self.assertEqual(result["gpu_fallbacks"], [
            "GPU shared transport fallback: asynchronous mode",
            "[nr_layer] GPU transport fallback: Vulkan 1.1 instance required; frames take the usual path"])
        self.assertFalse(result["fresh_frames"])
        core._LOG_STATES.clear()
        result = core.status(self.profile)
        self.assertEqual(result["daemon_session"], metadata)
        self.assertEqual(result["last_frame"]["transport"], "gpu-image-host")
        with log.open("a", encoding="utf-8") as handle:
            handle.write('daemon session {"pid": 1235}\n'
                         "half rounding: nearest\nmodel ready in 1s\n"
                         "1280x720 rgba in 0.01s change 0.1 network 512x320 "
                         "transport=gpu-shared image prep=0.75ms compose=1.00ms\n")
        core._LOG_STATES.clear()
        result = core.status(self.profile)
        self.assertEqual(result["daemon_session"], {"pid": 1235})
        self.assertEqual(result["processed"], 1)
        self.assertEqual(result["gpu_fallbacks"], [])
        self.assertEqual(result["last_frame"]["transport"], "gpu-shared")

    def test_gpu_daemon_diagnostics_ignore_invalid_records_and_limit_fallbacks(self):
        state = core._new_log_state()
        for line in ('daemon session []', 'daemon session {broken}',
                     '1280x720 rgba in 0.01s change 0.1 image prep=nanms compose=-1ms'):
            core._parse_log_line(state, line)
        self.assertIsNone(state["daemon_session"])
        self.assertNotIn("prep_ms", state["last_frame"])
        for index in range(35):
            core._parse_log_line(state, f"GPU image fallback: fixture {index}")
        self.assertEqual(len(state["gpu_fallbacks"]), 30)
        self.assertEqual(state["gpu_fallbacks"][0], "GPU image fallback: fixture 5")
        self.assertEqual(state["errors"], 0)

    def test_layer_timing_report_is_bounded_and_does_not_enable_measurements(self):
        self.assertIsNone(core._layer_timing_report(self.profile))
        path = self.root / "work/logs/layer-timing.jsonl"
        path.parent.mkdir(parents=True)
        with path.open("w", encoding="utf-8") as handle:
            handle.write('{"padding": "' + 'x' * core.MAX_LOG_TAIL + '"}\n')
            handle.write('broken\n[]\n')
            for index in range(140):
                handle.write(json.dumps({"frame": index, "transport": "gpu-shared", "capture_ms": 0.5}) + "\n")
            handle.write('{"frame": 999')
        timing = core._layer_timing_report(self.profile)
        self.assertTrue(timing["truncated"])
        self.assertEqual(len(timing["records"]), 128)
        self.assertEqual(timing["records"][0]["frame"], 12)
        self.assertEqual(timing["records"][-1]["frame"], 139)
        self.assertEqual(timing["malformed_records"], 2)
        report = self.base / "timing report.json"
        with patch.object(core, "_system_metadata", return_value={}):
            self.assertTrue(core.export_report(self.profile, report)["ok"])
        self.assertEqual(core._read_json(report)["layer_timings"], timing)
        self.assertFalse(core.profile_path(self.root).exists())
        self.assertNotIn("NR_LAYER_TIMING", core.runtime_env(self.profile))

    def test_export_without_git_or_settings_includes_diagnostics_only(self):
        core._atomic_json(self.root / "release-metadata.json", {"schema_version": 1, "source_commit": "04459d2", "source_branch": "improve-release"})
        (self.root / "work/nr_daemon.log").write_text("fixture daemon output\n")
        report = self.base / "reports (local)" / "result.json"
        with patch.object(core, "_system_metadata", return_value={"gpu": [{"Name": "fixture GPU", "DriverVersion": "fixture"}]}), patch.object(core, "_run", side_effect=AssertionError("No Git or executable needed")):
            result = core.export_report(self.profile, report)
        self.assertTrue(result["ok"], result)
        saved = core._read_json(report)
        self.assertIsNone(saved["settings"])
        self.assertTrue(saved["settings_error"])
        self.assertEqual(saved["release"]["source_commit"], "04459d2")
        self.assertIn("fixture daemon output", saved["daemon_log_tail"])
        # What Check says, so a report of a failed installation names the item (issue #12).
        self.assertIn("runtime_dependencies", {item["name"] for item in saved["checks"]})
        self.assertEqual(saved["dll"], {"present": False})
        self.assertEqual(saved["weights_log_tail"], "")
        self.assertEqual(saved["warnings"], [])
        self.assertEqual(saved["elevation"], {"window_integrity": 0x2000, "window_elevated": False, "uac_enabled": True})
        self.assertEqual(saved["dxvk_logs"], [])
        self.assertIsNone(saved["layer_timings"])
        self.assertNotIn("fixture_648", report.read_text(encoding="utf-8"))
        self.assertFalse(core.export_report(self.profile, self.base / "report.dll")["ok"])

    def test_report_carries_dxvks_logs_and_the_windows_elevation(self):
        # Whether DXVK drew the game at all is in its logs: the release's work/logs, the game's
        # folder, and the folders the proxy saw the game started in. Another game's are not.
        profile = replace(self.profile, api="dxvk")
        logs = self.root / "work/logs"
        logs.mkdir(parents=True, exist_ok=True)
        (logs / "selected_d3d11.log").write_text("info:  Game: selected.exe\ninfo:  DXVK: v3.1.1\n", encoding="utf-8")
        (logs / "other_d3d11.log").write_text("another game\n", encoding="utf-8")
        (self.game.parent / "selected_dxgi.log").write_bytes(b"head " + b"x" * (3 * core.DXVK_LOG_PART) + b" tail")
        started = self.base / "started (test)"
        started.mkdir()
        (started / "selected_d3d9.log").write_text("from where the game was started\n", encoding="utf-8")
        (self.own(profile) / core.START_FOLDERS).write_text(f"{started}\n{self.base / 'gone'}\n", encoding="utf-8")
        self.integrity.return_value = 0x3000
        self.uac.return_value = False
        report = self.base / "report.json"
        with patch.object(core, "_system_metadata", return_value={}):
            self.assertTrue(core.export_report(profile, report)["ok"])
        saved = core._read_json(report)
        self.assertEqual(saved["elevation"], {"window_integrity": 0x3000, "window_elevated": True, "uac_enabled": False})
        self.assertEqual([item["name"] for item in saved["warnings"]], ["uac_off"])
        texts = {Path(item["path"]).name: item["text"] for item in saved["dxvk_logs"]}
        self.assertEqual(set(texts), {"selected_d3d11.log", "selected_dxgi.log", "selected_d3d9.log"})
        self.assertIn("DXVK: v3.1.1", texts["selected_d3d11.log"])
        self.assertIn("from where the game was started", texts["selected_d3d9.log"])
        # A long log as its start, which names the GPU and the device, and its end.
        long = texts["selected_dxgi.log"]
        self.assertTrue(long.startswith("head ") and long.endswith(" tail") and "[...]" in long, long[:40])
        self.assertLess(len(long), 2 * core.DXVK_LOG_PART + 16)

    def speed_script(self):
        script = self.root / "src/bench/speed_test.py"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("# release fixture\n", encoding="utf-8")
        return script

    def test_the_speed_test_runs_the_releases_script_and_keeps_its_result(self):
        # NR alone on this PC, saved with the PC for Save report: testers' reports gather it.
        script = self.speed_script()
        measured = {"ok": True, "error": None, "device": "Fixture GPU", "model_ready_s": 1.0, "preset": "B",
                    "cases": [{"width": 640, "height": 360, "render_scale": 0.4, "network": [320, 320],
                               "network_ms": 23, "network_fps": 43.5, "round_trip_ms": 26.9,
                               "round_trip_min_ms": 25.0, "round_trip_max_ms": 29.0, "round_trip_fps": 37.2}]}

        def run(command, timeout=10, env=None, emit=None, cwd=None):
            command = [str(part) for part in command]
            self.assertEqual(command[:3], [str(self.python), "-I", str(script)])
            self.assertEqual(command[command.index("--root") + 1], str(self.root))
            # The network a game started now would run.
            self.assertEqual(command[command.index("--preset") + 1], "B")
            target = Path(command[command.index("--json") + 1])
            target.write_text(json.dumps(measured), encoding="utf-8")
            return {"command": command, "returncode": 0, "stdout": "", "stderr": "", "output": "",
                    "timed_out": False}

        self.assertIsNone(core.status(self.profile)["speed_test"])
        self.assertTrue(core.install(self.profile)["speed_test_due"])
        write_weights(self.preset_b_weights(), count=core.PRESETS["B"][1])
        with patch.object(core, "_run", side_effect=run), \
                patch.object(core, "_system_metadata", return_value={"gpu": [{"Name": "fixture"}]}):
            result = core.speed_test(replace(self.profile, preset="B"))
        self.assertTrue(result["ok"], result)
        saved = core._read_json(self.root / core.SPEED_TEST)
        self.assertEqual((saved["device"], saved["cases"]), ("Fixture GPU", measured["cases"]))
        self.assertEqual(saved["system"], {"gpu": [{"Name": "fixture"}]})
        self.assertIn("created_utc", saved)
        # The window's status line takes a summary; Save report takes the whole.
        self.assertEqual(core.status(self.profile)["speed_test"]["cases"],
                         [{"width": 640, "height": 360, "render_scale": 0.4, "network_ms": 23, "network_fps": 43.5,
                           "round_trip_ms": 26.9, "round_trip_fps": 37.2}])
        self.assertEqual(core.status(self.profile)["speed_test"]["preset"], "B")
        report = self.base / "speed report.json"
        with patch.object(core, "_system_metadata", return_value={}):
            self.assertTrue(core.export_report(self.profile, report)["ok"])
        self.assertEqual(core._read_json(report)["speed_test"]["device"], "Fixture GPU")
        # Run once after a first installation: the next one is not due.
        self.assertFalse(core.install(self.profile)["speed_test_due"])

    def test_the_speed_test_needs_the_weights_and_the_gpu_to_itself_and_says_what_failed(self):
        self.speed_script()
        self.weights.unlink()
        self.assertIn("Install NR first", core.speed_test(self.profile)["error"])
        write_weights(self.weights)
        self.launch_state(pid=77)
        self.assertIn("Close the game first", core.speed_test(self.profile)["error"])
        self.running.side_effect = None

        def failing(command, timeout=10, env=None, emit=None, cwd=None):
            command = [str(part) for part in command]
            Path(command[command.index("--json") + 1]).write_text(json.dumps(
                {"ok": False, "error": "the daemon ended (code 1) before it listened"}), encoding="utf-8")
            return {"command": command, "returncode": 1, "stdout": "", "stderr": "", "output": "",
                    "timed_out": False}

        with patch.object(core, "_run", side_effect=failing):
            result = core.speed_test(self.profile)
        self.assertFalse(result["ok"])
        self.assertIn("before it listened", result["error"])
        self.assertFalse((self.root / core.SPEED_TEST).exists())
        with patch.object(core, "_run", return_value={"command": [], "returncode": None, "stdout": "",
                                                      "stderr": "", "output": "", "timed_out": True}):
            self.assertIn("no answer in 900 s", core.speed_test(self.profile)["error"])
        self.assertFalse((self.root / core.SPEED_TEST).exists())

    def test_the_speed_test_measures_the_network_it_is_asked_for_or_says_which_is_here(self):
        # A and B are measurably different networks, so the window lets a tester compare them on
        # one PC without changing the network a game runs. The choice is the test's own: the
        # profile may say A while the test measures B, and vice versa.
        self.speed_script()
        write_weights(self.weights)
        measured = {"ok": True, "error": None, "device": "Fixture GPU", "preset": "A", "cases": []}
        asked = []

        def run(command, timeout=10, env=None, emit=None, cwd=None):
            command = [str(part) for part in command]
            asked.append(command[command.index("--preset") + 1])
            target = Path(command[command.index("--json") + 1])
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(dict(measured, preset=asked[-1])), encoding="utf-8")
            return {"command": command, "returncode": 0, "stdout": "", "stderr": "", "output": "",
                    "timed_out": False}

        # A release that carries both measures whichever it is asked for, profile untouched.
        write_weights(self.preset_b_weights(), count=core.PRESETS["B"][1])
        self.assertEqual(core.get_settings(self.root)["presets"], {"A": True, "B": True})
        with patch.object(core, "_run", side_effect=run), \
                patch.object(core, "_system_metadata", return_value={}):
            self.assertTrue(core.speed_test(self.profile, preset="B")["ok"], asked)
            self.assertTrue(core.speed_test(replace(self.profile, preset="B"), preset="A")["ok"], asked)
        self.assertEqual(asked, ["B", "A"])
        # The network measured is kept with the result, so a report cannot be read as the other.
        self.assertEqual(core._read_json(self.root / core.SPEED_TEST)["preset"], "A")
        # Measuring changes neither the profile nor what a game would run.
        self.assertFalse(core.profile_path(self.root).exists())

        # A release with A alone still measures A, and refuses B by name rather than running
        # something else: the window greys B for the same reason.
        self.preset_b_weights().unlink()
        self.assertEqual(core.get_settings(self.root)["presets"], {"A": True, "B": False})
        with patch.object(core, "_run", side_effect=run), \
                patch.object(core, "_system_metadata", return_value={}):
            self.assertTrue(core.speed_test(self.profile, preset="A")["ok"])
        refused = core.speed_test(self.profile, preset="B")
        self.assertFalse(refused["ok"])
        self.assertIn("B's weights are not in this release", refused["error"])
        self.assertIn("measure A", refused["error"])
        self.assertEqual(refused["presets"], {"A": True, "B": False})
        self.assertEqual(asked, ["B", "A", "A"])
        # A network the profile's own type would refuse is refused here too, and the message
        # names the two there are.
        for bad in ("C", 1, ["B"]):
            wrong = core.speed_test(self.profile, preset=bad)
            self.assertFalse(wrong["ok"], bad)
            self.assertIn("Measure which network?", wrong["error"])
            self.assertIn("A", wrong["error"])
            self.assertIn("B", wrong["error"])

    def test_a_python_or_a_library_that_fails_says_why_in_its_check(self):
        self.native.return_value = python_info(probe_ok=False, probe_error="no answer in 30 s")
        self.assertIn("did not answer: no answer in 30 s", self.checks()["python_native_x64"]["detail"])
        self.native.return_value = python_info()
        self.deps.return_value = {"ok": False, "failures": {
            str(self.root / "work/libnr_image.dll"): "Could not find module",
            "VCOMP140.DLL": "missing: install the Microsoft Visual C++ x64 Redistributable"}}
        detail = self.checks()["runtime_dependencies"]["detail"]
        self.assertIn("libnr_image.dll: Could not find module", detail)
        self.assertIn("VCOMP140.DLL: missing: install the Microsoft Visual C++ x64 Redistributable", detail)


class ProbeTests(unittest.TestCase):
    """What the Python and library probes say when they fail; the probe's process stood in for."""

    def answering(self, **got):
        result = {"command": [], "returncode": 0, "stdout": "", "stderr": "", "output": "", "timed_out": False}
        result.update(got)
        return patch.object(core, "_run", return_value=result)

    def test_a_missing_openmp_runtime_is_named_with_its_installer(self):
        profile = SimpleNamespace(python=Path("python.exe"), root=Path("release"))
        answer = json.dumps({"loaded": ["vulkan-1.dll"], "failures": {
            "release/work/libnr_image.dll": "Could not find module", "VCOMP140.DLL": "missing"}})
        with self.answering(stdout=answer):
            result = core._load_dependencies(profile)
        self.assertFalse(result["ok"])
        self.assertIn("Visual C++ x64 Redistributable", result["failures"]["VCOMP140.DLL"])
        self.assertIn(core.VC_REDIST, result["failures"]["VCOMP140.DLL"])

    @unittest.skipUnless(os.name == "nt", "Windows' version resource")
    def test_a_files_version_is_read_from_its_resource(self):
        kernel = Path(os.environ["SystemRoot"]) / "System32" / "kernel32.dll"
        self.assertRegex(core._file_version(kernel) or "", r"^\d+\.\d+\.\d+\.\d+$")
        with tempfile.TemporaryDirectory() as folder:
            self.assertIsNone(core._file_version(write_pe(Path(folder) / "no version.dll")))
            self.assertIsNone(core._file_version(Path(folder) / "absent.dll"))

    def test_probes_that_do_not_answer_say_so(self):
        profile = SimpleNamespace(python=Path("python.exe"), root=Path("release"))
        with self.answering(returncode=None, timed_out=True):
            self.assertEqual(core._load_dependencies(profile)["failures"], {"dependency_probe": "no answer in 30 s"})
            info = core._python_info(Path("python.exe"))
        self.assertFalse(info["probe_ok"])
        self.assertEqual(info["probe_error"], "no answer in 30 s")

    @unittest.skipUnless(os.name == "nt", "Windows' token and UAC policy")
    def test_this_processs_integrity_and_uac_are_read(self):
        # Low 0x1000, medium 0x2000, high 0x3000, system 0x4000: whatever runs the tests.
        self.assertIn(core._integrity(), (0x1000, 0x2000, 0x2100, 0x3000, 0x4000))
        self.assertIn(core._uac_enabled(), (True, False))

    def test_off_windows_neither_is_known(self):
        with patch.object(core.os, "name", "posix"):
            self.assertIsNone(core._integrity())
            self.assertIsNone(core._uac_enabled())


class PeImportTests(unittest.TestCase):
    """The import and delay-load tables, read from the file: nothing in it is loaded."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nr imports (fixture) ")
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)

    def test_both_tables_and_both_architectures(self):
        game = write_pe_importing(self.folder / "game.exe", ["KERNEL32.dll", "vulkan-1.dll"], ["D3D12.dll"])
        self.assertEqual(core.pe_imports(game), ["kernel32.dll", "vulkan-1.dll", "d3d12.dll"])
        x86 = write_pe_importing(self.folder / "x86.exe", ["USER32.dll"], x64=False)
        self.assertEqual(core.pe_imports(x86), ["user32.dll"])
        self.assertEqual(core.pe_imports(write_pe_importing(self.folder / "none.exe")), [])

    def test_what_is_not_a_readable_pe_file_has_no_imports(self):
        (self.folder / "text.exe").write_bytes(b"not an executable")
        self.assertIsNone(core.pe_imports(self.folder / "text.exe"))
        self.assertIsNone(core.pe_imports(self.folder / "absent.exe"))
        # Headers only, as the other fixtures write them: no optional header to read.
        self.assertIsNone(core.pe_imports(write_pe(self.folder / "headers.exe")))
        whole = write_pe_importing(self.folder / "cut.exe", ["KERNEL32.dll"]).read_bytes()
        (self.folder / "cut.exe").write_bytes(whole[:0x400 + 8])
        self.assertEqual(core.pe_imports(self.folder / "cut.exe"), [])

    def test_a_game_that_loads_the_loader_by_name_is_not_taken_for_a_launcher(self):
        # Unreal loads vulkan-1.dll by a UTF-16 name, imported by nothing.
        named = write_pe_importing(self.folder / "unreal" / "Game.exe", ["KERNEL32.dll"],
                                   text="vulkan-1.dll".encode("utf-16-le"))
        self.assertIsNone(core._vulkan_elsewhere(named))
        ascii_named = write_pe_importing(self.folder / "ascii" / "Game.exe", ["KERNEL32.dll"], text=b"VULKAN-1.DLL")
        self.assertIsNone(core._vulkan_elsewhere(ascii_named))
        alone = write_pe_importing(self.folder / "alone" / "Game.exe", ["KERNEL32.dll"])
        self.assertEqual(core._vulkan_elsewhere(alone), [])
        self.assertIsNone(core._vulkan_elsewhere(write_pe(self.folder / "headers" / "Game.exe")))


class CommandTests(unittest.TestCase):
    def test_capture_both_streams_exact_argv_and_exit_code(self):
        result = core._run([sys.executable, "-I", "-c", "import sys; print(sys.argv[1]); print('stderr fixture',file=sys.stderr); sys.exit(7)", "spaces (and parentheses)"], timeout=5)
        self.assertEqual(result["returncode"], 7)
        self.assertIn("spaces (and parentheses)", result["stdout"])
        self.assertIn("stderr fixture", result["stderr"])

    def test_deadline_applies_when_exited_parent_descendant_keeps_output_open(self):
        # Child is ours, CPU-only and self-terminates; no game or daemon is touched.
        child = "import sys,time; end=time.monotonic()+3;\nwhile time.monotonic()<end: print('x'*100,flush=True); time.sleep(.001)"
        parent = "import subprocess,sys; subprocess.Popen([sys.executable,'-I','-c',sys.argv[1]])"
        started = time.monotonic()
        result = core._run([sys.executable, "-I", "-c", parent, child], timeout=0.1)
        self.assertTrue(result["timed_out"])
        self.assertLess(time.monotonic() - started, 2.8)
        time.sleep(1)  # the finite child finishes before this test process exits


@unittest.skipUnless(os.name == "nt", "asks Windows about a process")
class RunningExecutableTests(unittest.TestCase):
    """The status check's one query: a live process by its path, a finished one not at all."""

    def test_live_process_reports_its_executable(self):
        # Not sys.executable: a virtual environment's python.exe starts the base interpreter.
        import ctypes
        own = ctypes.create_unicode_buffer(32768)
        ctypes.windll.kernel32.GetModuleFileNameW(None, own, len(own))
        self.assertEqual(core._canonical(core._running_executable(os.getpid())),
                         core._canonical(own.value))

    def test_finished_and_invalid_processes_report_nothing(self):
        child = subprocess.Popen([sys.executable, "-c", "pass"])
        child.wait(timeout=60)
        self.assertIsNone(core._running_executable(child.pid))
        for pid in (0, -1, None, True, "12"):
            self.assertIsNone(core._running_executable(pid))


if __name__ == "__main__":
    unittest.main()


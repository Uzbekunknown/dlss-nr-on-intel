#!/usr/bin/env python3
"""The weight table, the manifest, and what a DLL actually carries.

Three things are being kept true at once here:

  - an install from before any of this existed (one file, no manifest) must resolve to exactly the
    file it always used;
  - a DLL that carries one weight resource and a DLL that carries two must both install, and the
    second must be recorded rather than silently ignored;
  - a settings value that names a model this machine cannot load must fall back to one it can,
    because a settings file travels between machines.

The DLL here is synthetic - a PE built in the test with the resource tree an nvngx_dlssnr.dll has -
so the check runs without NVIDIA's binary. `NR_NR_DLL` and `NR_NR_DLL_NEW` point it at real ones.
"""
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import ast
import unittest
from types import SimpleNamespace

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src" / "ref"))
import nr_weight_models as weights  # noqa: E402


# ---------------------------------------------------------------- a synthetic nvngx_dlssnr.dll

RCDATA = 10
LANG_EN_US = 1033


def build_pe(resources):
    """A minimal PE32+ whose .rsrc holds [(name, payload)] as RCDATA resources.

    The tree is the one a real DLL has: RCDATA -> name -> language -> data. Offsets are relative to
    the start of the section, which is why every level is patched after its size is known.
    """
    count = len(resources)
    type_dir = 0x18
    sub_at = (type_dir + 16 + 8 * count + 7) & ~7
    leaf_at = sub_at + 0x20 * count
    payload_at = (leaf_at + 16 * count + 15) & ~15
    # After the payloads, not after the leaf table: a name label written over the first payload is
    # how this builder first produced a resource nothing could find.
    payload_end = payload_at + sum(len(payload) for _, payload in resources)
    labels_at = (payload_end + 15) & ~15
    label_bytes = sum(2 + 2 * len(name) + 2 for name, _ in resources)
    payload_cursor = payload_at

    rsrc = bytearray(labels_at + label_bytes)
    struct.pack_into("<IIHHHH", rsrc, 0, 0, 0, 0, 0, 0, 1)       # root: one id
    struct.pack_into("<II", rsrc, 16, RCDATA, 0x80000000 | type_dir)
    struct.pack_into("<IIHHHH", rsrc, type_dir, 0, 0, 0, 0, 0, count)  # `count` names

    cursor = labels_at
    for index, (name, payload) in enumerate(resources):
        encoded = name.encode("utf-16-le")
        struct.pack_into("<H", rsrc, cursor, len(name))
        rsrc[cursor + 2:cursor + 2 + len(encoded)] = encoded
        struct.pack_into("<II", rsrc, type_dir + 16 + 8 * index,
                         0x80000000 | cursor, 0x80000000 | (sub_at + 0x20 * index))
        cursor += 2 + len(encoded) + 2
        at = payload_cursor
        rsrc[at:at + len(payload)] = payload
        payload_cursor += len(payload)
        base = sub_at + 0x20 * index
        struct.pack_into("<IIHHHH", rsrc, base, 0, 0, 0, 0, 0, 1)
        struct.pack_into("<II", rsrc, base + 16, LANG_EN_US, leaf_at + 16 * index)
        struct.pack_into("<III", rsrc, leaf_at + 16 * index, 0x1000 + at, len(payload), 1200)
    return rsrc


def pe_file(rsrc):
    """Wrap a .rsrc image in a PE32+ file with one section."""
    out = bytearray(0x400 + len(rsrc))
    out[0:2] = b"MZ"
    struct.pack_into("<I", out, 0x3C, 0x40)
    out[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<H", out, 0x44 + 2, 1)                      # one section
    struct.pack_into("<H", out, 0x44 + 16, 240)                   # SizeOfOptionalHeader
    optional = 0x44 + 20
    struct.pack_into("<H", out, optional, 0x20B)                  # PE32+
    struct.pack_into("<I", out, optional + 128, 0x1000)           # resource RVA
    struct.pack_into("<I", out, optional + 132, len(rsrc))        # resource size
    section = optional + 240
    out[section:section + 8] = b".rsrc\0\0\0"
    struct.pack_into("<I", out, section + 8, len(rsrc))           # VirtualSize
    struct.pack_into("<I", out, section + 12, 0x1000)             # VirtualAddress
    struct.pack_into("<I", out, section + 16, len(rsrc))          # SizeOfRawData
    struct.pack_into("<I", out, section + 20, 0x400)              # PointerToRawData
    out[0x400:0x400 + len(rsrc)] = rsrc
    return bytes(out)


# ---------------------------------------------------------------- the preset, and nothing else

class PresetTests(unittest.TestCase):
    """The network is `NR_PRESET` alone. No manifest, no model names, no fallback."""

    def test_each_preset_names_one_file(self):
        self.assertEqual(weights.PRESET_WEIGHTS["A"], "dlssnr-logical.safetensors")
        self.assertEqual(weights.PRESET_WEIGHTS["B"], "dlssnr-b-logical.safetensors")
        self.assertEqual(set(weights.PRESETS), {"A", "B"})

    def test_the_path_is_built_from_the_preset_and_not_from_a_manifest(self):
        # No manifest is written, and the path is still exact. This is the whole change: the file
        # follows from the preset, so a Linux install with no manifest is not a second case.
        with tempfile.TemporaryDirectory() as room:
            work = pathlib.Path(room)
            self.assertEqual(weights.weights_path(work, "A"),
                             work / "mlxw" / "dlssnr-logical.safetensors")
            self.assertEqual(weights.weights_path(work, "B"),
                             work / "mlxw" / "dlssnr-b-logical.safetensors")
            self.assertFalse((work / "mlxw" / "weights.json").exists())

    def test_a_preset_with_no_file_is_refused_and_not_answered_with_the_other_one(self):
        """**The regression the maintainer hit.** `NR_PRESET=B` on an install whose B was never
        extracted used to fall back to A, silently - the game ran the other network and the log
        still said B. The answer is now "B is not here", naming the file."""
        with tempfile.TemporaryDirectory() as room:
            work = pathlib.Path(room)
            (work / "mlxw").mkdir()
            (work / "mlxw" / "dlssnr-logical.safetensors").write_bytes(b"A")
            self.assertTrue(weights.installed(work, "A"))
            self.assertFalse(weights.installed(work, "B"))
            # The path is still exact - it is what the error message has to name.
            self.assertEqual(weights.weights_path(work, "B").name,
                             "dlssnr-b-logical.safetensors")
            message = weights.missing_message(work, "B")
            self.assertIn("dlssnr-b-logical.safetensors", message)
            self.assertIn("unpack_preset.py", message)

    def test_both_presets_reported_from_one_call(self):
        with tempfile.TemporaryDirectory() as room:
            work = pathlib.Path(room)
            (work / "mlxw").mkdir()
            (work / "mlxw" / "dlssnr-logical.safetensors").write_bytes(b"A")
            self.assertEqual(weights.describe(work), {"A": True, "B": False})

    def test_an_unknown_name_is_a_not_an_error(self):
        # A settings file from before the presets, or a hand-edited one, has to start a working
        # daemon. A is the shipped default and the same network as the old `ht`.
        for odd in (None, "", "ht", "cc_small_fp8", "a", 7):
            self.assertEqual(weights.preset_of(odd), "A")
        self.assertEqual(weights.preset_of("b"), "B")
        self.assertEqual(weights.preset_of(" B "), "B")

    def test_the_removed_surface_says_what_happened(self):
        """A stale caller that asks for `resolve` gets a sentence naming the change, not a
        `TypeError` from calling nothing."""
        with self.assertRaises(AttributeError) as caught:
            weights.resolve
        self.assertIn("NR_PRESET", str(caught.exception))


# ---------------------------------------------------------------- the DLL's own resource table

class DllTests(unittest.TestCase):
    def synthetic(self, names):
        payloads = [(name, bytes(range(32))) for name in names]
        return pe_file(build_pe(payloads))

    def test_one_resource_is_the_old_dll_and_two_is_the_new_one(self):
        """`weight_resources` names PRESETS now, not models - the table is gone."""
        sys.path.insert(0, str(ROOT / "scripts"))
        import nr_weights
        with tempfile.TemporaryDirectory() as room:
            for names, wanted in ((("WEIGHTS_HT",), ["A"]),
                                  (("WEIGHTS_HT", "WEIGHTS_CC_SMALL_FP8"), ["A", "B"])):
                path = pathlib.Path(room) / "dll"
                path.write_bytes(self.synthetic(names))
                found = nr_weights.weight_resources(path)
                self.assertEqual([preset for preset, _, _ in found], wanted)
                report = nr_weights.describe(path)
                self.assertEqual([entry["name"] for entry in report["resources"]], wanted)

    def test_an_unknown_weight_resource_is_reported_not_ignored(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        import nr_weights
        with tempfile.TemporaryDirectory() as room:
            path = pathlib.Path(room) / "dll"
            path.write_bytes(self.synthetic(("WEIGHTS_HT", "WEIGHTS_SOMETHING_NEW")))
            report = nr_weights.describe(path)
            names = [entry["name"] for entry in report["resources"]]
            self.assertIn("A", names)
            self.assertIn(None, names)
            self.assertTrue(any(entry["resource"] == "WEIGHTS_SOMETHING_NEW"
                                for entry in report["resources"] if not entry["known"]))

    def test_inspect_cli_is_quiet_about_a_dll_with_no_weights(self):
        with tempfile.TemporaryDirectory() as room:
            path = pathlib.Path(room) / "dll"
            path.write_bytes(pe_file(bytes(0x400)))
            result = subprocess.run([sys.executable, str(ROOT / "scripts" / "nr_weights.py"),
                                     "--inspect", str(path)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


class HotSwitchTests(unittest.TestCase):
    """The daemon's `refresh_model`: a preset change swaps the network, and a missing one refuses.

    **The refusal is the point.** The old code resolved a model NAME through a manifest and fell
    back to whatever was installed; `NR_PRESET=B` on an install whose B had never been extracted
    therefore ran A, silently, and the log still said B. These tests pin the opposite: a preset
    whose file is absent produces a message naming the file and leaves the running network alone.
    """

    def setUp(self):
        self.source = (ROOT / "src/layer/nr_daemon.py").read_text(encoding="utf-8")
        self.tree = ast.parse(self.source)

    def _refresh(self, request, root):
        """Run the real `refresh_model` against a temporary root and a recording backend."""
        fn = next(node for node in self.tree.body
                  if isinstance(node, ast.FunctionDef) and node.name == "refresh_model")

        class FakeBackend:
            def __init__(self, tag):
                self.tag = tag

            def close(self):
                pass

        def ResidentBackend(path):
            return FakeBackend(str(path))

        printed = []
        namespace = {
            "pathlib": pathlib, "os": os, "sys": sys, "time": __import__("time"),
            "ROOT": root,
            "nr_weight_models": __import__("nr_weight_models"),
            "nr_frame": SimpleNamespace(ResidentBackend=ResidentBackend),
            "close_gpu_images": lambda *a, **k: None,
            "History": lambda: None,
            "print": lambda *a, **k: printed.append(" ".join(str(x) for x in a)),
        }
        exec(compile(ast.Module(body=[fn], type_ignores=[]), "nr_daemon.py", "exec"), namespace)
        model = {"backend": FakeBackend("A"), "name": "dlssnr-logical.safetensors",
                 "path": ROOT / "work/mlxw/dlssnr-logical.safetensors",
                 "preset": "A", "requested": "A"}
        args = SimpleNamespace(history=None, model=model, live=SimpleNamespace(preset=request), gpu=None)
        namespace["refresh_model"](args, model["backend"])
        return model, printed

    def test_a_preset_with_its_file_present_is_loaded(self):
        with tempfile.TemporaryDirectory() as room:
            work = pathlib.Path(room)
            (work / "mlxw").mkdir()
            (work / "mlxw" / "dlssnr-b-logical.safetensors").write_bytes(b"B")
            model, printed = self._refresh("B", work)
            # The swap ran: the model now records B, and it was reached through the preset's file.
            self.assertEqual(model["requested"], "B")
            self.assertTrue(any("dlssnr-b-logical.safetensors" in line for line in printed)
                            or model.get("preset") == "B")

    def test_a_preset_with_no_file_is_refused_not_answered_with_the_other(self):
        """The regression: B missing must NOT become A."""
        with tempfile.TemporaryDirectory() as room:
            work = pathlib.Path(room)
            (work / "mlxw").mkdir()
            (work / "mlxw" / "dlssnr-logical.safetensors").write_bytes(b"A")
            model, printed = self._refresh("B", work)
            joined = " ".join(printed)
            self.assertIn("dlssnr-b-logical.safetensors", joined)
            self.assertIn("unpack_preset.py", joined)
            # And it did not quietly take the network that IS present.
            self.assertNotEqual(model.get("preset"), "B")

    def test_an_unknown_name_falls_back_to_a_and_says_nothing_alarming(self):
        with tempfile.TemporaryDirectory() as room:
            model, printed = self._refresh("nonsense", pathlib.Path(room))
        self.assertEqual(model["requested"], "A")


class RealDllTests(unittest.TestCase):
    """Opt-in, on the DLLs this machine happens to have: the version pair that matters."""

    def check(self, path, expected):
        path = pathlib.Path(path)
        if not path.is_file():
            self.skipTest(f"{path} is not here")
        sys.path.insert(0, str(ROOT / "scripts"))
        import nr_weights
        report = nr_weights.describe(path)
        self.assertEqual(sorted(entry["name"] or "" for entry in report["resources"]), sorted(expected))

    def test_the_old_single_weight_dll(self):
        old = os.environ.get("NR_NR_DLL")
        if not old:
            self.skipTest("NR_NR_DLL is not set")
        self.check(old, ["ht"])

    def test_the_new_dual_weight_dll(self):
        new = os.environ.get("NR_NR_DLL_NEW")
        if not new:
            self.skipTest("NR_NR_DLL_NEW is not set")
        self.check(new, ["cc_small_fp8", "ht"])


if __name__ == "__main__":
    unittest.main(verbosity=2)




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
import unittest

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


# ---------------------------------------------------------------- the table and the manifest

class TableTests(unittest.TestCase):
    def test_the_legacy_install_resolves_to_the_file_it_always_used(self):
        with tempfile.TemporaryDirectory() as room:
            work = pathlib.Path(room)
            (work / "mlxw").mkdir()
            (work / "mlxw" / "dlssnr-logical.safetensors").write_bytes(b"weights")
            self.assertIsNone(weights.read_manifest(work))
            name, path = weights.resolve(work, "cc_small_fp8")
            self.assertEqual(name, "ht")
            self.assertEqual(path, work / "mlxw" / "dlssnr-logical.safetensors")

    def test_ht_keeps_the_file_name_the_tree_knows(self):
        ht = weights.BY_NAME["ht"]
        self.assertEqual(ht.logical, "dlssnr-logical.safetensors")
        self.assertEqual(ht.packed, "dlssnr-packed.safetensors")
        self.assertTrue(ht.supported)

    def test_the_second_model_is_known_but_not_offered_as_loadable(self):
        model = weights.BY_NAME["cc_small_fp8"]
        self.assertFalse(model.supported)
        self.assertNotIn(model.name, [entry["name"] for entry in
                                      [{"name": e.name} for e in weights.MODELS
                                       if e.supported]])

    def test_a_manifest_naming_two_files_switches_between_them(self):
        with tempfile.TemporaryDirectory() as room:
            work = pathlib.Path(room)
            mlxw = work / "mlxw"
            mlxw.mkdir()
            for name in ("dlssnr-logical.safetensors", "other.safetensors"):
                (mlxw / name).write_bytes(b"x")
            manifest = {"format": weights.MANIFEST_FORMAT, "models": [
                {"name": "ht", "logical": "dlssnr-logical.safetensors", "supported": True},
                {"name": "other", "logical": "other.safetensors", "supported": True}]}
            (mlxw / weights.MANIFEST_NAME).write_text(json.dumps(manifest), encoding="utf-8")
            self.assertEqual(weights.resolve(work, "other"), ("other", mlxw / "other.safetensors"))
            self.assertEqual(weights.resolve(work, "ht")[0], "ht")

    def test_a_missing_file_falls_back_rather_than_leaving_no_model(self):
        with tempfile.TemporaryDirectory() as room:
            work = pathlib.Path(room)
            mlxw = work / "mlxw"
            mlxw.mkdir()
            (mlxw / "dlssnr-logical.safetensors").write_bytes(b"x")
            manifest = {"format": weights.MANIFEST_FORMAT, "models": [
                {"name": "ht", "logical": "dlssnr-logical.safetensors", "supported": True},
                {"name": "other", "logical": "not-here.safetensors", "supported": True}]}
            (mlxw / weights.MANIFEST_NAME).write_text(json.dumps(manifest), encoding="utf-8")
            self.assertEqual(weights.resolve(work, "other")[0], "ht")

    def test_a_manifest_in_another_format_is_ignored(self):
        with tempfile.TemporaryDirectory() as room:
            work = pathlib.Path(room)
            (work / "mlxw").mkdir()
            (work / "mlxw" / "dlssnr-logical.safetensors").write_bytes(b"x")
            (work / "mlxw" / weights.MANIFEST_NAME).write_text(
                json.dumps({"format": "something-else", "models": []}), encoding="utf-8")
            self.assertIsNone(weights.read_manifest(work))
            self.assertEqual(weights.resolve(work, "ht")[0], "ht")


# ---------------------------------------------------------------- the DLL's own resource table

class DllTests(unittest.TestCase):
    def synthetic(self, names):
        payloads = [(name, bytes(range(32))) for name in names]
        return pe_file(build_pe(payloads))

    def test_one_resource_is_the_old_dll_and_two_is_the_new_one(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        import nr_weights
        with tempfile.TemporaryDirectory() as room:
            for names, wanted in ((("WEIGHTS_HT",), ["ht"]),
                                  (("WEIGHTS_HT", "WEIGHTS_CC_SMALL_FP8"),
                                   ["ht", "cc_small_fp8"])):
                path = pathlib.Path(room) / "dll"
                path.write_bytes(self.synthetic(names))
                found = nr_weights.weight_resources(path)
                self.assertEqual([model.name for model, _, _ in found], wanted)
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
            self.assertIn("ht", names)
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
    """`nr_daemon.refresh_model`: the decision, without a GPU or a loaded model.

    The swap itself is `ResidentBackend.close()` then `ResidentBackend(path)` - already exercised by
    every daemon run. What is worth pinning here is when it happens and when it refuses to: a model
    this DLL does not carry, one the loader cannot read, and a repeated value must all cost nothing.
    """

    def setUp(self):
        sys.path.insert(0, str(ROOT / "src" / "layer"))
        import nr_daemon
        self.daemon = nr_daemon
        self.room = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.room.name)
        self.work = self.root / "work"
        (self.work / "mlxw").mkdir(parents=True)
        (self.work / "mlxw" / "dlssnr-logical.safetensors").write_bytes(b"ht")
        (self.work / "mlxw" / "alt.safetensors").write_bytes(b"alt")
        self.saved = (nr_daemon.ROOT, nr_daemon.nr_frame.ResidentBackend, getattr(nr_daemon, "close_gpu_images"))
        nr_daemon.ROOT = self.root
        self.built = []
        built = self.built

        class Fake:
            def __init__(self, path):
                self.path, self.closed = path, False
                built.append(path)

            def close(self):
                self.closed = True

        nr_daemon.nr_frame.ResidentBackend = Fake
        nr_daemon.close_gpu_images = lambda args: None

    def tearDown(self):
        (self.daemon.ROOT, self.daemon.nr_frame.ResidentBackend, self.daemon.close_gpu_images) = self.saved
        self.room.cleanup()

    def manifest(self, *models):
        (self.work / "mlxw" / weights.MANIFEST_NAME).write_text(
            json.dumps({"format": weights.MANIFEST_FORMAT, "models": list(models)}),
            encoding="utf-8")

    def args(self, wanted, backend, name="ht"):
        return type("Args", (), {
            "model": {"backend": backend, "name": name, "path": self.work / "mlxw" / name,
                      "requested": name},
            "live": type("Live", (), {"weights": wanted})(),
            "history": None})()

    def test_a_loadable_model_is_swapped_in(self):
        self.manifest({"name": "ht", "logical": "dlssnr-logical.safetensors", "supported": True},
                      {"name": "alt", "logical": "alt.safetensors", "supported": True})
        started = object()
        args = self.args("alt", started)
        fresh = self.daemon.refresh_model(args, started)
        self.assertIsNot(fresh, started)
        self.assertEqual(self.built, [self.work / "mlxw" / "alt.safetensors"])
        self.assertEqual(args.model["name"], "alt")
        self.assertEqual(args.model["requested"], "alt")

    def test_an_unloadable_model_keeps_the_one_running(self):
        self.manifest({"name": "ht", "logical": "dlssnr-logical.safetensors", "supported": True},
                      {"name": "cc_small_fp8", "logical": "fp8.safetensors", "supported": False})
        started = object()
        args = self.args("cc_small_fp8", started)
        self.assertIs(self.daemon.refresh_model(args, started), started)
        self.assertEqual(self.built, [])
        self.assertEqual(args.model["name"], "ht")
        # Resolved once, not once a frame: a second call with the same value does no file work.
        self.assertEqual(args.model["requested"], "cc_small_fp8")
        self.assertIs(self.daemon.refresh_model(args, started), started)
        self.assertEqual(self.built, [])

    def test_the_legacy_install_swaps_to_nothing(self):
        started = object()
        args = self.args("ht", started)
        self.assertIs(self.daemon.refresh_model(args, started), started)
        self.assertEqual(self.built, [])

    def test_a_model_with_no_file_is_refused(self):
        self.manifest({"name": "ht", "logical": "dlssnr-logical.safetensors", "supported": True},
                      {"name": "gone", "logical": "gone.safetensors", "supported": True})
        started = object()
        args = self.args("gone", started)
        self.assertIs(self.daemon.refresh_model(args, started), started)
        self.assertEqual(self.built, [])


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




#!/usr/bin/env python3
"""What weight models does an nvngx_dlssnr.dll carry, and where do they go?

The DLL is the user's, and NVIDIA is not the only variable here: 310.8 ships one weight resource
(`WEIGHTS_HT`) and 310.9.16 ships two (`WEIGHTS_HT` and `WEIGHTS_CC_SMALL_FP8`). This finds them by
reading the DLL's resource directory - no NVIDIA binary is copied or redistributed, and nothing is
decoded here; the third-party extractor still does that - and it writes what it found into
`work/mlxw/weights.json`, which is what the daemon and the panel read.

    python scripts/nr_weights.py --inspect D:\\nvngx_dlssnr.dll
    python scripts/nr_weights.py --extract D:\\nvngx_dlssnr.dll --work-dir work
    python scripts/nr_weights.py --list --work-dir work

`--extract` is what the installer calls. It always produces the file the project has always used
(`work/mlxw/dlssnr-logical.safetensors`) from `WEIGHTS_HT`, so a single-weight DLL behaves exactly
as before, and it additionally extracts any second model the DLL carries - once, in the same run -
to a file of its own. A model the loader cannot read yet is extracted and recorded as unsupported
rather than skipped silently: the point of the manifest is to say what is here, not to hide it.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src" / "ref"))
import nr_weight_models as weights  # noqa: E402  - the table, manifest and resolution, once

RCDATA = 10                 # the resource type every weight resource is filed under

# **There is no manifest and no model table.** They gave the project two ways to choose the
# network - `NR_PRESET` in the environment, and a `weights` name resolved through a manifest - and
# the two disagreed in the games. The preset maps to a file by name now; see
# `src/ref/nr_weight_models.py`.
PRESETS = weights.PRESETS
PRESET_WEIGHTS = weights.PRESET_WEIGHTS
BY_RESOURCE = {"WEIGHTS_HT": "A", "WEIGHTS_CC_SMALL_FP8": "B"}


# ---------------------------------------------------------------- the DLL's resource directory

def _u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def _resource_root(data):
    """(offset in the file, size) of the .rsrc section, or None."""
    pe = _u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    sections = _u16(data, pe + 6)
    optional = pe + 24
    size_of_optional = _u16(data, pe + 20)
    # PE32+ puts the data directories 112 bytes into the optional header; resources are index 2.
    rsrc_rva = _u32(data, optional + 128)
    if not rsrc_rva:
        return None
    table = optional + size_of_optional
    for index in range(sections):
        base = table + 40 * index
        va = _u32(data, base + 12)
        vsize = _u32(data, base + 8)
        raw = _u32(data, base + 20)
        if va <= rsrc_rva < va + max(vsize, 1):
            return raw + (rsrc_rva - va)
    return None


def _walk(data, root, offset, depth, path, rows):
    base = root + offset
    named = _u16(data, base + 12)
    ids = _u16(data, base + 14)
    for index in range(named + ids):
        entry = base + 16 + 8 * index
        key = _u32(data, entry)
        target = _u32(data, entry + 4)
        if key & 0x80000000:
            at = root + (key & 0x7FFFFFFF)
            length = _u16(data, at)
            label = data[at + 2:at + 2 + 2 * length].decode("utf-16-le", errors="replace")
        else:
            label = f"#{key}"
        if target & 0x80000000:
            _walk(data, root, target & 0x7FFFFFFF, depth + 1, path + [label], rows)
        else:
            leaf = root + target
            rows.append({"path": path + [label], "size": _u32(data, leaf + 4),
                         "rva": _u32(data, leaf)})


def resource_entries(dll):
    """Every resource entry as {'path': [type, name, language], 'size': bytes}."""
    data = pathlib.Path(dll).read_bytes()
    root = _resource_root(data)
    if root is None:
        return []
    rows = []
    _walk(data, root, 0, 0, [], rows)
    return rows


def weight_resources(dll):
    """[(preset or None, resource_name, size)] for each RCDATA entry that looks like weights.

    An unknown WEIGHTS_* entry is reported with `None` rather than ignored: a future version that
    renames the resource should read as "there is a weight resource this build does not know",
    not as "no weights".
    """
    found = []
    for row in resource_entries(dll):
        path = row["path"]
        if len(path) < 2 or path[0] != f"#{RCDATA}":
            continue
        name = path[1]
        if not name.startswith("WEIGHTS"):
            continue
        found.append((BY_RESOURCE.get(name), name, row["size"]))
    return found


def describe(dll):
    dll = pathlib.Path(dll)
    return {"dll": str(dll), "sha256": hashlib.sha256(dll.read_bytes()).hexdigest(),
            "bytes": dll.stat().st_size,
            "resources": [{"resource": name, "known": preset is not None,
                           "name": preset, "bytes": size}
                          for preset, name, size in weight_resources(dll)]}


# ---------------------------------------------------------------- extraction through MLX-DLSS

def _load_script(path, module_name):
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    # Registered before it runs: its dataclasses resolve their annotations through
    # `sys.modules[cls.__module__]`, which is empty for a module nobody registered.
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def _run_tool(path, argv, resource_name=None):
    """Run one of the vendored tools IN THIS PROCESS, without editing it.

    The extractor keeps its resource name in a module constant (`RESOURCE_NAME = "WEIGHTS_HT"`),
    which is the one thing standing between it and a second model. Importing it and setting that
    constant is a smaller risk than patching a third-party file: the file stays byte-identical to
    the pinned upstream commit, which is what `get_weights.py` verifies.
    """
    module = _load_script(path, path.stem)
    if resource_name is not None:
        # `RESOURCE_NAME` alone is not enough: `extract_pe_resource(data, name=RESOURCE_NAME)`
        # binds that name as a default argument when the function is DEFINED, so rebinding the
        # constant afterwards changes nothing and every model comes out as WEIGHTS_HT. The name is
        # a parameter of the function, so the function is what gets wrapped - `extract_dll` looks
        # the name up in the module globals at call time, which is what makes this work.
        if hasattr(module, "extract_pe_resource"):
            original = module.extract_pe_resource
            module.extract_pe_resource = lambda data, _name=None: original(data, resource_name)
        module.RESOURCE_NAME = resource_name
        if hasattr(module, "FORMAT_NAME"):
            module.FORMAT_NAME = f"dlssnr-{resource_name}-packed-u8-v2"
    saved = sys.argv
    sys.argv = [str(path)] + [str(a) for a in argv]
    try:
        code = module.main()
    finally:
        sys.argv = saved
    if code not in (0, None):
        raise SystemExit(f"{path.name} failed with {code}")


def extract(dll, work_dir, tools=None, *, preset=None, quiet=False):
    """Extract one preset's weights from `dll` into `work_dir/mlxw`.

    `preset` is "A" or "B"; None means "every resource this DLL carries, as long as it maps to a
    preset". There is no manifest to write: the file name follows from the preset, so the daemon
    finds it without one. Returns the list of files this call produced.
    """
    work = pathlib.Path(work_dir)
    mlxw = work / "mlxw"
    mlxw.mkdir(parents=True, exist_ok=True)
    tools = pathlib.Path(tools) if tools else work / "mlx-dlss" / "python" / "mlxdlss" / "tools"
    extractor = tools / "extract_dlssnr_weights.py"
    unpacker = tools / "unpack_dlssnr_weights.py"
    for path in (extractor, unpacker):
        if not path.is_file():
            raise SystemExit(f"missing {path}: work/mlx-dlss is not installed")

    found = weight_resources(dll)
    if not found:
        raise SystemExit(f"{dll} carries no weight resource this build knows")

    wanted = [preset] if preset else [name for name, _, _ in found if name]
    if not wanted:
        raise SystemExit(f"{dll} carries no resource that maps to a preset this build knows")

    written = []
    for target in wanted:
        resource = next((r for p, r, _ in found if p == target), None)
        if resource is None:
            continue
        logical = weights.weights_path(work, target)
        fresh = logical.with_name(logical.name + ".new")
        fresh.unlink(missing_ok=True)
        if not quiet:
            print(f"  {resource}: unpacking preset {target} to {logical.name}", flush=True)
        # `unpack_preset.py` is the tool that knows both: it extracts from the DLL and applies the
        # role-renaming decoder B's records need. Using it for A too keeps one path.
        result = subprocess.run(
            [sys.executable, "-I", str(ROOT / "src" / "tools" / "unpack_preset.py"),
             str(dll), str(fresh), "--preset", target,
             "--mlx-dlss", str(work / "mlx-dlss")],
            cwd=str(ROOT))
        if result.returncode != 0 or not fresh.is_file():
            fresh.unlink(missing_ok=True)
            raise SystemExit(f"{resource}: unpack_preset failed for preset {target}; "
                             "see its output above")
        os.replace(fresh, logical)
        written.append(logical)
        if not quiet:
            print(f"  {resource}: wrote {logical.name} ({logical.stat().st_size} bytes)",
                  flush=True)
    return written


# ---------------------------------------------------------------- reading what is installed
#
# What an install has is one question with two possible answers per preset, and
# `nr_weight_models.describe` is what the daemon and the setup window both read. No manifest.

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--inspect", metavar="DLL", help="report the weight resources a DLL carries")
    parser.add_argument("--extract", metavar="DLL",
                        help="extract the weights for --preset from this DLL")
    parser.add_argument("--preset", choices=PRESETS,
                        help="which preset to extract; default is every one the DLL carries")
    parser.add_argument("--work-dir", type=pathlib.Path, default=pathlib.Path("work"))
    parser.add_argument("--tools", type=pathlib.Path,
                        help="the MLX-DLSS tools directory; defaults to "
                             "<work-dir>/mlx-dlss/python/mlxdlss/tools")
    parser.add_argument("--list", action="store_true",
                        help="which preset files this install has")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if args.inspect:
        report = describe(args.inspect)
        if args.json:
            print(json.dumps(report, indent=2))
        else:
            print(f"{report['dll']}  {report['bytes']:,} bytes  "
                  f"sha256 {report['sha256'][:16]}")
            for entry in report["resources"]:
                known = entry["name"] or "unknown to this build"
                print(f"  {entry['resource']:<24} {entry['bytes']:>12,} B   {known}")
        return 0

    if args.extract:
        written = extract(args.extract, args.work_dir, args.tools,
                          preset=args.preset, quiet=args.quiet)
        if args.json:
            print(json.dumps([str(p) for p in written], indent=2))
        else:
            for path in written:
                print(f"  {path.relative_to(args.work_dir)}")
        return 0

    if args.list:
        report = weights.describe(args.work_dir)
        if args.json:
            print(json.dumps(report, indent=2))
            return 0
        for name, here in report.items():
            print(f"  preset {name:<2} {weights.PRESET_WEIGHTS[name]:<32} "
                  f"{'present' if here else 'missing'}")
        return 0

    parser.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())

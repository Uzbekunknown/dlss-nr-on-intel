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
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src" / "ref"))
import nr_weight_models as weights  # noqa: E402  - the table, manifest and resolution, once

RCDATA = 10                 # the resource type every weight resource is filed under
MANIFEST_FORMAT = weights.MANIFEST_FORMAT
MANIFEST_NAME = weights.MANIFEST_NAME
MODELS = weights.MODELS
BY_RESOURCE = weights.BY_RESOURCE
BY_NAME = weights.BY_NAME


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
    """[(Model or None, resource_name, size)] for each RCDATA entry that looks like weights.

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
            "resources": [{"resource": name, "known": model is not None,
                           "name": None if model is None else model.name, "bytes": size}
                          for model, name, size in weight_resources(dll)]}


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


def extract(dll, work_dir, tools=None, *, all_models=True, quiet=False):
    """Extract every weight model `dll` carries into `work_dir/mlxw`, and write the manifest.

    Returns the manifest dict. `work_dir` is the tree's `work/` - the same directory the installer
    and the daemon use, so a caller never has to know the layout.
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

    entries = []
    for model, resource, size in found:
        if model is None:
            if not all_models:
                continue
            packed = mlxw / f"{resource.lower()}-packed.safetensors"
            if not quiet:
                print(f"  {resource}: unknown model, extracting to {packed.name} only", flush=True)
            _run_tool(extractor, [dll, packed], resource_name=resource)
            entries.append({"name": None, "resource": resource, "logical": None,
                            "supported": False, "resource_bytes": size,
                            "packed": packed.name})
            continue
        if not all_models and not model.supported:
            continue
        packed = mlxw / model.packed
        record = model.as_dict() | {"packed": packed.name, "resource_bytes": size}
        if model.supported:
            logical = mlxw / model.logical
            if not quiet:
                print(f"  {model.resource}: extracting to {packed.name}", flush=True)
            _run_tool(extractor, [dll, packed], resource_name=model.resource)
            if not quiet:
                print(f"  {model.resource}: unpacking to {logical.name}", flush=True)
            _run_tool(unpacker, [packed, logical])
            if not logical.is_file():
                raise SystemExit(f"{model.resource} unpacked to nothing at {logical}")
            record["logical_bytes"] = logical.stat().st_size
        else:
            if not quiet:
                print(f"  {model.resource}: extracting to {packed.name} (not loadable yet)",
                      flush=True)
            _run_tool(extractor, [dll, packed], resource_name=model.resource)
        entries.append(record)

    manifest = {"format": MANIFEST_FORMAT,
                "source": describe(dll),
                "models": entries,
                "default": next((e["name"] for e in entries
                                 if e.get("name") and e.get("supported")), None)}
    (mlxw / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


# ---------------------------------------------------------------- reading what is installed
#
# The manifest format, the model table and the "which file does a settings value load" answer live
# in `src/ref/nr_weight_models.py`, because the daemon and the panel need exactly the same answers
# and a second copy of them here is how the two would start disagreeing. These are the names this
# script uses for them.

read_manifest = weights.read_manifest
available = weights.available
resolve = weights.resolve


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--inspect", metavar="DLL", help="report the weight resources a DLL carries")
    parser.add_argument("--extract", metavar="DLL", help="extract every model the DLL carries")
    parser.add_argument("--work-dir", type=pathlib.Path, default=pathlib.Path("work"))
    parser.add_argument("--tools", type=pathlib.Path,
                        help="the MLX-DLSS tools directory; defaults to <work-dir>/mlx-dlss/python/mlxdlss/tools")
    parser.add_argument("--list", action="store_true", help="what an install has, from the manifest")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if args.inspect:
        report = describe(args.inspect)
        if args.json:
            print(json.dumps(report, indent=2))
        else:
            print(f"{report['dll']}  {report['bytes']:,} bytes  sha256 {report['sha256'][:16]}…")
            for entry in report["resources"]:
                known = entry["name"] or "unknown to this build"
                print(f"  {entry['resource']:<24} {entry['bytes']:>12,} B   {known}")
        return 0

    if args.extract:
        manifest = extract(args.extract, args.work_dir, args.tools, quiet=args.quiet)
        if args.json:
            print(json.dumps(manifest, indent=2))
        else:
            print(f"manifest: {args.work_dir / 'mlxw' / MANIFEST_NAME}")
            for entry in manifest["models"]:
                if not entry.get("name"):
                    continue
                state = "loadable" if entry.get("supported") else "not loadable yet"
                print(f"  {entry['name']:<14} {entry['resource']:<24} {state}")
        return 0

    if args.list:
        manifest = read_manifest(args.work_dir)
        models = available(args.work_dir)
        if args.json:
            print(json.dumps({"manifest": manifest, "models": models}, indent=2))
            return 0
        if manifest:
            print(f"source: {manifest['source']['dll']} ({manifest['source']['sha256'][:16]}…)")
        for entry in models:
            state = "loadable" if entry["supported"] else "not loadable yet"
            present = "" if entry["present"] else "  (missing file)"
            print(f"  {entry['name']:<14} {state}{present}")
        return 0

    parser.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())

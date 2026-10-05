#!/usr/bin/env python3
"""Assemble the built runtime for setup.sh/setup.bat, without model weights.

Both deploy scripts call this so Windows and Linux use the same completeness
checks. This copies an existing build; it does not compile or extract weights.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = ("layer", "ref", "gpu", "bench")
IGNORE = shutil.ignore_patterns(
    ".git", ".venv", "__pycache__", "*.pyc", "*.pyo", "*.o", "*.obj",
    "*.lib", "*.exp", "*.dll", "*.exe", "*.so*", "*.spv", "*.safetensors",
    "*.bin", "*.npy", "*.npz", "*.ptx", "*.cubin", "*.fatbin", "*.png",
    "*.jpg", "*.jpeg", "*.webp", "*.mp4", "*.webm", "*.pt", "*.pth",
    "*.onnx", "*.gguf", "*.dylib", "*.sys", "*.bmp", "*.gif", "*.7z",
    "*.zip", "*.zst", "*.tar", "*.gz",
)


def required_shaders(root: Path) -> set[str]:
    """Use the filenames the runtime loads, plus the daemon's startup probe."""
    names = {"half_probe.spv"}
    for source in ("src/gpu/libxmx.c", "src/gpu/xmx.py", "src/gpu/xmxres.py"):
        names.update(re.findall(r"([A-Za-z0-9_]+\.spv)",
                                (root / source).read_text(encoding="utf-8")))
    return names


def assemble(root: Path, target: Path, platform: str) -> Path:
    root, target = root.resolve(), target.resolve()
    if platform not in ("linux", "windows"):
        raise ValueError(f"Unsupported platform: {platform}")
    # Never merge a release with stale weights, builds or user files.
    if target.exists() and (not target.is_dir() or any(target.iterdir())):
        raise ValueError(f"Release destination must be new or empty: {target}")
    for source in (root / "src", root / "work", root / "scripts", root / "dist-tools"):
        if target == source or source in target.parents or target in source.parents:
            raise ValueError(f"Release destination overlaps source files: {target}")

    windows = platform == "windows"
    layer = "nr_layer.dll" if windows else "libnr_layer.so"
    libraries = ("libxmx.dll", "libnr_image.dll", "libnr_alloc.dll") if windows else (
        "libxmx.so", "libnr_image.so")
    shaders = required_shaders(root)
    package = root / "work/mlx-dlss/python/mlxdlss"
    files = [root / "work" / name for name in (layer, *libraries, *sorted(shaders))]
    files += [
        root / "src/layer/nr_daemon.py", root / "src/ref/nr_frame.py",
        root / "src/layer/VkLayer_dlss_nr.json", root / "scripts/get_weights.py",
        root / "dist-tools/setup.sh", root / "dist-tools/setup.bat",
        root / "docs/RELEASE-QUICKSTART.md", root / "LICENSE", root / "NOTICE",
        root / "work/mlx-dlss/LICENSE", package / "features.py",
        package / "tools/extract_dlssnr_weights.py",
        package / "tools/unpack_dlssnr_weights.py",
    ]
    missing = [str(path.relative_to(root)) for path in files if not path.is_file()]
    if missing:
        raise ValueError("Incomplete build; missing: " + ", ".join(missing))

    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".nr-release-", dir=target.parent) as room:
        stage = Path(room) / "payload"
        (stage / "work").mkdir(parents=True)
        (stage / "scripts").mkdir()
        for name in SOURCE_DIRS:
            shutil.copytree(root / "src" / name, stage / "src" / name, ignore=IGNORE)
        # Runtime + extractor only, without the upstream checkout's .git or assets.
        shutil.copytree(package, stage / "work/mlx-dlss/python/mlxdlss", ignore=IGNORE)
        shutil.copy2(root / "work/mlx-dlss/LICENSE", stage / "work/mlx-dlss/LICENSE")
        shutil.copy2(root / "work" / layer, stage / ("nr_layer.dll" if windows else "nr_layer.so"))
        for name in (*libraries, *sorted(shaders)):
            shutil.copy2(root / "work" / name, stage / "work" / name)
        (stage / "work/nr_settings.json").write_text(
            json.dumps({"render_scale": 0.4, "min_extent": 320}) + "\n", encoding="utf-8")
        shutil.copy2(root / "src/layer/VkLayer_dlss_nr.json", stage / "VkLayer_dlss_nr.json")
        shutil.copy2(root / "scripts/get_weights.py", stage / "scripts/get_weights.py")
        for name in ("setup.sh", "setup.bat"):
            shutil.copy2(root / "dist-tools" / name, stage / name)
        (stage / "setup.sh").chmod(0o755)
        for name in ("LICENSE", "NOTICE"):
            shutil.copy2(root / name, stage / name)
        shutil.copy2(root / "docs/RELEASE-QUICKSTART.md", stage / "README.md")
        # All copying succeeds before the destination becomes a release. rmdir refuses
        # to remove a destination another process filled while we were assembling.
        if target.exists():
            target.rmdir()
        stage.rename(target)
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new or empty release directory")
    parser.add_argument("--platform", choices=("linux", "windows"),
                        default="windows" if os.name == "nt" else "linux")
    args = parser.parse_args()
    try:
        target = assemble(ROOT, args.output, args.platform)
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 3
    print(f"Done. Release folder: {target}")
    print("No model weights or NVIDIA DLL included. Read README.md and run the setup script.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

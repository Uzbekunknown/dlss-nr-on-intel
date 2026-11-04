#!/usr/bin/env python3
"""Put the Python a Windows release runs on where the release takes it from: work/python.

A Windows release carries its own interpreter, so a player installs no Python and setup downloads
no packages: python.org's embeddable CPython for x64, with numpy and safetensors unpacked beside
it from their PyPI wheels. Each of the three files is pinned in dist-tools/python.json by name,
URL and the SHA-256 python.org or PyPI publishes for it, and is refused unless it is that file.
Nothing in them is changed. The embeddable package's ._pth gains one line, `Lib\\site-packages`,
the folder the wheels are unpacked into, as pip would place these two.

That ._pth also makes the interpreter isolated, as -I would: no PYTHONPATH, no user site-packages,
no registry, and not the script's own folder on sys.path (`sys.flags.safe_path`). A script the
release runs therefore puts the folders it imports from on sys.path itself, as nr_daemon.py and
windows_wizard.py do.

    python scripts/fetch_python.py                       # does nothing if work/python is current
    python scripts/fetch_python.py --files X:\\downloads  # the three pinned files, already there
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PIN = ROOT / "dist-tools/python.json"
# What work/python was made from; a pin that names anything else makes it again.
MARKER = "NR-PYTHON.json"
# The ._pth line, as Windows' Python reads it, and the folder it names.
SITE_LINE = "Lib\\site-packages"
SITE = Path("Lib", "site-packages")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def made_from(pinned: dict) -> dict:
    """The marker's content: the interpreter's version, each package's, and every file's hash."""
    return {"python": pinned["python"]["version"],
            "packages": {wheel["package"]: wheel["version"] for wheel in pinned["wheels"]},
            "sha256": {entry["name"]: entry["sha256"] for entry in (pinned["python"], *pinned["wheels"])}}


def current(target: Path, pinned: dict) -> bool:
    try:
        marker = json.loads((target / MARKER).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return marker == made_from(pinned) and (target / "python.exe").is_file()


def obtain(entry: dict, room: Path, files: Path | None) -> Path:
    """The pinned file, from `files` or downloaded into `room`, once its SHA-256 is the pin's."""
    name = entry["name"]
    if files is not None:
        path = files / name
        if not path.is_file():
            raise ValueError(f"{name} is not in {files}")
    else:
        path = room / name
        with urllib.request.urlopen(entry["url"], timeout=300) as response, path.open("wb") as out:
            shutil.copyfileobj(response, out)
    if sha256(path) != entry["sha256"]:
        raise ValueError(f"{path} is not the pinned {name}: its SHA-256 differs from {PIN.name}'s")
    return path


def unpack(archive: Path, destination: Path) -> None:
    """Every member, refusing one that would land outside `destination` or that a wheel keeps
    for pip to move elsewhere (`*.data/`), which a plain unpack would leave in the wrong place."""
    destination.mkdir(parents=True, exist_ok=True)
    inside = destination.resolve()
    with zipfile.ZipFile(archive) as zipped:
        for member in zipped.infolist():
            landing = (inside / member.filename).resolve()
            if landing != inside and inside not in landing.parents:
                raise ValueError(f"{archive.name}: {member.filename} would land outside {destination}")
            if archive.suffix == ".whl" and member.filename.split("/")[0].endswith(".data"):
                raise ValueError(f"{archive.name}: {member.filename} is for pip to place, not an unpack")
        zipped.extractall(destination)


def fetch(target: Path, files: Path | None = None, pin: Path = PIN) -> Path:
    pinned = json.loads(pin.read_text(encoding="utf-8"))
    target = target.resolve()
    if current(target, pinned):
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".python-", dir=target.parent) as room:
        room = Path(room)
        stage = room / "python"
        unpack(obtain(pinned["python"], room, files), stage)
        found = list(stage.glob("python3*._pth"))
        if len(found) != 1 or not (stage / "python.exe").is_file():
            raise ValueError(f"{pinned['python']['name']} is not an embeddable package: no python.exe "
                             "and one python3*._pth at its top")
        lines = found[0].read_text(encoding="utf-8").splitlines()
        if SITE_LINE not in lines:
            found[0].write_text("\n".join(lines + [SITE_LINE]) + "\n", encoding="utf-8")
        for wheel in pinned["wheels"]:
            unpack(obtain(wheel, room, files), stage / SITE)
        (stage / MARKER).write_text(json.dumps(made_from(pinned), indent=2) + "\n", encoding="utf-8")
        if target.exists():
            shutil.rmtree(target)
        stage.rename(target)
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--files", type=Path, help="a folder holding the three pinned files")
    parser.add_argument("--target", type=Path, default=ROOT / "work/python")
    args = parser.parse_args()
    try:
        target = fetch(args.target, args.files)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 3
    marker = json.loads((target / MARKER).read_text(encoding="utf-8"))
    print(f"Python {marker['python']} with " + ", ".join(f"{name} {version}" for name, version
                                                       in marker["packages"].items()) + f" in {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

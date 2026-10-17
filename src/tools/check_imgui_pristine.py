"""Is the vendored Dear ImGui unmodified?

`NOTICE` says the vendored ImGui is "used unmodified - no patch is applied to it". That is a claim a
reviewer should be able to check rather than take on trust, and it is worth being true: an unmodified
library can be replaced on upgrade without re-applying anything, and a reader comparing it against
upstream gets the upstream they expect.

Compared against the upstream 1.93.0 ZIP the vendored copy came from, not over the network - the
archive is the exact artefact, and a fetch that times out proves nothing either way.

    python src/tools/check_imgui_pristine.py [path/to/imgui-1.93.0.zip]

Exits 0 when every vendored file is byte-identical, 1 otherwise, listing what differs. The files
upstream ships that this project does not vendor (examples, misc, the demo) are reported but are not
failures: the build names the files it compiles.
"""
from __future__ import annotations

import hashlib
import pathlib
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
VENDORED = ROOT / "third_party" / "imgui"
DEFAULT_ARCHIVE = pathlib.Path(r"F:\Downloads\imgui-1.93.0.zip")


def compare(archive: pathlib.Path) -> int:
    z = zipfile.ZipFile(archive)
    prefix = "imgui-1.93.0/"
    upstream: dict[str, bytes] = {}
    for name in z.namelist():
        if not name.startswith(prefix) or name.endswith("/"):
            continue
        # The archive keeps the backend under `backends/`; the vendored copy puts it at the top level
        # beside the core, because the build names the files it compiles rather than a directory.
        rel = name[len(prefix):]
        if rel.startswith("backends/"):
            rel = rel[len("backends/"):]
        upstream.setdefault(rel, z.read(name))

    same, differ, extra = [], [], []
    vendored = sorted(p for p in VENDORED.rglob("*") if p.is_file())
    for p in vendored:
        rel = p.relative_to(VENDORED).as_posix()
        if rel not in upstream:
            extra.append(rel)
            continue
        a = hashlib.sha256(p.read_bytes()).hexdigest()
        b = hashlib.sha256(upstream[rel]).hexdigest()
        (same if a == b else differ).append(rel)

    for n in sorted(same):
        print(f"  identical  {n}")
    for n in differ:
        print(f"  DIFFERS    {n}")
    for n in extra:
        print(f"  NOT UPSTREAM  {n}")

    not_vendored = [n for n in sorted(upstream) if not (VENDORED / n).exists()]
    print()
    print(f"  identical: {len(same)}/{len(vendored)}")
    print(f"  upstream files not vendored: {len(not_vendored)} "
          f"(examples, misc, the demo - the build names what it compiles)")

    if differ or extra:
        print()
        print("NOT pristine - restore the files, or correct the NOTICE sentence")
        return 1
    print()
    print("the vendored ImGui is unmodified - NOTICE is accurate")
    return 0


def main() -> int:
    archive = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ARCHIVE
    if not archive.is_file():
        print(f"no upstream archive to compare against: {archive}")
        print("Pass the path to imgui-1.93.0.zip.")
        return 2
    print(f"=== vendored ImGui against {archive.name} ===")
    print()
    return compare(archive)


if __name__ == "__main__":
    raise SystemExit(main())

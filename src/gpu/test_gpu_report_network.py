#!/usr/bin/env python3
"""The GPU report's network (`src/bench/gpu_report_network.py`): the networks it builds are the
real ones' shapes, its weights are E4M3 values, and it runs.

- The tensors it makes for A and B, from MLX-DLSS's spec and `nr_topology`, against the real
  logical weights' headers, name for name, shape for shape and dtype for dtype, where
  `work/mlxw` holds them (that half is skipped where it does not).
- Every half it makes is an E4M3 value, as every value of NVIDIA's large matrices is, so the
  runtime keeps the bottleneck's weights as bytes for it too and the frame it times is a game's.
- Network B at 640x360, two frames, prints its table and exits 0; exit 77 (skipped) where no
  device here runs the network.
"""
import json
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src" / "bench"))
sys.path.insert(0, str(ROOT / "src" / "gpu"))
import gpu_report_network as G  # noqa: E402
import xmxres  # noqa: E402

SCRIPT = ROOT / "src" / "bench" / "gpu_report_network.py"


def header(path):
    with open(path, "rb") as source:
        size = struct.unpack("<Q", source.read(8))[0]
        entries = json.loads(source.read(size))
    entries.pop("__metadata__", None)
    return {name: (tuple(e["shape"]), e["dtype"]) for name, e in entries.items()}


def main():
    if not G.SPEC.is_file():
        print(f"SKIP: {G.SPEC.relative_to(ROOT)} is missing (MLX-DLSS not unpacked)")
        return 77
    failures = 0
    for network, name in G.WEIGHT_FILES.items():
        made = G.shapes(network)
        path = ROOT / "work" / "mlxw" / name
        if not path.is_file():
            print(f"skip: {name} is not in work/mlxw, {network}'s {len(made)} tensors not compared")
            continue
        real = header(path)
        if made != real:
            failures += 1
            print(f"FAIL: {network}'s tensors differ from {name}: "
                  f"{sorted(set(real) ^ set(made))[:4]} "
                  f"{[k for k in real if k in made and real[k] != made[k]][:4]}")
        else:
            print(f"ok: {network}'s {len(made)} tensors are {name}'s, name, shape and dtype")
    weights = G.synthetic("A")
    halves = [k for k, (_, dtype) in G.shapes("A").items() if dtype == "F16"]
    not_e4m3 = [k for k in halves if xmxres.e4m3_bytes(weights[k]) is None]
    if not_e4m3:
        failures += 1
        print(f"FAIL: {len(not_e4m3)} of A's half tensors hold a value E4M3 cannot: {not_e4m3[:3]}")
    else:
        print(f"ok: every value of A's {len(halves)} half tensors is an E4M3 one")
    del weights

    run = subprocess.run([sys.executable, str(SCRIPT), "--networks", "B", "--fields", "640x360",
                          "--frames", "2", "--warm", "1", "--no-compare", "--no-profile"],
                         capture_output=True, text=True, timeout=600)
    if run.returncode == 2:
        print("SKIP: no device here runs the network:\n" + run.stdout.strip())
        return 77 if not failures else 1
    table = [line for line in run.stdout.splitlines() if line.strip().startswith("network B ")]
    if run.returncode != 0 or not table:
        failures += 1
        print(f"FAIL: the run exited {run.returncode}:\n{run.stdout}{run.stderr}")
    else:
        print("ok: " + table[0].strip())
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

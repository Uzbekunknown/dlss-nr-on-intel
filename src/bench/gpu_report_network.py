#!/usr/bin/env python3
"""NR's own network on this GPU without NVIDIA's DLL: the second half of the GPU report.

`nr_gpu_probe.exe` (`src/probe/coopmat_probe.c`) asks whether the matrix units compute NR's
shape, and then runs this, which runs the network itself. Networks B and A are built from their
tensors' shapes alone, A's from MLX-DLSS's `weight_spec.json` and B's from A's by each block's
role (`nr_topology.Topology.same_role`), with random weights, every half an E4M3 value as in
NVIDIA's large matrices. So the passes, their shapes, the kernels that take them and the forms
the weights are kept in are the ones a game's frame runs, and the time is the network's time.
What it draws from such weights is noise, and nothing looks at it. `--weights DIR` runs the
real logical weights instead, to check that on a machine that has them.

    python src/bench/gpu_report_network.py [--networks B,A] [--frames N] [--warm N]
                                           [--no-profile] [--no-compare] [--weights DIR]

The lines it prints are the ones `nr_gpu_probe.exe` copies into `nr-gpu-report.txt`. On Intel's
Windows driver the runtime keeps the weights as E4M3 bytes and by column where its start-up
checks pass; a second process with `XMX_E4M3_WEIGHTS=0` and `XMX_WEIGHTS_BY_COLUMN=0` times them
as 0.0.5 kept them, half and by row, so a report says what those forms do on its card. The
per-pass profile of network B at 1280x720 says where that frame's time goes, pass by pass: on
an A770 the network pays ~55 us a pass whatever the field (`notes/players-20261103.md`), and
whether that is the passes or the gaps between them is what such a profile tells.

Exit status: 0 measured; 2 no device here runs the network; 3 the shapes are missing (not a
release or a checkout with `work/mlx-dlss`).
"""
import argparse
import ctypes
import json
import os
import pathlib
import subprocess
import sys
import time

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
for _sub in ("src/bench", "src/ref", "src/gpu"):
    sys.path.insert(0, str(ROOT / _sub))
import nr_topology  # noqa: E402

SPEC = ROOT / "work" / "mlx-dlss" / "python" / "mlxdlss" / "weight_spec.json"
WEIGHT_FILES = {"A": "dlssnr-logical.safetensors", "B": "dlssnr-b-logical.safetensors"}
# A game's window and the network's field at render scale 0.4, (height, width), as the setup
# window's speed test runs them (`speed_test.py`); 640x360 is raised to the 320 floor.
FIELDS = (("640x360", (320, 320)), ("1280x720", (320, 512)), ("1920x1080", (448, 768)))
COMPARE = "1280x720"
# The half magnitudes the random weights take, 2**-7 to 2**-4: E4M3 values small enough that a
# frame of them stays finite through 71 blocks, which does not change what a pass costs.
LOW, HIGH = -7, -4


def shapes(network):
    """Each tensor of `network` as {name: (shape, dtype)}, from A's spec alone."""
    spec = json.loads(SPEC.read_text(encoding="utf-8"))["tensors"]
    if network == "A":
        return {name: (tuple(entry["shape"]), entry["dtype"]) for name, entry in spec.items()}
    source, target = nr_topology.PRESET_A, nr_topology.Topology(nr_topology.PRESETS[network])
    blocks = {}
    for name, entry in spec.items():
        head, _, rest = name.partition(".")
        if not (head.startswith("block") and head[5:].isdigit()):
            raise ValueError(f"{name}: a tensor of no block, which the spec did not have before")
        blocks.setdefault(int(head[5:]), []).append((rest, entry))
    return {f"block{index}.{rest}": (tuple(entry["shape"]), entry["dtype"])
            for index in range(target.block_count)
            for rest, entry in blocks[target.same_role(index, source)]}


def e4m3_table(low=LOW, high=HIGH):
    """Every E4M3 value of magnitude 2**low to 2**high, both signs, as float32."""
    codes = np.arange(256)
    exponent, mantissa = (codes >> 3) & 15, codes & 7
    value = np.where(exponent == 0, mantissa / 8.0 * 2.0 ** -6,
                     (1.0 + mantissa / 8.0) * 2.0 ** (exponent - 7.0))
    positive = value[(codes < 0x7F) & (value >= 2.0 ** low) & (value <= 2.0 ** high)]
    return np.concatenate([positive, -positive]).astype(np.float32)


def synthetic(network, seed=1112):
    """Random weights of `network`'s shapes, float32 as `nr_model.load_logical` gives them."""
    table = e4m3_table()
    rng = np.random.default_rng(seed)
    weights = {}
    for name, (shape, dtype) in shapes(network).items():
        if dtype == "F32":                       # attn_scale, a head's logit scale
            weights[name] = np.full(shape, 0.125, np.float32)
        else:
            weights[name] = table[rng.integers(0, table.size, size=shape, dtype=np.uint8)]
    return weights


def real(directory, network):
    import nr_model
    return nr_model.load_logical(pathlib.Path(directory) / WEIGHT_FILES[network])[0]


def features(height, width):
    return (np.random.default_rng(11).standard_normal((height, width, 16)) * 0.3).astype(np.float32)


def timed(frame, height, width, frames, warm, warm_seconds=1.0):
    """The network's own time a frame, in ms: the recorded graph replayed as the daemon runs
    it, and its time as the daemon reports it (`gpu a+B+c`, `ResidentFrame.split`), the
    median of `frames` back to back, as the setup window's speed test reads it. Before them
    `warm` frames and at least `warm_seconds`: the GPU's clock falls while the host makes a
    network's weights, and eight frames of 20 ms left it low enough to read A at twice its
    time on the Arc 140V."""
    x = features(height, width)
    started = time.perf_counter()
    done = 0
    while done < max(1, warm) or time.perf_counter() - started < warm_seconds:
        frame.run(x)                             # the first records the graph
        done += 1
    graph = []
    for _ in range(frames):
        frame.run(x)
        graph.append(frame.split[1])
    return float(np.median(graph)) * 1e3


def measure(runtime, weights, fields, frames, warm, profile_window=None):
    import nr_frame_resident as F
    import xmxres
    device = F.DeviceWeights(runtime, weights)
    times, profile = {}, None
    try:
        for window, (height, width) in fields:
            frame = F.ResidentFrame(runtime, device, height, width)
            try:
                times[window] = timed(frame, height, width, frames, warm)
            finally:
                frame.close()
            if window == profile_window:
                # timestamps go in as a graph is recorded, so the profile is a frame of its own
                xmxres.profile(True)
                frame = F.ResidentFrame(runtime, device, height, width)
                try:
                    x = features(height, width)
                    for _ in range(2):
                        frame.run(x, execution="single")
                    xmxres.profile_reset()
                    for _ in range(frames):
                        frame.run(x, execution="single")
                    profile = (xmxres.profile_totals(), frames)
                finally:
                    frame.close()
                    xmxres.profile(False)
    finally:
        device.close()
    return times, profile


def pass_rows(profile, top):
    import frame_profile
    totals, runs = profile
    rows = sorted(((ms / runs, n / runs, frame_profile.label(f, s))
                   for (f, s), (ms, n) in totals.items()), reverse=True)
    return rows[:top], sum(r[0] for r in rows), sum(r[1] for r in rows)


def runtime_or_exit():
    import xmxres
    try:
        runtime = xmxres.Runtime()
    except Exception as error:                   # no device with NR's matrix shape, a driver refusal
        print(f"  not measured: no GPU here runs NR's network ({error})")
        sys.exit(2)
    runtime.lib.xmx_device.restype = ctypes.c_char_p
    return runtime, runtime.lib.xmx_device().decode(errors="replace")


def child_compare(args):
    """The weights as 0.0.5 kept them: this process runs with both forms turned off."""
    runtime, _ = runtime_or_exit()
    out = {"e4m3": bool(runtime.e4m3_weights), "by_column": bool(runtime.weights_by_column)}
    for network in args.networks:
        weights = real(args.weights, network) if args.weights else synthetic(network)
        times, _ = measure(runtime, weights, [f for f in FIELDS if f[0] == COMPARE],
                           args.frames, args.warm)
        out[network] = times[COMPARE]
        del weights
    print("compare " + json.dumps(out), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--networks", default="B,A", help="comma-separated, in order (B,A)")
    parser.add_argument("--frames", type=int, default=20, help="timed frames a case")
    parser.add_argument("--warm", type=int, default=8, help="frames before the timed ones")
    parser.add_argument("--no-profile", action="store_true", help="leave out the per-pass profile")
    parser.add_argument("--no-compare", action="store_true",
                        help="leave out the second process, the weights as 0.0.5 kept them")
    parser.add_argument("--weights", type=pathlib.Path,
                        help="a folder with the real logical weights to run instead")
    parser.add_argument("--fields", default=",".join(w for w, _ in FIELDS),
                        help="which of the windows to time (640x360,1280x720,1920x1080)")
    parser.add_argument("--child-compare", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.networks = [n.strip().upper() for n in args.networks.split(",") if n.strip()]
    if any(n not in nr_topology.PRESETS for n in args.networks):
        parser.error("networks are A and B")
    fields = [f for f in FIELDS if f[0] in args.fields.split(",")]
    if not args.weights and not SPEC.is_file():
        print(f"  not measured: the network's shapes are missing ({SPEC.relative_to(ROOT)}); "
              "run the probe in the unpacked release folder")
        sys.exit(3)
    if args.child_compare:
        return child_compare(args)

    runtime, device_name = runtime_or_exit()
    source = "the real weights" if args.weights else "random weights of its shapes, no DLL needed"
    print(f"NR's network on this GPU, with {source}:", flush=True)
    print(f"  device: {device_name}")
    print(f"  weights kept as E4M3 bytes: {'yes' if runtime.e4m3_weights else 'no'}; "
          f"by column: {'yes' if runtime.weights_by_column else 'no'}", flush=True)
    results, profile = {}, None
    for network in args.networks:
        started = time.perf_counter()
        weights = real(args.weights, network) if args.weights else synthetic(network)
        made = time.perf_counter() - started
        want = COMPARE if network == "B" and not args.no_profile else None
        results[network], got = measure(runtime, weights, fields, args.frames, args.warm, want)
        profile = profile or got
        del weights
        print(f"  network {network}: weights made in {made:.1f} s, measured in "
              f"{time.perf_counter() - started:.1f} s", flush=True)
    print("  %-26s" % "ms a frame, render scale 0.4" + "".join("%11s" % w for w, _ in fields))
    for network, times in results.items():
        print("  %-26s" % f"network {network}" + "".join("%11.1f" % times[w] for w, _ in fields))
    if all(COMPARE in t for t in results.values()):
        rates = [f"{1000 / t[COMPARE]:.0f}" for t in results.values()]
        names = list(results)
        allows = f"{rates[0]} frames a second with {names[0]}" + "".join(
            f", and {r} with {n}" for r, n in zip(rates[1:], names[1:]))
        print(f"  At 1280x720 and render scale 0.4 the network alone allows at most {allows};\n"
              "  a game's own frame and NR's passes around the network come on top.")
    sys.stdout.flush()

    if not args.no_compare and any(COMPARE == w for w, _ in fields):
        env = dict(os.environ, XMX_E4M3_WEIGHTS="0", XMX_WEIGHTS_BY_COLUMN="0")
        command = [sys.executable, os.path.abspath(__file__), "--child-compare",
                   "--networks", ",".join(args.networks), "--frames", str(args.frames),
                   "--warm", str(args.warm)] + (["--weights", str(args.weights)] if args.weights else [])
        try:
            child = subprocess.run(command, env=env, capture_output=True, text=True, timeout=600)
            line = next((l for l in child.stdout.splitlines() if l.startswith("compare ")), None)
            if line is None:
                tail = (child.stdout + child.stderr).strip().splitlines()[-1:] or ["no output"]
                print(f"  with the weights as 0.0.5 kept them: not measured (exit {child.returncode}: "
                      f"{tail[0][:160]})")
            else:
                got = json.loads(line[len("compare "):])
                forms = "half, by row" if not (got["e4m3"] or got["by_column"]) else "as here"
                print("  with the weights as 0.0.5 kept them (%s): %s at 1280x720"
                      % (forms, ", ".join(f"{n} {got[n]:.1f} ms" for n in args.networks if n in got)))
        except subprocess.TimeoutExpired:
            print("  with the weights as 0.0.5 kept them: not measured (no answer in 600 s)")

    if profile:
        rows, device, passes = pass_rows(profile, 8)
        print(f"  where network B's 1280x720 frame goes on the GPU: {device:.1f} ms in "
              f"{passes:.0f} passes, {1000 * device / max(passes, 1):.1f} us a pass on average")
        for ms, n, name in rows:
            print("    %-34s %7.2f ms %5.1f %% %6.0f passes %7.1f us each"
                  % (name[:34], ms, 100 * ms / device, n, 1000 * ms / max(n, 1)))
    sys.stdout.flush()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:                   # one line in a public report, not a traceback
        print(f"  not measured: {type(error).__name__}: {error}", flush=True)
        sys.exit(1)

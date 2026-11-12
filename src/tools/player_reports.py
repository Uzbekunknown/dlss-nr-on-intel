#!/usr/bin/env python3
"""Players' reports as data: what each one says about NR's speed, and a fit across them.

    python3 src/tools/player_reports.py digest DLSS-NR-report.json [--issue N]
    python3 src/tools/player_reports.py probe nr-gpu-report.txt [--issue N]
    python3 src/tools/player_reports.py fit [notes/measurements/players.json]

`digest` reads the setup window's Save report and prints, as JSON, what
`notes/measurements/players.json` takes from it: the release, the GPU and its driver, the CPU and
memory, the game's API and NR's settings, the speed test's cases, and the daemon log's frames as
medians for each extent. A report also carries the player's folders, Windows user name, process
ids and pipe keys. A digest reads none of them, and the reports themselves are not committed.

`probe` reads `nr_gpu_probe.exe`'s report (`nr-gpu-report.txt`, or the copy an issue carries):
each device with matrix units, what it offers, and the rates it measured.

`fit` prints, for each speed test in the file, the network's time as a fixed part plus a part per
megapixel of network field. The fit covers the fields up to 0.45 MP, the sizes games run at, and
divides the fixed part by the network's passes. `knob_doc.py` renders
the same into `docs/SETTINGS.md`, and `notes/players-20261103.md` reads the results.
"""
import argparse
import json
import pathlib
import re
import statistics
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DATA = ROOT / "notes" / "measurements" / "players.json"

# The releases by the commit `release-metadata.json` names. 0.0.1 to 0.0.1.3 wrote no version.
RELEASES = {"7f9c364": "0.0.1", "f9ec5f1": "0.0.1.1", "2f90064": "0.0.1.2", "d89563f": "0.0.1.3",
            "81b0a59": "0.0.2", "86a5799": "0.0.3", "c0b7f9d": "0.0.3.1"}
# Device passes a frame, as test_resident.py counts them on Intel's driver (notes/phase72).
PASSES = {"A": 415, "B": 179}
# The fields a game at render scale 0.4 runs, up to 1920x1080's 768x448, and 832x512.
GAME_FIELDS_MP = 0.45

# The daemon's line for each frame (nr_daemon.process_connection, and print_gpu_frame on the
# GPU path, which also gives the passes around the network).
FRAME = re.compile(r"(\d+)x(\d+) \S+ in ([0-9.]+)s\b.*?gpu (\d+)\+(\d+)\+(\d+)ms\s+"
                   r"network (\d+)x(\d+) scale ([0-9.]+)(.*)$")
GPU_PASSES = re.compile(r"transport=(\S+) image prep=([0-9.]+)ms compose=([0-9.]+)ms")


def release_of(report):
    release = report.get("release") or {}
    commit = (release.get("source_commit") or "")[:7]
    return release.get("version") or RELEASES.get(commit) or commit or None


def gpu_of(report):
    """The GPU NR ran on and its driver: the speed test's device, or the daemon's, among the
    adapters Windows lists (which include virtual displays)."""
    name = (report.get("speed_test") or {}).get("device")
    ready = (report.get("status") or {}).get("model_ready") or ""
    if not name and " on " in ready:
        name = ready.split(" on ", 1)[1].strip()
    adapters = (report.get("system") or {}).get("gpu") or []
    for adapter in adapters:
        listed = adapter.get("Name") or ""
        if (listed == name) if name else ("Intel" in listed):
            return listed, adapter.get("DriverVersion")
    return name, None


def frames(log):
    """The log's frames, grouped by output, field, scale and transport, as medians."""
    groups = {}
    for line in (log or "").splitlines():
        found = FRAME.search(line)
        if not found:
            continue
        w, h, seconds, carried, ran, read, nw, nh, scale, rest = found.groups()
        passes = GPU_PASSES.search(rest)
        key = (int(w), int(h), int(nw), int(nh), float(scale), passes.group(1) if passes else "cpu")
        groups.setdefault(key, []).append(
            (1000 * float(seconds), int(carried), int(ran), int(read),
             float(passes.group(2)) if passes else None, float(passes.group(3)) if passes else None))
    out = []
    for (w, h, nw, nh, scale, transport), rows in groups.items():
        columns = list(zip(*rows))

        def median(i):
            values = [v for v in columns[i] if v is not None]
            return round(statistics.median(values), 2) if values else None
        out.append({"output": [w, h], "field": [nw, nh], "render_scale": scale,
                    "transport": transport, "frames": len(rows), "total_ms": median(0),
                    "upload_ms": median(1), "network_ms": median(2), "readback_ms": median(3),
                    "prep_ms": median(4), "compose_ms": median(5)})
    return out


def digest(report, issue=None):
    """What a Save report says about speed, and nothing that names the player."""
    system = report.get("system") or {}
    profile = report.get("profile") or {}
    test = report.get("speed_test") or {}
    gpu, driver = gpu_of(report)
    cpu = ((system.get("cpu") or {}).get("Name") or "").strip() or None
    memory = system.get("memory_bytes")
    game = (profile.get("game_exe") or "").replace("\\", "/").rsplit("/", 1)[-1] or None
    named = sorted({n.get("reported") for n in test.get("networks") or [] if n.get("reported")})
    cases = []
    for case in test.get("cases") or []:
        cases.append({"size": [case.get("width"), case.get("height")],
                      "render_scale": case.get("render_scale"), "field": case.get("network"),
                      "network_ms": case.get("network_ms"), "round_trip_ms": case.get("round_trip_ms"),
                      **({"network": case["preset"]} if case.get("preset") else {})})
    created = report.get("created_utc") or ""
    return {
        "issue": issue, "date": created[:10] or None, "release": release_of(report),
        "gpu": gpu, "driver": driver, "cpu": cpu,
        "memory_gib": round(memory / 2 ** 30, 1) if memory else None,
        "windows_build": (system.get("windows") or {}).get("BuildNumber"),
        "dll": (report.get("dll") or {}).get("version"),
        "api": profile.get("api"), "game_exe": game, "network": profile.get("preset"),
        "frames_on_gpu": profile.get("gpu_image_path"), "every": profile.get("nr_every"),
        "asynchronous": profile.get("nr_async"), "settings": report.get("settings"),
        "speed_test": {"date": (test.get("created_utc") or "")[:10] or None,
                       "network": test.get("preset"), "networks_named": named,
                       "blocks": test.get("blocks"), "memory": test.get("memory"),
                       "cases": cases} if cases else None,
        "frames": frames(report.get("daemon_log_tail")),
    }


PROBE_DEVICE = re.compile(r"^=== device \d+: (.+?) ===$")
PROBE_DRIVER = re.compile(r"^\s*driver\s*:\s*(.+?),\s*(\S+)\s*$")
PROBE_CONFIG = re.compile(r"^\s*\d+\s+8\s+(\d+)\s+(\d+)\s+(\w+)\s+(\w+)\s+(\w+)\s+(\w+)\s+\d+\s+subgroup")
PROBE_MATRIX = re.compile(r"^\s*matrix 8x(\d+)x16, subgroup\s+(\d+): (exact, (\d+) GFLOP/s|.+)$")
PROBE_VECTOR = re.compile(r"^\s*vector float32 FMA, subgroup\s+(\d+): (\d+) GFLOP/s")
PROBE_VERDICT = re.compile(r"^\s*NR's kernels \(float16 8x16x16 at subgroup 32\): (.+)$")
# The network half since 0.0.6 (src/bench/gpu_report_network.py), on the device libxmx picks.
NET_START = re.compile(r"^NR's network on this GPU, with (.+):$")
NET_DEVICE = re.compile(r"^\s*device: (.+)$")
NET_FORMS = re.compile(r"^\s*weights kept as E4M3 bytes: (yes|no); by column: (yes|no)")
NET_WINDOWS = re.compile(r"^\s*ms a frame, render scale 0\.4\s+(.+)$")
NET_ROW = re.compile(r"^\s*network ([AB])\s+([0-9.\s]+)$")
NET_COMPARE = re.compile(r"^\s*with the weights as 0\.0\.5 kept them \(([^)]*)\): (.+) at 1280x720$")
NET_PROFILE = re.compile(r"^\s*where network B's 1280x720 frame goes on the GPU: ([0-9.]+) ms in "
                         r"(\d+) passes")
NET_PASS = re.compile(r"^\s{4}(\S.*?)\s+([0-9.]+) ms\s+[0-9.]+ %\s+(\d+) passes\s+([0-9.]+) us each$")


def probe_network(text):
    """The network half of a probe report, or None where the report has none."""
    network, windows = None, []
    for line in text.splitlines():
        line = line.rstrip()
        if found := NET_START.match(line):
            network = {"weights": found.group(1), "device": None, "e4m3": None, "by_column": None,
                       "ms": {}, "as_0_0_5": {}, "profile": None}
        elif network is None:
            continue
        elif found := NET_DEVICE.match(line):
            network["device"] = found.group(1)
        elif found := NET_FORMS.match(line):
            network["e4m3"], network["by_column"] = (v == "yes" for v in found.groups())
        elif found := NET_WINDOWS.match(line):
            windows = found.group(1).split()
        elif (found := NET_ROW.match(line)) and windows:
            network["ms"][found.group(1)] = dict(zip(windows, map(float, found.group(2).split())))
        elif found := NET_COMPARE.match(line):
            for name, ms in re.findall(r"\b([AB]) ([0-9.]+) ms", found.group(2)):
                network["as_0_0_5"][name] = float(ms)
        elif found := NET_PROFILE.match(line):
            network["profile"] = {"ms": float(found.group(1)), "passes": int(found.group(2)),
                                  "rows": []}
        elif (found := NET_PASS.match(line)) and network["profile"] is not None:
            name, ms, passes, each = found.groups()
            network["profile"]["rows"].append({"pass": name, "ms": float(ms), "passes": int(passes),
                                               "us_each": float(each)})
    return network


def probe(text, issue=None):
    """Each device with matrix units in a probe report (coopmat_probe.c's `say` lines)."""
    devices, device = [], None
    for line in text.splitlines():
        line = line.rstrip()
        if found := PROBE_DEVICE.match(line.strip()):
            device = {"issue": issue, "gpu": found.group(1), "driver": None, "offered": [],
                      "matrix_gflops": {}, "fma32_gflops": {}, "nr_kernels": None}
            devices.append(device)
        elif device is None:
            continue
        elif found := PROBE_DRIVER.match(line):
            device["driver"] = found.group(2)
        elif found := PROBE_CONFIG.match(line):
            n, k, a, _, c, _ = found.groups()
            device["offered"].append(f"8x{n}x{k} {a} to {c}")
        elif found := PROBE_MATRIX.match(line):
            n, subgroup, verdict, rate = found.groups()
            device["matrix_gflops"].setdefault(f"8x{n}x16", {})[subgroup] = (
                int(rate) if rate else verdict)
        elif found := PROBE_VECTOR.match(line):
            device["fma32_gflops"][found.group(1)] = int(found.group(2))
        elif found := PROBE_VERDICT.match(line):
            device["nr_kernels"] = found.group(1)
    network = probe_network(text)
    for device in devices:
        if network and network["device"] == device["gpu"]:
            device["network"] = network
    return [d for d in devices if d["offered"]]


def middle(value):
    """A number, or the middle of a [low, high] range."""
    return sum(value) / 2 if isinstance(value, list) else value


def fit_network(cases):
    """Least squares of the network's ms on its field's megapixels, over the fields games run:
    (fixed ms, ms per megapixel, worst residual)."""
    points = [(case["field"][0] * case["field"][1] / 1e6, middle(case["network_ms"]))
              for case in cases if case["field"][0] * case["field"][1] / 1e6 <= GAME_FIELDS_MP]
    n = len(points)
    sx = sum(x for x, _ in points)
    sy = sum(y for _, y in points)
    sxx = sum(x * x for x, _ in points)
    sxy = sum(x * y for x, y in points)
    slope = (n * sxy - sx * sy) / (n * sxx - sx * sx)
    fixed = (sy - slope * sx) / n
    worst = max(abs(fixed + slope * x - y) for x, y in points)
    return fixed, slope, worst


def load(path=DATA):
    return json.loads(pathlib.Path(path).read_text(encoding="utf-8"))


def fits(data):
    """One row per speed test: who, network, the fit, and the fixed part per pass."""
    rows = []
    for test in data["speed_tests"]:
        fixed, slope, worst = fit_network(test["cases"])
        rows.append({"test": test, "fixed": fixed, "slope": slope, "worst": worst,
                     "per_pass_us": 1000 * fixed / PASSES[test["network"]]})
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="what", required=True)
    for name in ("digest", "probe"):
        one = sub.add_parser(name)
        one.add_argument("file")
        one.add_argument("--issue", type=int)
    sub.add_parser("fit").add_argument("file", nargs="?", default=str(DATA))
    args = parser.parse_args(argv)
    if args.what == "digest":
        report = json.loads(pathlib.Path(args.file).read_text(encoding="utf-8-sig"))
        print(json.dumps(digest(report, args.issue), indent=2, ensure_ascii=False))
    elif args.what == "probe":
        text = pathlib.Path(args.file).read_text(encoding="utf-8-sig", errors="replace")
        print(json.dumps(probe(text, args.issue), indent=2, ensure_ascii=False))
    else:
        for row in fits(load(args.file)):
            test = row["test"]
            print(f"{test['gpu']:10} {test['network']} {test['source']:34} "
                  f"{row['fixed']:5.1f} ms + {row['slope']:5.1f} ms/MP (worst {row['worst']:.1f}),"
                  f" {row['per_pass_us']:3.0f} us a pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())

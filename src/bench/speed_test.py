#!/usr/bin/env python3
"""NR's own speed on this PC: the setup window's speed test, and what Save report carries of it.

The daemon is started as the layer starts it and handed synthetic frames over its own transport,
one connection a frame as nr_layer.c makes them, at four window sizes, at the render scale a
release starts at (0.4) and at full scale (1.0). Two times for each:
- the network's, as the daemon reports it (the middle of its `gpu in+run+out` split): what the
  network itself takes for a frame;
- the round trip a game waits for: the frame there and back, the host passes around the network,
  and the network.
No game runs, so these are the best NR does on this PC. In a game the layer also captures each
frame and writes the answer back, and the game draws on the same GPU, so its frame rate is lower.

    python src/bench/speed_test.py [--root RELEASE] [--json FILE] [--preset A|B]

Writes JSON: the GPU the daemon took, how long it took to load and the network preset it runs,
and for each case the network's field and both times. A named pipe on Windows, a Unix socket
elsewhere. live_rates.py measures docs/SETTINGS.md's table, with its own plan; this one is short
enough for a first install.
"""
import argparse
import json
import os
import pathlib
import re
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import uuid

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
MAGIC = 0x304E524E
FORMAT_B8G8R8A8 = 44
# Where a release starts (windows_wizard_core.install), then full scale, for comparison: the
# numbers are comparable from one PC to the next only if every PC measures them alike.
MIN_EXTENT = 320
SCALES = (0.4, 1.0)
SIZES = ((640, 360), (800, 450), (1280, 720), (1920, 1080))
# More than live_rates.py's nine and three: through the setup window, with the window itself on
# screen, nine put 1280x720 at 38 ms in one run and 50 in the next.
FRAMES, WARMUP = 15, 5


def network_of(log):
    """The preset and the block count the daemon's start-up line names, or None: `network preset B
    (dlssnr-b-logical.safetensors), 31 blocks, ...` since 346d300, `network preset A, 71 blocks`
    before it. A reading of the old form alone missed the new one, and every report from 0.0.2 to
    0.0.3 named no network."""
    found = re.search(r"network preset (\w+)\b[^\n]*?(\d+) blocks", log)
    return (found.group(1), int(found.group(2))) if found else None
# The daemon's line for each frame (nr_daemon.process_connection).
LINE = re.compile(r"^(\d+)x(\d+) \S+ in [0-9.]+s .*?gpu (\d+)\+(\d+)\+(\d+)ms.*?"
                  r"network (\d+)x(\d+) scale ([0-9.]+)", re.M)


def frames(width, height, count=3):
    """A few different frames of noise with structure at several scales (live_rates.frame),
    so each is a fresh frame, as in a game that moves, and no bars are found."""
    made = []
    for seed in range(count):
        rng = np.random.default_rng(seed)
        small = rng.random((max(2, -(-height // 16)), max(2, -(-width // 16)), 3))
        coarse = np.repeat(np.repeat(small, 16, axis=0), 16, axis=1)[:height, :width]
        image = np.clip(coarse * 0.7 + rng.random((height, width, 3)) * 0.3, 0, 1)
        out = np.empty((height, width, 4), np.uint8)
        out[..., :3] = (image * 255).astype(np.uint8)
        out[..., 3] = 255
        made.append(out.tobytes())
    return made


def connect(endpoint, deadline):
    """One connection to the daemon, as the layer opens one for each frame."""
    while True:
        try:
            if os.name == "nt":
                return open(endpoint, "r+b", buffering=0)
            import socket
            client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            client.settimeout(600)
            client.connect(endpoint)
            return client
        except OSError as error:
            # All of the pipe's instances busy, or not made yet: the layer waits too.
            if time.monotonic() > deadline:
                raise RuntimeError(f"no answer on {endpoint}: {error}") from error
            time.sleep(0.05)


def round_trip(endpoint, header, payload, want):
    started = time.perf_counter()
    link = connect(endpoint, time.monotonic() + 60)
    try:
        data = memoryview(header + payload)
        if os.name == "nt":
            while data:
                data = data[link.write(data):]
            got = 0
            while got < want:
                piece = link.read(want - got)
                if not piece:
                    break
                got += len(piece)
        else:
            link.sendall(data)
            got = 0
            while got < want:
                piece = link.recv(want - got)
                if not piece:
                    break
                got += len(piece)
    finally:
        link.close()
    if got != want:
        raise RuntimeError(f"the daemon answered {got} of {want} bytes: it refused the frame, "
                           "and its log says why")
    return time.perf_counter() - started


def tail(path, size=4000):
    try:
        return path.read_bytes()[-size:].decode("utf-8", errors="replace")
    except OSError:
        return ""


def measure(endpoint, log, width, height, scale, count, warmup):
    """One case: the round trips, and the daemon's own lines for the same frames."""
    header = struct.pack("<4I", MAGIC, width, height, FORMAT_B8G8R8A8)
    made, want = frames(width, height), 4 * width * height
    start = log.stat().st_size
    times = []
    for i in range(warmup + count):
        took = round_trip(endpoint, header, made[i % len(made)], want)
        if i >= warmup:
            times.append(took)
    with log.open("rb") as handle:
        handle.seek(start)
        text = handle.read().decode("utf-8", errors="replace")
    lines = [found for found in LINE.findall(text)
             if (int(found[0]), int(found[1])) == (width, height) and float(found[7]) == scale][-count:]
    network_ms = statistics.median(int(found[3]) for found in lines) if lines else None
    times.sort()
    middle = times[len(times) // 2]
    return {"width": width, "height": height, "render_scale": scale,
            "network": [int(lines[-1][5]), int(lines[-1][6])] if lines else None,
            "network_ms": network_ms, "network_fps": round(1000 / network_ms, 1) if network_ms else None,
            "round_trip_ms": round(1000 * middle, 1), "round_trip_min_ms": round(1000 * times[0], 1),
            "round_trip_max_ms": round(1000 * times[-1], 1), "round_trip_fps": round(1 / middle, 1)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT, help="the release or checkout")
    parser.add_argument("--json", type=pathlib.Path, help="write the result here as well as to stdout")
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("--warmup", type=int, default=WARMUP)
    # The test takes no NR_ setting from its own environment (below), so the preset is asked for.
    parser.add_argument("--preset", type=str.upper, choices=("A", "B"),
                        help="the network the daemon runs (NR_PRESET); A if left out")
    args = parser.parse_args()
    root = args.root.resolve()
    result = {"ok": False, "error": None, "min_extent": MIN_EXTENT, "scales": list(SCALES),
              "frames": args.frames, "warmup": args.warmup, "python": sys.version.split()[0], "cases": []}
    # The daemon's log and settings stay in the release, overwritten by the next test. On
    # Windows a virtual environment's python.exe starts the interpreter as a child, which ends
    # with this test (NR_LAYER_SPAWNED below) and holds its log open until then.
    room = root / "work" / "speed-test"
    room.mkdir(parents=True, exist_ok=True)
    log = room / "daemon.log"
    settings = room / "settings.json"
    settings.write_text(json.dumps({"render_scale": SCALES[0], "min_extent": MIN_EXTENT}), encoding="utf-8")
    if os.name == "nt":
        endpoint = rf"\\.\pipe\nr_speed_test_{uuid.uuid4().hex[:12]}"
    else:
        endpoint = str(pathlib.Path(tempfile.gettempdir()) / f"nr-speed-test-{uuid.uuid4().hex[:12]}.sock")
    # The daemon as the layer starts it: its keeping of NumPy's blocks, without the layer named
    # in its own Vulkan instance, and marked with this process, so it ends with this test.
    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith("NR_") and key.upper() != "VK_INSTANCE_LAYERS"}
    environment.update(NR_KEEP_BLOCKS="1", PYTHONUTF8="1", PYTHONDONTWRITEBYTECODE="1",
                       NR_LAYER_SPAWNED=str(os.getpid()))
    if args.preset:
        environment["NR_PRESET"] = args.preset
    hidden = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    started = time.perf_counter()
    with log.open("wb") as output:
        daemon = subprocess.Popen(
            [sys.executable, str(root / "src/layer/nr_daemon.py"), "--socket", endpoint,
             "--settings", str(settings), "--root", str(root)],
            stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT, env=environment,
            cwd=root, **hidden)
    try:
        # Ready once it listens: the model loads first, and the first start on a PC also builds
        # the driver's shader cache.
        while "listening on" not in tail(log, 1 << 16):
            if daemon.poll() is not None:
                raise RuntimeError(f"the daemon ended (code {daemon.returncode}) before it listened")
            if time.perf_counter() - started > 600:
                raise RuntimeError("the daemon did not listen within 600 s")
            time.sleep(0.2)
        result["daemon_ready_s"] = round(time.perf_counter() - started, 1)
        text = tail(log, 1 << 16)
        ready = re.search(r"model ready in ([0-9.]+)s on (.+)", text)
        if ready:
            result["model_ready_s"], result["device"] = float(ready.group(1)), ready.group(2).strip()
        network = network_of(text)
        if network:
            result["preset"], result["blocks"] = network
        memory = re.search(r"buffers in (.+)", text)
        if memory:
            result["memory"] = memory.group(1).strip()
        for scale in SCALES:
            # The daemon reads its settings between frames; the warm-up frames take the change.
            settings.write_text(json.dumps({"render_scale": scale, "min_extent": MIN_EXTENT}), encoding="utf-8")
            for width, height in SIZES:
                case = measure(endpoint, log, width, height, scale, args.frames, args.warmup)
                result["cases"].append(case)
                print(f"{width}x{height} at {scale:g}: network {case['network_ms']} ms, round trip "
                      f"{case['round_trip_ms']} ms, field {case['network']}", file=sys.stderr, flush=True)
        result["ok"] = True
    except (OSError, RuntimeError) as error:
        result["error"] = str(error)
        result["daemon_log_tail"] = tail(log)
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=30)
        except subprocess.TimeoutExpired:
            daemon.kill()
        result["seconds"] = round(time.perf_counter() - started, 1)
        if os.name != "nt":
            try:
                os.unlink(endpoint)
            except OSError:
                pass
    text = json.dumps(result, indent=2)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(text, encoding="utf-8")
    print(text)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())

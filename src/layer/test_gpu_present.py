"""Run the owned Win32 Vulkan fixture with a private daemon and test settings.

The default prints the proposed command and launches nothing. --run is an
explicit GPU test: coordinate it with other GPU work. This script never builds
or replaces runtime DLLs, installs a game, changes the registry, or stops an
existing process. Every child and settings/trigger file belongs to this run.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import statistics
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
MAX_LOG = 1024 * 1024
PRESENT_PATTERN = (r"^present phase=(\d+) frame=(\d+) extent=(\d+)x(\d+) "
                   r"image=\d+ result=-?\d+ present_ms=([0-9]+(?:\.[0-9]+)?)\r?$")


def parse_extent(value):
    match = re.fullmatch(r"(\d+)x(\d+)", value)
    if not match:
        raise argparse.ArgumentTypeError("extent must be WIDTHxHEIGHT")
    width, height = map(int, match.groups())
    if not 128 <= width <= 4096 or not 64 <= height <= 4096:
        raise argparse.ArgumentTypeError("extent requires width 128..4096 and height 64..4096")
    return width, height


def present_timings(output, warmup):
    rows = re.findall(PRESENT_PATTERN, output, re.MULTILINE)
    result = []
    for phase in (1, 2):
        samples = [(int(w), int(h), float(ms)) for p, f, w, h, ms in rows
                   if int(p) == phase and int(f) > warmup]
        if not samples:
            continue
        values = sorted(row[2] for row in samples)
        result.append({"phase": phase, "width": samples[0][0], "height": samples[0][1],
                       "samples": len(values), "median_ms": statistics.median(values),
                       "p95_ms": values[max(0, (95 * len(values) + 99) // 100 - 1)],
                       "scope": "vkQueuePresentKHR including layer capture/network/return and downstream WSI"})
    return result


def validate_present_output(output, frames, extents):
    timed = re.findall(PRESENT_PATTERN, output, re.MULTILINE)
    rows = [row[:4] for row in timed]
    if len(rows) != 2 * frames or len(re.findall(r"^present phase=", output, re.MULTILINE)) != len(rows):
        raise RuntimeError("missing/partial native present or timing evidence")
    for phase, size in enumerate(extents, 1):
        actual = [(int(f), int(w), int(h)) for p, f, w, h in rows if int(p) == phase]
        if sorted(actual) != [(frame, *size) for frame in range(1, frames + 1)]:
            raise RuntimeError(f"phase {phase}: frame IDs or actual swapchain extents differ from the request")
    if any(not 0 <= float(ms) < float("inf") for _, _, _, _, ms in timed):
        raise RuntimeError("invalid native present duration")


def atomic_json(path, value):
    temporary = path.with_name(path.name + ".new")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def tail(path, count=MAX_LOG):
    if not path.is_file():
        return ""
    with path.open("rb") as handle:
        handle.seek(max(0, path.stat().st_size - count))
        return handle.read(count).decode("utf-8", errors="replace")


def wait_for(path, pattern, seconds=10.0):
    """True once `pattern` appears in the log, waiting up to `seconds` for it.

    A settings change is written between presents and taken up when the daemon next reads the file,
    which is a frame or two later - at 128x64 that is 40 ms, and at a larger extent it is seconds.
    Waiting for the effect is what makes the check mean "the daemon used the new value" instead of
    "the timer happened to be long enough".
    """
    deadline = time.monotonic() + seconds
    while True:
        if re.search(pattern, tail(path), re.MULTILINE):
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.05)


def stop_owned(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def wait_ready(process, log, endpoint, timeout, *, offset=0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"owned daemon exited before listening ({process.returncode})")
        with log.open("rb") as handle:
            handle.seek(max(offset, max(0, log.stat().st_size - 65536)))
            latest = handle.read(65536).decode("utf-8", errors="replace")
        if "listening on " + endpoint in latest:
            return
        time.sleep(0.05)
    raise TimeoutError("owned daemon did not become ready within the startup deadline")


def monitor_fixture(process, log, trigger, settings, controls, timeout, *, stepped=False, callback=None):
    lines = queue.Queue()

    def reader():
        try:
            for line in process.stdout:
                log.write(line)
                log.flush()
                lines.put(line)
        except (OSError, ValueError):
            # The owned process may be terminated at the test deadline.
            pass
        finally:
            lines.put(None)

    worker = threading.Thread(target=reader, daemon=True)
    worker.start()
    events = []
    deadline = time.monotonic() + timeout
    try:
        while True:
            if time.monotonic() >= deadline:
                raise TimeoutError("owned Vulkan fixture exceeded its run deadline")
            try:
                line = lines.get(timeout=0.1)
            except queue.Empty:
                continue
            if line is None:
                break
            match = re.search(r"present phase=(\d+) frame=(\d+) ", line)
            if not match:
                continue
            phase, frame = map(int, match.groups())
            if controls and (phase, frame) == (1, 4):
                trigger.unlink(missing_ok=True)
                events.append({"after_phase": phase, "after_frame": frame, "effect": False})
            elif controls and (phase, frame) == (1, 6):
                trigger.touch()
                events.append({"after_phase": phase, "after_frame": frame, "effect": True})
            elif controls and (phase, frame) in ((2, 4), (2, 8)):
                values = json.loads(settings.read_text(encoding="utf-8"))
                values["render_scale"] = 1.0 if frame == 4 else 0.4
                atomic_json(settings, values)
                events.append({"after_phase": phase, "after_frame": frame,
                               "render_scale": values["render_scale"]})
            if callback is not None:
                event = callback(phase, frame, max(0.1, deadline - time.monotonic()))
                if event is not None:
                    events.append(event)
            if stepped:
                process.stdin.write("go\n")
                process.stdin.flush()
        process.wait(timeout=max(1, deadline - time.monotonic()))
    except BaseException:
        stop_owned(process)
        raise
    finally:
        worker.join(timeout=5)
        process.stdout.close()
        if stepped and process.stdin:
            process.stdin.close()
    return events


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="launch this GPU test after obtaining the shared test slot")
    parser.add_argument("--fixture", type=Path, default=ROOT / "work/test_gpu_present.exe")
    parser.add_argument("--layer", type=Path, default=ROOT / "work/libnr_layer.dll")
    parser.add_argument("--output-dir", type=Path, help="new/empty result folder under this checkout's work directory")
    parser.add_argument("--frames", type=int, default=12, help="1..120 frames per swapchain")
    parser.add_argument("--extent", type=parse_extent, default=(128, 64), help="initial swapchain WIDTHxHEIGHT")
    parser.add_argument("--resize-extent", type=parse_extent, default=(160, 96), help="second swapchain WIDTHxHEIGHT")
    parser.add_argument("--render-scale", type=float, default=0.4)
    parser.add_argument("--warmup", type=int, default=0, help="exclude the first N presents per phase from timing")
    parser.add_argument("--gpu-only", action="store_true", help="forbid CPU pixel fallback; failures retain the original")
    parser.add_argument("--unsupported", choices=("async", "settings"),
                        help="GPU-only diagnostic: unsupported mode must retain the original without CPU pixels")
    parser.add_argument("--pause-ms", type=int, default=None, help="0..1000 ms per frame; controls default to 100")
    parser.add_argument("--exercise-controls", action="store_true", help="test owned trigger off/on and render-scale changes")
    parser.add_argument("--restart-daemon", action="store_true", help="step the fixture through owned daemon loss/restart and safe fallback")
    parser.add_argument("--no-validation", action="store_true", help="disable the default Khronos validation request")
    parser.add_argument("--sync-validation", action="store_true", help="require synchronization validation support")
    parser.add_argument("--validation-ignore", action="append", default=None, metavar="REGEX",
                        help="an EXTRA validation ERROR pattern not to count; the loader failing to open "
                             "another module's library is always ignored, and this is for anything else")
    parser.add_argument("--validation-manifest", type=Path)
    parser.add_argument("--api11", action="store_true")
    parser.add_argument("--trace", action="store_true", help="print native Vulkan call boundaries for diagnosis")
    parser.add_argument("--baseline", action="store_true", help="diagnostic Vulkan presents without NR or a daemon")
    parser.add_argument("--fenced", action="store_true", help="force the fixture's memory-only fenced protocol v2")
    parser.add_argument("--loader-debug", choices=("error,warn", "all"), default="error,warn")
    parser.add_argument("--disable-steam-overlay", action="store_true", help="disable the implicit overlay only for this owned fixture")
    parser.add_argument("--startup-timeout", type=float, default=90)
    parser.add_argument("--run-timeout", type=float, default=60)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("the native Win32 present smoke requires Windows")
    if not 1 <= args.frames <= 120 or (args.exercise_controls and args.frames < 10):
        parser.error("frames must be 1..120, and at least 10 when exercising controls")
    if args.no_validation and args.sync_validation:
        parser.error("synchronization validation cannot be required with validation disabled")
    if args.baseline and args.fenced:
        parser.error("fenced transport requires NR; it cannot be forced for a no-NR baseline")
    if args.baseline and args.gpu_only:
        parser.error("GPU-only policy requires NR")
    if args.unsupported and (not args.gpu_only or args.baseline or args.restart_daemon or args.exercise_controls or args.frames < 8):
        parser.error("unsupported-mode test requires GPU-only, at least 8 frames, and no other controls")
    if not 0 <= args.warmup < args.frames or not 0.05 <= args.render_scale <= 1.0:
        parser.error("warmup must be below frames and render scale must be 0.05..1.0")
    if args.exercise_controls and args.render_scale != 0.4:
        parser.error("controlled scale changes start at render scale 0.4")
    if args.restart_daemon and (args.baseline or args.exercise_controls or args.frames < 8):
        parser.error("daemon restart requires NR, at least 8 frames, and separate trigger/scale controls")
    pause_ms = args.pause_ms if args.pause_ms is not None else (100 if args.exercise_controls else 0)
    if not 0 <= pause_ms <= 1000 or not 0 < args.startup_timeout <= 300 or not 0 < args.run_timeout <= 300:
        parser.error("invalid pause/deadline bounds")
    fixture, layer = args.fixture.resolve(), args.layer.resolve()
    if not fixture.is_file() or not layer.is_file():
        parser.error("compile the fixture/runtime first; the harness never builds or overwrites DLLs")
    work = (ROOT / "work").resolve()
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    folder = (args.output_dir or work / ("gpu-present-smoke-" + run_id)).resolve()
    if folder == work or not folder.is_relative_to(work):
        parser.error("result folder must be inside this checkout's work directory")
    if folder.exists() and (not folder.is_dir() or any(folder.iterdir())):
        parser.error("result folder must be new or empty")
    endpoint = "\\\\.\\pipe\\nr-present-smoke-" + uuid.uuid4().hex
    settings, trigger = folder / "settings.json", folder / "trigger"
    manifest, daemon_log = folder / "VkLayer_dlss_nr.json", folder / "daemon.log"
    native_python = Path(getattr(sys, "_base_executable", sys.executable)).resolve()
    spec = importlib.util.find_spec("numpy")
    if spec is None or not spec.submodule_search_locations:
        parser.error("selected Python environment lacks NumPy")
    daemon_command = [str(native_python), "-u", str(ROOT / "src/layer/nr_daemon.py"),
                      "--socket", endpoint, "--root", str(ROOT), "--settings", str(settings),
                      "--render-scale", str(args.render_scale), "--min-extent", "128"]
    fixture_command = [str(fixture), "--frames", str(args.frames), "--pause-ms", str(pause_ms),
                       "--extent", "%dx%d" % args.extent, "--resize-extent", "%dx%d" % args.resize_extent]
    if not args.no_validation:
        fixture_command.append("--sync-validation" if args.sync_validation else "--validation")
    if args.api11:
        fixture_command.append("--api11")
    if args.trace:
        fixture_command.append("--trace")
    if args.baseline:
        fixture_command.append("--no-layer")
    if args.restart_daemon:
        fixture_command += ["--step", "--check-original-frame", "3"]
        if args.gpu_only:
            fixture_command += ["--check-second-original-frame", "5"]
    elif args.unsupported:
        fixture_command += ["--check-original-frame", "3"]
    plan = {"root": str(ROOT), "output_dir": str(folder), "endpoint": endpoint,
            "layer": str(layer), "daemon_command": daemon_command, "fixture_command": fixture_command,
            "exercise_controls": args.exercise_controls, "gpu_test": bool(args.run), "baseline": args.baseline,
            "force_fence": args.fenced, "restart_daemon": args.restart_daemon,
            "gpu_only": args.gpu_only, "extents": [args.extent, args.resize_extent],
            "unsupported": args.unsupported,
            "render_scale": args.render_scale, "timing_warmup": args.warmup,
            "fixture_extra_environment": {"NR_GPU_TRANSPORT_FENCE": "1"} if args.fenced else {}}
    plan["artifact_sha256"] = {str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path):
                               hashlib.sha256(path.read_bytes()).hexdigest()
                               for path in (layer, fixture, ROOT / "work/libxmx.dll", ROOT / "work/image_stats.spv")
                               if path.is_file()}
    print(json.dumps(plan, indent=2), flush=True)
    if not args.run:
        return 0

    folder.mkdir(parents=True, exist_ok=True)
    atomic_json(manifest, {"file_format_version": "1.0.0", "layer": {
        "name": "VK_LAYER_dlssnr_intel", "type": "GLOBAL", "library_path": str(layer),
        "api_version": "1.3.0", "implementation_version": "1", "description": "Owned GPU present smoke"}})
    atomic_json(settings, {"render_scale": args.render_scale, "min_extent": 128, "profile": "standard",
                           "detail_strength": 0.5 if args.unsupported == "settings" else 1, "colour_strength": 1})
    trigger.touch()
    environment = {name: value for name, value in os.environ.items()
                   if not name.upper().startswith(("NR_", "XMX_", "VK_"))
                   and name.upper() not in ("ENABLE_NR_LAYER", "DISABLE_NR_LAYER")}
    # The base interpreter avoids the Windows venv launcher subprocess: the
    # Popen PID is precisely the owned GPU daemon stopped below. CPython's own
    # venv-launcher hint preserves site-packages even when the daemon replaces
    # PYTHONPATH for its independent half-rounding probe.
    if sys.prefix != sys.base_prefix:
        environment["__PYVENV_LAUNCHER__"] = sys.executable
    environment["PYTHONPATH"] = str(Path(next(iter(spec.submodule_search_locations))).parent)
    environment.update(NR_ROOT=str(ROOT), NR_SETTINGS=str(settings), NR_LAYER_SOCKET=endpoint,
        NR_LAYER_TRIGGER=str(trigger), NR_LAYER_LOG=str(daemon_log), NR_LAYER_LIVE="1", NR_LAYER_ASYNC="0",
        NR_LAYER_UI_MASK="0", NR_LAYER_SPAWN="0", NR_GPU_IMAGE="1", NR_GPU_TRANSPORT="1",
        NR_INPUT_FP16="1", NR_DLL_PATH=r"C:\msys64\ucrt64\bin", PYTHONUTF8="1", PYTHONDONTWRITEBYTECODE="1")
    if args.gpu_only:
        environment["NR_GPU_ONLY"] = "1"
    if args.unsupported == "async":
        environment["NR_LAYER_ASYNC"] = "1"
    # Startup watcher concerns only this harness, so a crashed harness cannot
    # leave its GPU daemon behind. No external process is stopped or observed.
    daemon_environment = dict(environment, NR_LAYER_SPAWNED=str(os.getpid()))
    layer_paths = [str(manifest)]
    sdk = os.environ.get("VULKAN_SDK", "")
    validation_manifest = args.validation_manifest or (Path(sdk) / "Bin/VkLayer_khronos_validation.json" if sdk else None)
    if not args.no_validation and validation_manifest and validation_manifest.is_file():
        layer_paths.append(str(validation_manifest.resolve()))
    environment.update(VK_LAYER_PATH=os.pathsep.join(layer_paths), VK_INSTANCE_LAYERS="VK_LAYER_dlssnr_intel",
        ENABLE_NR_LAYER="1", DISABLE_VK_LAYER_VALVE_steam_fossilize_1="1", VK_LOADER_DEBUG=args.loader_debug,
        NR_LAYER_TIMING="1", NR_LAYER_TIMING_FILE=str(folder / "layer-timing.jsonl"))
    environment["NR_LAYER_TIMING_EVERY"] = "1"
    # Applied after copying daemon_environment: only the exporting fixture
    # chooses v2. The daemon verifies the actual packet's version and handles.
    environment.update(plan["fixture_extra_environment"])
    if args.disable_steam_overlay:
        environment["DISABLE_VK_LAYER_VALVE_steam_overlay_1"] = "1"
    daemon = present = None
    owned_daemons = []
    summary = dict(plan, ok=False)
    try:
        with daemon_log.open("w", encoding="utf-8") as log:
            if not args.baseline:
                daemon = subprocess.Popen(daemon_command, cwd=ROOT, env=daemon_environment,
                    stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                owned_daemons.append(daemon)
                wait_ready(daemon, daemon_log, endpoint, args.startup_timeout)

            def restart_after_frame(phase, frame, remaining):
                nonlocal daemon
                if (phase, frame) == (1, 2):
                    # The frame ACK already means GPU work completed. Wait for
                    # its flushed metric too so the stop cannot erase evidence.
                    deadline = time.monotonic() + min(5, remaining)
                    while len(re.findall(rf"^{args.extent[0]}x{args.extent[1]}.*\btransport=gpu-shared(?:-fence)?\b",
                                         tail(daemon_log), re.MULTILINE)) < 2:
                        if time.monotonic() >= deadline or daemon.poll() is not None:
                            raise RuntimeError("initial daemon did not confirm the first two shared frames")
                        time.sleep(0.01)
                    old_pid = daemon.pid
                    stop_owned(daemon)
                    return {"after_phase": phase, "after_frame": frame,
                            "daemon_stopped": old_pid, "returncode": daemon.returncode}
                if (phase, frame) == (1, 3):
                    offset = daemon_log.stat().st_size
                    daemon = subprocess.Popen(daemon_command, cwd=ROOT, env=daemon_environment,
                        stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                        creationflags=subprocess.CREATE_NO_WINDOW)
                    owned_daemons.append(daemon)
                    wait_ready(daemon, daemon_log, endpoint, min(args.startup_timeout, remaining), offset=offset)
                    return {"after_phase": phase, "after_frame": frame,
                            "daemon_restarted": daemon.pid, "same_endpoint": endpoint}
                return None

            with (folder / "fixture.stderr.log").open("w", encoding="utf-8") as stderr, \
                    (folder / "fixture.stdout.log").open("w", encoding="utf-8") as stdout:
                present = subprocess.Popen(fixture_command, cwd=ROOT, env=environment,
                    stdin=subprocess.PIPE if args.restart_daemon else subprocess.DEVNULL,
                    stdout=subprocess.PIPE, stderr=stderr,
                    text=True, encoding="utf-8", errors="replace", creationflags=subprocess.CREATE_NO_WINDOW)
                summary["control_events"] = monitor_fixture(present, stdout, trigger, settings,
                    args.exercise_controls, args.run_timeout, stepped=args.restart_daemon,
                    callback=restart_after_frame if args.restart_daemon else None)
            summary.update(fixture_pid=present.pid, fixture_returncode=present.returncode,
                           daemon_pid=daemon.pid if daemon else None, daemon_returncode=daemon.poll() if daemon else None)
            if present.returncode:
                raise RuntimeError(f"owned Vulkan fixture failed ({present.returncode})")
            if daemon is not None and daemon.poll() is not None:
                raise RuntimeError(f"owned daemon exited while fixture was running ({daemon.returncode})")
            output = tail(folder / "fixture.stdout.log")
            validate_present_output(output, args.frames, (args.extent, args.resize_extent))
            summary["device"] = next(iter(re.findall(r"^device=.*", output, re.MULTILINE)), "")
            summary["driver"] = next(iter(re.findall(r"^driver=.*", output, re.MULTILINE)), "")
            for phase in (1, 2):
                count = len(re.findall(rf"^present phase={phase} frame=", output, re.MULTILINE))
                if count != args.frames:
                    raise RuntimeError(f"phase {phase}: got {count} presents, expected {args.frames}")
            frames = re.findall(r"^(\d+)x(\d+).*\btransport=(gpu-shared(?:-fence)?)\b", tail(daemon_log), re.MULTILINE)
            summary["gpu_frames"] = [{"width": int(w), "height": int(h), "transport": transport}
                                      for w, h, transport in frames]
            if args.fenced and any(transport != "gpu-shared-fence" for _, _, transport in frames):
                raise RuntimeError("requested fenced transport was not used for every shared GPU frame")
            extents = [tuple(map(str, size)) for size in (args.extent, args.resize_extent)]
            if not args.baseline and not args.restart_daemon and not args.unsupported and any(
                    not any((w, h) == size for w, h, _ in frames) for size in extents):
                raise RuntimeError("daemon did not confirm shared GPU processing for both swapchain extents")
            expected = {}
            for phase, size in enumerate(extents):
                expected[size] = expected.get(size, 0) + args.frames - (2 if args.exercise_controls and phase == 0 else 0)
            for extent, wanted in (expected.items() if not args.baseline and not args.restart_daemon and not args.unsupported else ()):
                observed = sum((w, h) == extent for w, h, _ in frames)
                if observed != wanted:
                    raise RuntimeError(f"extent {extent}: {observed} shared GPU frames, expected {wanted}")
            if args.restart_daemon:
                stderr_text = tail(folder / "fixture.stderr.log")
                fallback = re.findall(r"^(\d+)x(\d+).*\btransport=gpu-image-host\b",
                                      tail(daemon_log), re.MULTILINE)
                summary["fallback_frames"] = [{"width": int(w), "height": int(h)} for w, h in fallback]
                summary["original_pixel_verified"] = bool(re.search(
                    r"^original_pixel frame=3 .*status=retained\r?$", output, re.MULTILINE))
                summary["post_restart_pixel_verified"] = bool(re.search(
                    r"^original_pixel frame=5 .*status=retained\r?$", output, re.MULTILINE))
                if len(frames) != 2 or any((w, h) != extents[0] for w, h, _ in frames):
                    raise RuntimeError("outage policy expected only the two initial shared frames")
                wanted_fallback = 0 if args.gpu_only else 2 * args.frames - 3
                if len(fallback) != wanted_fallback:
                    raise RuntimeError("daemon restart did not obey the selected CPU fallback policy")
                if not summary["original_pixel_verified"] or "frame exchange failed; original frame retained" not in stderr_text:
                    raise RuntimeError("outage image did not prove that the game's original clear color was retained")
                if args.gpu_only and not summary["post_restart_pixel_verified"]:
                    raise RuntimeError("strict restart did not prove a fresh original frame after the daemon returned")
            if args.unsupported:
                summary["original_pixel_verified"] = bool(re.search(
                    r"^original_pixel frame=3 .*status=retained\r?$", output, re.MULTILINE))
                if frames or not summary["original_pixel_verified"]:
                    raise RuntimeError("unsupported GPU-only mode did not retain the exact original without processing")
            # "Acknowledged" is the daemon having RUN at the scale the harness wrote - a processed
            # frame whose line says `scale 1` - not a line about the file it read.
            #
            # Two things were wrong with asking for `^settings:.*render_scale=1`: the daemon writes
            # `settings (saved): render_scale=1.0`, which that pattern cannot match at all, and the
            # write and the daemon's re-read are a frame or two apart, so even the right line is a
            # race at 128x64 where a frame is 20 ms. The effect is the acknowledgement, and it is
            # waited for rather than assumed to have happened by a fixed frame.
            if not args.baseline and args.exercise_controls:
                if not wait_for(daemon_log, r"\bscale 1(?:\.0)?(?:\s|$)"):
                    raise RuntimeError("daemon did not acknowledge the controlled render-scale change")
            if re.search(r"^(?:frame rejected/failed|GPU lost|Traceback)", tail(daemon_log), re.MULTILINE):
                raise RuntimeError("daemon reported a frame failure or lost GPU")
            if args.gpu_only:
                successful = re.findall(r"^\d+x\d+ .*\bin [\d.]+s.*", tail(daemon_log), re.MULTILINE)
                if any(not re.search(r"\btransport=gpu-shared(?:-fence)?\b", line) for line in successful):
                    raise RuntimeError("GPU-only run used an unlabeled/CPU pixel transport")
                timing_path = folder / "layer-timing.jsonl"
                records = [json.loads(line) for line in timing_path.read_text(encoding="utf-8").splitlines()] if timing_path.is_file() else []
                if any(record.get("transport") == "cpu" and record.get("requests", 0) for record in records):
                    raise RuntimeError("GPU-only layer recorded a CPU pixel request")
                summary["cpu_pixel_requests"] = 0
            summary["present_timings"] = present_timings(output, args.warmup)
            summary["ok"] = True
    except Exception as error:
        summary["error"] = str(error)
        print("present smoke failed: " + str(error), file=sys.stderr, flush=True)
    finally:
        stop_owned(present)
        for child in owned_daemons:
            stop_owned(child)
        summary["owned_process_cleanup"] = {
            "fixture_returncode": present.returncode if present else None,
            "daemon_returncode": daemon.returncode if daemon else None}
        summary["owned_daemon_cleanup"] = [{"pid": child.pid, "returncode": child.returncode}
                                            for child in owned_daemons]
        trigger.unlink(missing_ok=True)
        summary["fixture_stderr_tail"] = tail(folder / "fixture.stderr.log", 65536)
        summary["daemon_log_tail"] = tail(daemon_log, 65536)
        summary["sync_modes"] = sorted(set(re.findall(r"GPU transport ready:.*\bsync=(\w+)",
                                                       summary["fixture_stderr_tail"])))
        summary["selected_protocols"] = sorted({2 if frame["transport"] == "gpu-shared-fence" else 1
                                                 for frame in summary.get("gpu_frames", [])})
        output = tail(folder / "fixture.stdout.log", 65536)
        summary["synchronization_validation"] = "synchronization_validation=on" in output
        # Every `validation ERROR:` in the fixture's own stderr, minus the loader's complaints about
        # ANOTHER module. The loader reports an implicit layer that will not load through the same
        # messenger - the development machine has a ReShade implicit layer whose DLL fails to open
        # in this process - and that is not this layer's verdict. The fixture skips the same message
        # for its own count; anything else is counted, and `--validation-ignore` can add patterns.
        counted = [line for line in summary["fixture_stderr_tail"].splitlines()
                   if line.startswith("validation ERROR:")]
        patterns = [re.compile(r"Failed to open dynamic library")] + \
                   [re.compile(pattern) for pattern in (args.validation_ignore or [])]
        ignored = 0
        kept = []
        for line in counted:
            if any(pattern.search(line) for pattern in patterns):
                ignored += 1
            else:
                kept.append(line)
        counted = kept
        summary["validation_errors"] = len(counted)
        summary["validation_ignored"] = ignored
        if summary["validation_errors"] or (args.sync_validation and not summary["synchronization_validation"]):
            summary["ok"] = False
            summary["error"] = "synchronization validation was unavailable or reported errors"
        atomic_json(folder / "summary.json", summary)
        print("present smoke result: " + str(folder / "summary.json"), flush=True)
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

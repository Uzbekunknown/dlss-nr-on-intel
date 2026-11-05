#!/usr/bin/env python3
"""`make test`: every check once, the ones that open no device in parallel beside the GPU's.

`make test` used to run its checks one after another, so a run kept one core busy while the
others waited. Most checks open no device at all - the tools, the CPU reference, the
protocol mocks - and need not wait for the ones that do. Each check here has a kind:

- `cpu`    no device, and its own temporary files and sockets: a pool of --jobs workers,
           running beside the GPU queue.
- `gpu`    opens the Vulkan device (libxmx, the layer, the probe, gemm_runner): the GPU
           queue, --gpu-jobs at a time. One unless asked, as `make test` always held the
           device and CTest still does: several processes on the iGPU share its memory.
- `heavy`  a `gpu` check that loads the whole network or starts a daemon: alone on the GPU
           queue, whatever --gpu-jobs says.
- `alone`  nothing else may run beside it: a timing gate, a check that reads the process
           list, a test that moves the focus. These run last, one by one.

A check that is not sure of its kind is `gpu` or `alone`, never `cpu`. The GPU queue keeps
the order below, which is the order `make test` had; the pool starts the longest first, by
the times the last run left in work/test-times.json.

Each check's output goes to work/test-logs/, and is printed whole when it ends, so a run reads
as before, only grouped by check. A failure does not stop the run (--fail-fast does); the
summary names every failure and the slowest checks.

The second memory mode changes only what opens the device: `XMX_STAGING=1 make test-gpu`
runs the `gpu` and `heavy` checks and nothing else.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "work"
LOGS = WORK / "test-logs"
TIMES = WORK / "test-times.json"
WINDOWS = os.name == "nt"
KINDS = ("cpu", "gpu", "heavy", "alone")
SKIPPED = 77            # the exit code CTest's SKIP_RETURN_CODE also reads as a skip


@dataclass
class Check:
    name: str
    argv: list
    kind: str
    cwd: Path = ROOT
    expect: str | None = None      # passes when the output holds this, as `| grep -q` did
    system: str | None = None      # "windows" or "posix" for one system's checks only


def binary(path):
    found = ROOT / path
    if WINDOWS and not found.exists():
        found = found.with_suffix(".exe")
    return str(found)


def py(script, *args, kind, system=None):
    return Check(" ".join((script, *args)), [sys.executable, str(ROOT / script), *args], kind,
                 system=system)


def exe(path, kind, system=None):
    return Check(path, [binary(path)], kind, system=system)


# The order is `make test`'s before this file, with CTest's two checks that `make test` lacked
# (the present's negative control and the daemon's view of the settings files) beside theirs.
CHECKS = [
    py("src/gpu/test_ffn_batch.py", "--gpu", kind="gpu"),
    py("src/gpu/test_device_pick.py", kind="gpu"),
    py("src/gpu/test_gemm_contract.py", kind="gpu"),
    py("src/gpu/test_denorm.py", kind="gpu"),
    py("src/gpu/test_input_fp16.py", kind="heavy"),
    py("src/gpu/test_compact_head.py", kind="heavy"),
    Check("work/coopmat_probe: NR's kernels run here", [binary("work/coopmat_probe")], "gpu",
          cwd=WORK, expect="NR's kernels (float16 8x16x16 at subgroup 32): run here"),
    exe("work/test_soft_cursor", "alone", system="windows"),
    exe("work/test_focus_gate", "alone", system="windows"),
    exe("work/test_settings_writer", "alone", system="windows"),
    exe("work/test_menu_hints", "alone", system="windows"),
    exe("work/test_input_guard", "alone", system="windows"),
    py("src/layer/test_daemon.py", kind="heavy"),
    py("src/layer/test_daemon_owner.py", kind="cpu"),
    py("src/layer/test_gpu_only.py", kind="cpu"),
    py("src/layer/test_gpu_transport.py", kind="cpu"),
    exe("work/test_surface_policy", "cpu"),
    exe("work/test_transfer_resources", "cpu"),
    exe("work/test_settled", "cpu"),
    exe("work/test_menu_rows", "alone", system="windows"),
    py("src/layer/test_menu_rows.py", kind="alone", system="windows"),
    exe("work/test_menu_persist", "alone", system="windows"),
    py("src/layer/test_ui_mask.py", kind="heavy"),
    py("src/layer/test_present.py", kind="heavy"),
    py("src/layer/test_present.py", "--negative-control", kind="heavy"),
    py("src/layer/test_temporal.py", kind="heavy"),
    py("src/layer/test_toggle.py", kind="heavy"),
    py("src/layer/test_panel.py", kind="alone"),     # reads the process list for daemons
    py("src/layer/test_spawn_exit.py", kind="cpu"),
    py("src/layer/test_settings_live.py", kind="cpu"),
    py("src/layer/test_ui_resize.py", kind="cpu"),     # was tools/check_ui_resize.py, run by nothing
    py("src/tools/publish_check.py", kind="cpu"),
    py("src/tools/claims_check.py", kind="cpu"),
    py("src/tools/build_check.py", kind="cpu"),
    py("src/tools/test_release.py", kind="cpu"),
    py("src/tools/test_setup_proxy.py", kind="cpu"),
    py("src/tools/test_player_reports.py", kind="cpu"),
    # The Windows setup's own suites. They skip what needs Windows, and until 2026-11-04 no
    # runner had them: they ran by hand on Windows, or not at all.
    py("src/tools/test_windows_wizard.py", kind="cpu"),
    py("src/tools/test_windows_launch.py", kind="cpu"),
    py("src/tools/test_windows_controls.py", kind="cpu"),
    py("src/tools/test_windows_wizard_bridge.py", kind="cpu"),
    py("src/tools/test_nr_weights.py", kind="cpu"),
    py("src/tools/test_windows_setup.py", kind="alone"),   # runs the real installer on Windows
    py("src/bench/frame_profile.py", "--tables", kind="cpu"),
    py("src/gpu/test_gemm_int8.py", kind="gpu"),
    py("src/gpu/test_gemm_int8_staged.py", kind="gpu"),
    py("src/gpu/test_window_block.py", kind="gpu"),
    py("src/gpu/test_global_attention.py", kind="gpu"),
    py("src/gpu/test_int8_quant.py", kind="gpu"),
    py("src/gpu/test_gemm_residual.py", kind="gpu"),
    py("src/gpu/test_window_residual.py", kind="gpu"),
    py("src/gpu/test_window_attention.py", kind="gpu"),
    py("src/gpu/test_gemm_qkv.py", kind="gpu"),
    py("src/gpu/test_glue.py", kind="gpu"),
    py("src/gpu/test_ffn_fused.py", kind="gpu"),
    py("src/gpu/test_staged_partial.py", kind="gpu"),
    py("src/gpu/test_staged32.py", kind="gpu"),
    py("src/gpu/test_staged_packed.py", kind="gpu"),
    py("src/gpu/test_epilogue.py", kind="gpu"),
    py("src/gpu/test_specialization.py", kind="gpu"),
    py("src/gpu/test_softmax_pack.py", kind="gpu"),
    py("src/gpu/test_qkv_fusion.py", kind="gpu"),
    py("src/gpu/test_scratch_arena.py", kind="heavy"),
    py("src/gpu/test_graph.py", kind="gpu"),
    py("src/gpu/test_frame_execution.py", kind="heavy"),
    py("src/gpu/test_image_preprocess.py", kind="gpu"),
    py("src/gpu/test_image_compose.py", kind="gpu"),
    py("src/gpu/test_image_stats.py", kind="gpu"),
    py("src/gpu/test_image_history.py", kind="gpu"),
    py("src/gpu/test_image_pipeline.py", "--full-model", kind="heavy"),
    py("src/gpu/test_resident.py", kind="heavy"),
    py("src/ref/test_frame_cache.py", kind="cpu"),
    py("src/ref/test_native_image.py", kind="cpu"),
    py("src/ref/test_topology.py", kind="cpu"),
    py("src/ref/test_nr_model.py", kind="cpu"),
    py("src/ref/test_temporal_controls.py", kind="cpu"),
    py("src/ref/test_dump_raw.py", kind="alone"),    # a timing gate: an idle machine
    py("src/ref/test_dump_geometry.py", kind="cpu"),
]


def shown(path):
    """A path as the repository names it, when it is in the repository."""
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def log_name(check):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", check.name).strip("_") + ".log"


class Run:
    def __init__(self, args, times, total):
        self.args = args
        self.times = times
        self.total = total
        self.lock = threading.Lock()
        self.results = []                  # (check, status, seconds, log path)
        self.stopping = threading.Event()  # start nothing more: a failure under --fail-fast
        self.interrupted = threading.Event()  # a Ctrl-C: what ends now is not a result
        self.children = set()

    def run(self, check):
        """One check, its output into its log; the children stay in the terminal's process
        group, so a Ctrl-C reaches them and their daemons as it did under `make`."""
        if self.stopping.is_set():
            return
        log = LOGS / log_name(check)
        started = time.monotonic()
        code = None
        with open(log, "wb") as out:
            try:
                child = subprocess.Popen(check.argv, cwd=check.cwd,
                                         env=dict(os.environ, PYTHONUNBUFFERED="1"),
                                         stdin=subprocess.DEVNULL, stdout=out,
                                         stderr=subprocess.STDOUT)
            except OSError as error:
                out.write(f"could not start: {error}\n".encode())
            else:
                with self.lock:
                    self.children.add(child)
                try:
                    code = child.wait(timeout=self.args.timeout)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                    out.write(f"\nkilled after {self.args.timeout:g} s\n".encode())
                    code = None
                finally:
                    with self.lock:
                        self.children.discard(child)
        seconds = time.monotonic() - started
        if self.interrupted.is_set():
            return                         # neither a result nor a time
        text = log.read_text(encoding="utf-8", errors="replace")
        if check.expect is not None:
            status = "PASS" if check.expect in text else "FAIL"
        else:
            status = {0: "PASS", SKIPPED: "SKIP"}.get(code, "FAIL")
        with self.lock:
            self.results.append((check, status, seconds, log))
            self.times[check.name] = round(seconds, 2)
            if not self.args.quiet or status == "FAIL":
                print(f"\n----- {check.name} ({check.kind}) -----", flush=True)
                sys.stdout.write(text if not text or text.endswith("\n") else text + "\n")
            exit_note = f"   (exit {code})" if status == "FAIL" and code is not None else ""
            print(f"[{len(self.results)}/{self.total}] {status} {seconds:6.1f} s  "
                  f"{check.kind:5}  {check.name}{exit_note}", flush=True)
        if status == "FAIL" and self.args.fail_fast:
            self.stopping.set()

    def gpu_queue(self, checks, width):
        """In the list's order, `width` at a time; a heavy check waits for the device to be
        free and holds all of it."""
        state = threading.Condition()
        running = [0]

        def body(check, slots):
            try:
                self.run(check)
            finally:
                with state:
                    running[0] -= slots
                    state.notify_all()

        threads = []
        for check in checks:
            slots = width if check.kind == "heavy" else 1
            with state:
                while running[0] + slots > width and not self.stopping.is_set():
                    state.wait(0.5)
                if self.stopping.is_set():
                    break
                running[0] += slots
            thread = threading.Thread(target=body, args=(check, slots), daemon=True)
            thread.start()
            threads.append(thread)
        for thread in threads:
            thread.join()

    def stop(self):
        self.interrupted.set()
        self.stopping.set()
        with self.lock:
            children = list(self.children)
        for child in children:
            try:
                child.kill()
            except OSError:
                pass


def default_jobs():
    configured = os.environ.get("NR_TEST_JOBS")
    if configured:
        return max(1, int(configured))
    return max(1, min(4, (os.cpu_count() or 2) - 1))


def execute(run, checks, args, times):
    if args.serial:
        for check in checks:
            run.run(check)
        return
    pool_checks = [c for c in checks if c.kind == "cpu"]
    # Longest first, by the last run's times; a check with no time yet goes first.
    pool_checks.sort(key=lambda c: -times.get(c.name, float("inf")))
    gpu_checks = [c for c in checks if c.kind in ("gpu", "heavy")]
    alone_checks = [c for c in checks if c.kind == "alone"]
    print(f"{len(checks)} checks: {len(pool_checks)} cpu in a pool of {args.jobs}, "
          f"{len(gpu_checks)} on the GPU queue {max(1, args.gpu_jobs)} at a time, "
          f"{len(alone_checks)} alone at the end. Logs in {shown(LOGS)}/",
          flush=True)
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs))
    gpu = threading.Thread(target=run.gpu_queue, args=(gpu_checks, max(1, args.gpu_jobs)),
                           daemon=True)
    try:
        futures = [pool.submit(run.run, check) for check in pool_checks]
        gpu.start()
        while gpu.is_alive():
            gpu.join(0.5)                  # a join with a timeout lets a Ctrl-C through
        for future in futures:
            while not future.done():
                time.sleep(0.2)
            future.result()
    except KeyboardInterrupt:
        run.stop()
        pool.shutdown(wait=False, cancel_futures=True)
        raise
    pool.shutdown(wait=True)
    for check in alone_checks:
        run.run(check)


def interrupted(*_):
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(
        description="Run make test's checks: the cpu ones in a pool beside the GPU queue.")
    parser.add_argument("--only", help="comma-separated kinds: cpu, gpu (with heavy), heavy, "
                                       "alone")
    parser.add_argument("-k", action="append", default=[], metavar="TEXT",
                        help="only the checks whose name holds TEXT (repeatable)")
    parser.add_argument("--jobs", type=int, default=default_jobs(),
                        help="the cpu pool's size (NR_TEST_JOBS; default the cores less one, "
                             "at most 4)")
    parser.add_argument("--gpu-jobs", type=int,
                        default=int(os.environ.get("NR_TEST_GPU_JOBS", "1")),
                        help="light gpu checks at once (NR_TEST_GPU_JOBS; default 1)")
    parser.add_argument("--serial", action="store_true",
                        help="everything one at a time in the list's order, as make test was")
    parser.add_argument("--fail-fast", action="store_true", help="start nothing after a failure")
    parser.add_argument("--timeout", type=float, default=None, help="seconds a check may take")
    parser.add_argument("--list", action="store_true", help="print the checks and their kinds")
    parser.add_argument("--quiet", action="store_true",
                        help="print a failing check's output only; every log is in work/test-logs")
    args = parser.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

    system = "windows" if WINDOWS else "posix"
    checks = [c for c in CHECKS if c.system in (None, system)]
    if args.only:
        wanted = set()
        for kind in args.only.split(","):
            kind = kind.strip()
            if kind not in KINDS:
                parser.error(f"unknown kind {kind!r}; the kinds are {', '.join(KINDS)}")
            wanted |= {"gpu", "heavy"} if kind == "gpu" else {kind}
        checks = [c for c in checks if c.kind in wanted]
    if args.k:
        checks = [c for c in checks if any(text in c.name for text in args.k)]

    try:
        times = json.loads(TIMES.read_text())
    except (OSError, ValueError):
        times = {}

    if args.list:
        for c in checks:
            last = times.get(c.name)
            took = f"{last:6.1f} s" if last is not None else "       -"
            print(f"{c.kind:5}  {took}  {c.name}")
        return 0
    if not checks:
        print("no checks selected")
        return 1

    LOGS.mkdir(parents=True, exist_ok=True)
    if not WINDOWS:
        signal.signal(signal.SIGTERM, interrupted)
    run = Run(args, times, len(checks))
    started = time.monotonic()
    try:
        execute(run, checks, args, times)
    except KeyboardInterrupt:
        run.stop()
        print("\ninterrupted", flush=True)
        return 130
    finally:
        try:
            TIMES.write_text(json.dumps(dict(sorted(run.times.items())), indent=1) + "\n")
        except OSError:
            pass

    wall = time.monotonic() - started
    counts = {s: sum(1 for r in run.results if r[1] == s) for s in ("PASS", "SKIP", "FAIL")}
    not_run = len(checks) - len(run.results)
    print(f"\n===== {len(run.results)} checks in {int(wall // 60)} min {wall % 60:.0f} s: "
          f"{counts['PASS']} passed, {counts['SKIP']} skipped, {counts['FAIL']} failed"
          + (f", {not_run} not started" if not_run else "") + " =====")
    slowest = sorted(run.results, key=lambda r: -r[2])[:5]
    print("slowest: " + ", ".join(f"{c.name} {s:.0f} s" for c, _, s, _ in slowest))
    for c, status, _, log in run.results:
        if status == "FAIL":
            print(f"FAILED  {c.name}   log: {shown(log)}")
    return 1 if counts["FAIL"] or not_run else 0


if __name__ == "__main__":
    sys.exit(main())

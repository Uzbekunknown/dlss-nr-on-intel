"""Controlled Win32 same-Intel external memory/semaphore smoke, without a game.

Default launches one private importer process and the C exporter fixture. The
importer executes GPU copies only; diagnostic pixel readback belongs to the C
fixture. --server is private to the test. Never run concurrently with GPU tests.
"""
from __future__ import annotations
import argparse
import ctypes
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "src/gpu"), str(ROOT / "src/ref"), str(ROOT / "src/layer")]


def receive(connection, count):
    result = bytearray()
    while len(result) < count:
        piece = connection.recv(count - len(result))
        if not piece:
            raise EOFError("exporter closed a control request")
        result += piece
    return bytes(result)


def serve(path, marker):
    import xmxres as X
    import nr_pipe
    import nr_gpu_transport as T

    runtime = X.Runtime()
    lib = runtime.lib
    if not lib.xmx_external_available():
        raise RuntimeError("driver does not support Win32 GPU external memory/semaphores")
    identity = (ctypes.c_ubyte * 32)()
    if lib.xmx_device_ids(identity):
        raise X.failure(lib, "interop identity query")
    contexts = {}
    server = nr_pipe.server(path, backlog=4)
    flush = nr_pipe._k32.FlushFileBuffers
    flush.argtypes = (ctypes.c_void_p,)
    flush.restype = ctypes.c_int
    Path(marker).touch()
    released = frames = 0
    while released < 2:
        connection, _ = server.accept()
        with connection:
            magic, width, height, fmt = struct.unpack("<4I", receive(connection, 16))
            if magic != T.MAGIC:
                raise ValueError("unexpected GPU magic")
            p = T.unpack_packet(receive(connection, 128), width, height, fmt)
            if T.peer_pid(connection) != p["pid"]:
                raise ValueError("exporter PID did not match the pipe")
            if p["device_uuid"] + p["driver_uuid"] != bytes(identity):
                raise ValueError("exporter selected a different Intel device or driver")
            key = p["pid"], p["session"]
            if p["action"] == T.HELLO:
                if key in contexts:
                    raise ValueError("unexpected reused session")
                # Accept both packet field spellings during the staged rename;
                # the v1 wire fields always carry actual exporter memory indices.
                input_type = p.get("input_memory_type", p.get("input_type"))
                output_type = p.get("output_memory_type", p.get("output_type"))
                inp = X.Buffer.import_win32(p["pid"], p["input_memory"], p["bytes"], p["input_alloc"], input_type)
                out = X.Buffer.import_win32(p["pid"], p["output_memory"], p["bytes"], p["output_alloc"], output_type)
                ready = done = -1
                if p["version"] == 1:
                    ready = lib.xmx_sem_import_win32(p["pid"], p["ready_sem"])
                    done = lib.xmx_sem_import_win32(p["pid"], p["done_sem"])
                    if ready < 0 or done < 0:
                        raise X.failure(lib, "interop semaphore import")
                if not inp.address or not out.address or lib.xmx_buf_host_visible(inp.id) or lib.xmx_buf_host_visible(out.id):
                    raise ValueError("imported shared image buffers must remain unmapped")
                contexts[key] = inp, out, ready, done
                detail = os.getpid()
            elif p["action"] == T.FRAME:
                inp, out, ready, done = contexts[key]
                if lib.xmx_external_acquire(inp.id, out.id, ready):
                    raise X.failure(lib, "interop acquire")
                runtime.begin()
                runtime.copy(inp, out, p["bytes"])
                runtime.submit()
                if lib.xmx_external_release(inp.id, out.id, done):
                    raise X.failure(lib, "interop release")
                frames += 1
                detail = 0
            else:
                inp, out, ready, done = contexts.pop(key)
                if (ready >= 0 and lib.xmx_sem_destroy(ready)) or (done >= 0 and lib.xmx_sem_destroy(done)):
                    raise X.failure(lib, "interop destroy semaphore")
                inp.free()
                out.free()
                released += 1
                detail = 0
            connection.sendall(T.REPLY.pack(0, detail, p["session"] & 0xffffffff, p["session"] >> 32))
            # Match production server semantics: finish delivering the reply
            # before closing the synchronous server pipe endpoint.
            if not flush(ctypes.c_void_p(connection._handle)):
                error = ctypes.get_last_error()
                if error not in (109, 232):  # client may close after reading the entire reply
                    raise OSError(error, "FlushFileBuffers")
    server.close()
    if contexts or frames != 6:
        raise AssertionError("interop sessions did not finish cleanly")
    print("Importer: six GPU-only copies, two sessions, identity and unmapped buffers verified", flush=True)


def run(executable):
    work = ROOT / "work"
    marker = work / f"gpu-interop-ready-{os.getpid()}"
    log = work / f"gpu-interop-importer-{os.getpid()}.log"
    path = rf"\\.\pipe\nr-gpu-interop-{os.getpid()}"
    marker.unlink(missing_ok=True)
    env = os.environ.copy()
    env.update(NR_GPU_TRANSPORT="1", NR_LAYER_ENABLE="0", PYTHONDONTWRITEBYTECODE="1", PYTHONUTF8="1")
    child = None
    try:
        with log.open("wb") as output:
            child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--server", path,
                                      "--marker", str(marker)], cwd=ROOT, env=env, stdout=output,
                                     stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            deadline = time.monotonic() + 15
            while not marker.exists() and child.poll() is None and time.monotonic() < deadline:
                time.sleep(.05)
            if not marker.exists():
                raise RuntimeError("importer did not become ready: " + log.read_text(encoding="utf-8", errors="replace"))
            result = subprocess.run([str(executable), path], cwd=ROOT, env=env,
                                    text=True, encoding="utf-8", errors="replace", capture_output=True,
                                    timeout=40, creationflags=subprocess.CREATE_NO_WINDOW)
            (work / f"gpu-interop-exporter-{os.getpid()}.log").write_text(result.stderr + result.stdout, encoding="utf-8")
            print(result.stdout, end="")
            if result.returncode:
                raise RuntimeError(f"exporter exited {result.returncode}: {result.stderr}\n" +
                                   log.read_text(encoding="utf-8", errors="replace"))
            if child.wait(timeout=5):
                raise RuntimeError(log.read_text(encoding="utf-8", errors="replace"))
        print(log.read_text(encoding="utf-8", errors="replace"), end="")
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            child.wait(timeout=5)
        marker.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server")
    parser.add_argument("--marker")
    parser.add_argument("--executable", type=Path, default=ROOT / "work/test_gpu_interop.exe")
    args = parser.parse_args()
    if args.server:
        serve(args.server, args.marker)
    elif os.name == "nt":
        run(args.executable)
    else:
        raise SystemExit("Win32 external GPU interop smoke requires Windows")

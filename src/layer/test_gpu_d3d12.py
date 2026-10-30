"""The GPU transport's protocol 3 against a real D3D12 client, without a game.

Default: starts a private server with the daemon's own GPUSharedTransport, whose image
pipeline is replaced by the frame as it came, so the bytes the client gets back say whether
the transport moved them; then runs the C client (test_gpu_d3d12.c). The server declines one
frame on purpose, which must still move the client's fence on. --server is private to the
test.

--network: the daemon itself, with the network and its GPU image path (NR_GPU_IMAGE), warmed
up by two frames of the host path at the same extent, and the client's frames through it. It
needs the weights in work/mlxw. Never run either concurrently with GPU tests.
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

# What test_gpu_d3d12.c expects declined: session 2's second frame.
DECLINED_WIDTH, DECLINED_VALUE = 96, 3


def receive(connection, count):
    result = bytearray()
    while len(result) < count:
        piece = connection.recv(count - len(result))
        if not piece:
            raise EOFError("client closed a control request")
        result += piece
    return bytes(result)


class Frame:
    """The image pipeline's place: hands back the frame it was given."""

    def __init__(self):
        self.reason = None
        self.last = {"network_ms": 0.0}

    def supported(self, *_args, **_kwargs):
        return self.reason

    def process_device(self, raw, *_args, motion=None):
        return raw

    def reset_history(self):
        pass

    def close(self):
        pass


def serve(path, marker):
    import xmxres as X
    import nr_pipe
    import nr_gpu_transport as T

    runtime = X.Runtime()
    lib = runtime.lib
    if not lib.xmx_external_available():
        raise RuntimeError("driver does not support Win32 GPU external memory")
    backend = type("Backend", (), {})()
    backend.runtime = runtime
    images = Frame()
    transport = T.GPUSharedTransport(backend, enabled=True)
    transport.images = images
    server = nr_pipe.server(path, backlog=4)
    flush = nr_pipe._k32.FlushFileBuffers
    flush.argtypes = (ctypes.c_void_p,)
    flush.restype = ctypes.c_int
    Path(marker).touch()
    released = answered = declined = 0
    while released < 2:
        connection, _ = server.accept()
        with connection:
            magic, width, height, fmt = struct.unpack("<4I", receive(connection, 16))
            if magic != T.MAGIC:
                raise ValueError("unexpected GPU magic")
            seen = {}

            def read(connection, count):
                raw = receive(connection, count)
                seen.update(T.unpack_packet(raw, width, height, fmt))
                if seen["version"] != T.D3D12:
                    raise ValueError("the client did not speak protocol 3")
                images.reason = ("declined by the test" if width == DECLINED_WIDTH and
                                 seen["action"] == T.FRAME and seen["done_sem"] == DECLINED_VALUE
                                 else None)
                return raw

            metrics = transport.handle(connection, width, height, fmt, read, object())
            if seen["action"] == T.FRAME:
                if metrics:
                    if metrics.get("sync") != "d3d12":
                        raise AssertionError("a D3D12 frame was not taken through protocol 3")
                    answered += 1
                else:
                    declined += 1
                value = ctypes.c_ulonglong()
                context = transport.contexts[(seen["pid"], seen["session"])]
                if lib.xmx_sem_value(context.ready, ctypes.byref(value)) or value.value != seen["done_sem"] + 1:
                    raise AssertionError(f"fence at {value.value}, not {seen['done_sem'] + 1}")
            elif seen["action"] == T.RELEASE:
                released += 1
            if not flush(ctypes.c_void_p(connection._handle)):
                error = ctypes.get_last_error()
                if error not in (109, 232):  # client may close after reading the entire reply
                    raise OSError(error, "FlushFileBuffers")
    server.close()
    if transport.contexts or (answered, declined) != (7, 1):
        raise AssertionError(f"sessions left {len(transport.contexts)}, answered {answered}, declined {declined}")
    transport.close()
    print("Server: protocol 3 through the daemon's transport, 7 frames answered, 1 declined, "
          "every fence moved on, two sessions released", flush=True)


def tail(path, size=1 << 16):
    try:
        return path.read_bytes()[-size:].decode("utf-8", errors="replace")
    except OSError:
        return ""


def network(executable):
    """The client's frames through the daemon as the layer starts it, network and all."""
    import json
    work = ROOT / "work"
    log = work / f"gpu-d3d12-daemon-{os.getpid()}.log"
    settings = work / f"gpu-d3d12-settings-{os.getpid()}.json"
    settings.write_text(json.dumps({"render_scale": 0.5, "min_extent": 320}), encoding="utf-8")
    path = rf"\\.\pipe\nr-gpu-d3d12-net-{os.getpid()}"
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("NR_")}
    env.update(NR_GPU_IMAGE="1", NR_GPU_TRANSPORT="1", NR_KEEP_BLOCKS="1", NR_LAYER_SPAWNED=str(os.getpid()),
               PYTHONUTF8="1", PYTHONDONTWRITEBYTECODE="1")
    for key in ("NR_PRESET", "NR_DLL_PATH"):
        if os.environ.get(key):
            env[key] = os.environ[key]
    width, height, frames = 640, 360, 6
    daemon = None
    try:
        with log.open("wb") as output:
            daemon = subprocess.Popen([sys.executable, str(ROOT / "src/layer/nr_daemon.py"), "--socket", path,
                                       "--settings", str(settings), "--root", str(ROOT)],
                                      cwd=ROOT, env=env, stdin=subprocess.DEVNULL, stdout=output,
                                      stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        deadline = time.monotonic() + 600
        while "listening on" not in tail(log):
            if daemon.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("the daemon did not listen:\n" + tail(log))
            time.sleep(.2)
        # Two frames of the host path build the graph for this extent, which takes longer than
        # one control exchange may.
        header = struct.pack("<4I", 0x304E524E, width, height, 37)
        payload = bytes(range(256)) * (width * height * 4 // 256)
        for _ in range(2):
            with open(path, "r+b", buffering=0) as link:
                data = memoryview(header + payload)
                while data:
                    data = data[link.write(data):]
                got = 0
                while got < len(payload):
                    piece = link.read(len(payload) - got)
                    if not piece:
                        raise RuntimeError("the daemon refused the warm-up frame:\n" + tail(log))
                    got += len(piece)
        result = subprocess.run([str(executable), path, "--network"], cwd=ROOT, env=env,
                                text=True, encoding="utf-8", errors="replace", capture_output=True,
                                timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
        print(result.stdout, end="")
        if result.returncode:
            raise RuntimeError(f"client exited {result.returncode}: {result.stderr}\n" + tail(log))
        lines = [line for line in tail(log).splitlines() if "transport=gpu-shared-d3d12" in line]
        if len(lines) != frames:
            raise AssertionError(f"the daemon logged {len(lines)} protocol 3 frames, not {frames}:\n" + tail(log))
        print("Daemon:", lines[-1], flush=True)
    finally:
        if daemon is not None and daemon.poll() is None:
            daemon.terminate()
            daemon.wait(timeout=30)
        settings.unlink(missing_ok=True)


def run(executable):
    work = ROOT / "work"
    marker = work / f"gpu-d3d12-ready-{os.getpid()}"
    log = work / f"gpu-d3d12-server-{os.getpid()}.log"
    path = rf"\\.\pipe\nr-gpu-d3d12-{os.getpid()}"
    marker.unlink(missing_ok=True)
    env = os.environ.copy()
    env.update(NR_LAYER_ENABLE="0", PYTHONDONTWRITEBYTECODE="1", PYTHONUTF8="1")
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
                raise RuntimeError("server did not become ready: " + log.read_text(encoding="utf-8", errors="replace"))
            result = subprocess.run([str(executable), path], cwd=ROOT, env=env,
                                    text=True, encoding="utf-8", errors="replace", capture_output=True,
                                    timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
            print(result.stdout, end="")
            if result.returncode:
                raise RuntimeError(f"client exited {result.returncode}: {result.stderr}\n" +
                                   log.read_text(encoding="utf-8", errors="replace"))
            if child.wait(timeout=10):
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
    parser.add_argument("--executable", type=Path, default=ROOT / "work/test_gpu_d3d12.exe")
    parser.add_argument("--network", action="store_true", help="through the daemon itself, with the network")
    args = parser.parse_args()
    if args.server:
        serve(args.server, args.marker)
    elif os.name == "nt":
        (network if args.network else run)(args.executable)
    else:
        raise SystemExit("the D3D12 GPU transport smoke requires Windows")

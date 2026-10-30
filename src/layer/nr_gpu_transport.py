"""Authenticated, same-Arc GPU requests; control messages contain no pixels.

The caller must explicitly enable this experimental transport. Imported resource
lifetimes belong to the authenticated process handle and the session, rather than
to an individual short-lived pipe connection. Cleanup is acknowledged only after
the runtime confirms that all imports were released.
"""
from __future__ import annotations

import ctypes
import os
import struct
import time

import xmxres as X
from gpu_image_pipeline import GPUImagePipeline

MAGIC = 0x3147524E
PACKET = struct.Struct("<8I8Q16s16s")
REPLY = struct.Struct("<4I")
HELLO, FRAME, RELEASE = 0, 1, 2


def peer_pid(connection):
    if os.name != "nt" or not getattr(connection, "_handle", None):
        raise ValueError("GPU requests require an authenticated Windows named pipe")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    query = kernel.GetNamedPipeClientProcessId
    query.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32))
    query.restype = ctypes.c_int
    pid = ctypes.c_uint32()
    if not query(ctypes.c_void_p(connection._handle), ctypes.byref(pid)):
        raise OSError(ctypes.get_last_error(), "GetNamedPipeClientProcessId")
    return pid.value


class _ProcessLifetime:
    """Retain the authenticated HELLO process identity even after PID reuse."""

    def __init__(self, pid):
        if os.name != "nt":
            raise ValueError("GPU process lifetimes require Windows")
        kernel = self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        kernel.WaitForSingleObject.restype = ctypes.c_uint32
        kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
        kernel.CloseHandle.restype = ctypes.c_int
        self.handle = kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE only
        if not self.handle:
            raise OSError(ctypes.get_last_error(), "OpenProcess for authenticated GPU client")

    def exited(self):
        result = self.kernel.WaitForSingleObject(self.handle, 0)
        if result == 0:  # WAIT_OBJECT_0
            return True
        if result == 258:  # WAIT_TIMEOUT
            return False
        raise OSError(ctypes.get_last_error(), "WaitForSingleObject for GPU client")

    def close(self):
        if self.handle:
            if not self.kernel.CloseHandle(self.handle):
                raise OSError(ctypes.get_last_error(), "CloseHandle for GPU client")
            self.handle = None


def unpack_packet(raw, width, height, fmt):
    if len(raw) != PACKET.size:
        raise ValueError("GPU control packet must be 128 bytes")
    names = ("action", "pid", "width", "height", "format", "input_memory_type", "output_memory_type", "version",
             "session", "input_alloc", "output_alloc", "bytes", "input_memory", "output_memory",
             "ready_sem", "done_sem", "device_uuid", "driver_uuid")
    p = dict(zip(names, PACKET.unpack(raw)))
    if p["action"] not in (HELLO, FRAME, RELEASE) or p["version"] not in (1, 2) or not p["session"]:
        raise ValueError("unsupported GPU protocol action/version/session")
    if (p["width"], p["height"], p["format"]) != (width, height, fmt):
        raise ValueError("GPU control header and packet geometry disagree")
    # No extent ceiling: an empty extent is a corrupt header, but a LARGE one is a real swapchain.
    # The byte count is what has to agree - it is the size the client actually allocated, and a
    # mismatch is the only case where importing would read past it.
    if width < 1 or height < 1 or p["bytes"] != width * height * 4:
        raise ValueError("GPU control extent or byte count is invalid")
    if fmt not in (37, 43, 44, 50):
        raise ValueError("GPU transport supports SDR RGBA/BGRA only")
    for key in ("input_alloc", "output_alloc"):
        if p[key] < p["bytes"] or p[key] > (1 << 30):
            raise ValueError("invalid GPU allocation size")
    if any(p[key] >= 32 for key in ("input_memory_type", "output_memory_type")):
        raise ValueError("invalid GPU memory type index")
    if not p["pid"] or any(not p[k] for k in ("input_memory", "output_memory")):
        raise ValueError("GPU control request lacks process/handles")
    if p["version"] == 1 and (not p["ready_sem"] or not p["done_sem"]):
        raise ValueError("GPU semaphore protocol requires ready/done handles")
    if p["version"] == 2 and (p["ready_sem"] or p["done_sem"]):
        raise ValueError("GPU fence protocol must not carry semaphore handles")
    if p["input_memory"] == p["output_memory"] or (p["version"] == 1 and p["ready_sem"] == p["done_sem"]):
        raise ValueError("GPU input/output resources must be distinct")
    return p


class _Context:
    def __init__(self, lib, packet, process):
        self.lib, self.packet, self.process = lib, packet, process
        self.input = self.output = None
        self.ready = self.done = -1
        self.usable = False

    def open(self):
        p = self.packet
        self.input = X.Buffer.import_win32(p["pid"], p["input_memory"], p["bytes"],
                                           p["input_alloc"], p["input_memory_type"])
        self.output = X.Buffer.import_win32(p["pid"], p["output_memory"], p["bytes"],
                                            p["output_alloc"], p["output_memory_type"])
        if p["version"] == 1:
            self.ready = self.lib.xmx_sem_import_win32(p["pid"], p["ready_sem"])
            if self.ready < 0:
                raise X.failure(self.lib, "GPU ready semaphore import")
            self.done = self.lib.xmx_sem_import_win32(p["pid"], p["done_sem"])
            if self.done < 0:
                raise X.failure(self.lib, "GPU done semaphore import")
        self.usable = True

    def close(self):
        self.usable = False
        if self.lib.xmx_device_lost():
            raise X.failure(self.lib, "GPU session cleanup on lost device; resources remain pinned")
        for name in ("ready", "done"):
            ident = getattr(self, name)
            if ident >= 0:
                if self.lib.xmx_sem_destroy(ident) or self.lib.xmx_device_lost():
                    raise X.failure(self.lib, "GPU imported semaphore cleanup")
                setattr(self, name, -1)
        for name in ("input", "output"):
            buffer = getattr(self, name)
            if buffer is not None:
                # Buffer.free historically discarded its id even on destroy failure.
                # Verify removal as a pending external submit may pin memory.
                if self.lib.xmx_buf_destroy(buffer.id) or self.lib.xmx_device_lost():
                    raise X.failure(self.lib, "GPU imported buffer cleanup")
                if self.lib.xmx_buf_bytes(buffer.id):
                    raise RuntimeError("GPU imported buffer cleanup: resources remain pinned")
                buffer.id = -1
                setattr(self, name, None)
        self.process.close()


class GPUSharedTransport:
    def __init__(self, backend, *, enabled=False):
        self.backend, self.lib = backend, backend.runtime.lib
        self.enabled = bool(enabled)
        self.contexts = {}
        self.images = None  # disabled requests allocate no GPU resources
        self.active = None
        self.last = {}
        self.fallback_reason = None

    def rebind_backend(self, backend):
        """Follow a model swap, keeping the imports the game established.

        The backend this transport was built with belongs to the model it was built with, and the
        engine is rebuilt lazily from `self.backend` on the next frame - so a swap that leaves
        this object alone would rebuild the pipeline on the model it just replaced. The imports
        in `self.contexts` are the game's and are untouched by this: only the engine they feed
        goes, which is the same thing `close_gpu_images(keep_sessions=True)` does on the daemon
        side. Called with the new backend after every swap."""
        if backend is self.backend:
            return
        self.backend, self.lib = backend, backend.runtime.lib
        engine, self.images = self.images, None
        if engine is not None:
            engine.close()
        self.active = None

    def _reply(self, connection, p, status, detail=0):
        connection.sendall(REPLY.pack(status, detail, p["session"] & 0xffffffff, p["session"] >> 32))

    def _failure_reply(self, connection, p):
        # A disconnected pipe must not mask DeviceLost or a cleanup failure.
        try:
            self._reply(connection, p, 2)
        except OSError:
            pass

    def reset_history(self):
        if self.images is not None:
            self.images.reset_history()
        self.active = None

    def cleanup_stale(self):
        """Reap only clients whose retained authenticated process has exited.

        Called on each request so a crashed game cannot occupy both slots
        forever. Unknown process state or failed cleanup retains the imports.
        GPU cleanup stays on the daemon's frame thread.
        """
        for key, context in list(self.contexts.items()):
            if context.process.exited():
                context.close()
                del self.contexts[key]
                if self.active == key:
                    self.reset_history()

    def _decline(self, connection, p, reason):
        self.reset_history()
        self._reply(connection, p, 1)
        if self.fallback_reason != reason:
            print("GPU shared transport fallback: " + reason, flush=True)
            self.fallback_reason = reason
        return None

    def handle(self, connection, width, height, fmt, read, live, *, diagnostic=False):
        p = unpack_packet(read(connection, PACKET.size), width, height, fmt)
        key = (p["pid"], p["session"])
        if peer_pid(connection) != p["pid"]:
            self._reply(connection, p, 1)
            raise ValueError("GPU packet process does not match named-pipe client")
        if not self.enabled:
            return self._decline(connection, p, "experimental GPU image/transport is disabled")
        try:
            self.cleanup_stale()
        except Exception:
            self._failure_reply(connection, p)
            raise
        context = self.contexts.get(key)
        if context is not None and context.packet != {**p, "action": HELLO}:
            self._reply(connection, p, 2)
            raise ValueError("GPU request metadata does not match imported session")
        if p["action"] == RELEASE:
            try:
                if context is not None:
                    context.close()
                    del self.contexts[key]
                if self.active == key:
                    self.reset_history()
            except Exception:
                self._failure_reply(connection, p)
                raise
            self._reply(connection, p, 0)
            return None
        identity = (ctypes.c_ubyte * 32)()
        if self.lib.xmx_device_ids(identity):
            self._failure_reply(connection, p)
            raise X.failure(self.lib, "GPU identity query")
        if self.lib.xmx_device_lost():
            self._failure_reply(connection, p)
            raise X.failure(self.lib, "GPU request on lost device")
        if not self.lib.xmx_external_available():
            return self._decline(connection, p, "driver lacks GPU memory/semaphore sharing")
        if bytes(identity) != p["device_uuid"] + p["driver_uuid"]:
            return self._decline(connection, p, "game and network must use the same Arc and driver")
        if self.images is None:
            self.images = GPUImagePipeline(self.backend)
        reason = self.images.supported(width, height, fmt, live, diagnostic=diagnostic)
        if reason:
            return self._decline(connection, p, reason)
        self.fallback_reason = None
        if p["action"] == HELLO:
            if context is not None and not context.usable:
                self._reply(connection, p, 2)
                raise ValueError("GPU imported session requires cleanup before reuse")
            if context is None:
                if len(self.contexts) >= 2:
                    return self._decline(connection, p, "GPU imported session capacity reached")
                try:
                    process = _ProcessLifetime(p["pid"])
                    context = self.contexts[key] = _Context(self.lib, {**p}, process)
                    context.open()
                except X.DeviceLost:
                    self._failure_reply(connection, p)
                    raise
                except (ValueError, OSError, RuntimeError) as error:
                    if context is not None:
                        try:
                            context.close()
                            del self.contexts[key]
                        except Exception:
                            self._failure_reply(connection, p)
                            raise
                    return self._decline(connection, p, str(error))
            self._reply(connection, p, 0, os.getpid())
            return None
        if context is None or not context.usable:
            self._reply(connection, p, 2)
            raise ValueError("GPU frame has no matching imported session")
        if self.active != key:
            self.reset_history()
            self.active = key
        started = time.perf_counter()
        acquired = False
        try:
            if self.lib.xmx_external_acquire(context.input.id, context.output.id, context.ready):
                raise X.failure(self.lib, "GPU input ownership acquire")
            acquired = True
            result = self.images.process_device(context.input, width, height, fmt, live)
            self.backend.runtime.begin()
            self.backend.runtime.copy(result, context.output, p["bytes"])
            self.backend.runtime.submit()
            if self.lib.xmx_external_release(context.input.id, context.output.id, context.done):
                raise X.failure(self.lib, "GPU output ownership release")
            acquired = False
            # v2's caller waits its private capture fence before FRAME and does
            # not start its output copy until this release fence and ACK finish.
            # Pixel ownership still transfers through GPU queue-family barriers.
            self.last = {**self.images.last, "total_seconds": time.perf_counter() - started,
                         "sync": "fence" if p["version"] == 2 else "semaphore"}
            self._reply(connection, p, 0)
            return self.last
        except Exception:
            context.usable = False
            self.reset_history()
            try:
                if acquired and not self.lib.xmx_device_lost():
                    if self.lib.xmx_external_release(context.input.id, context.output.id, context.done):
                        raise X.failure(self.lib, "GPU failed-frame ownership release")
            except Exception:
                self._failure_reply(connection, p)
                raise
            self._failure_reply(connection, p)
            raise

    def close(self):
        for key, context in list(self.contexts.items()):
            context.close()
            del self.contexts[key]
        if self.images is not None:
            self.images.close()
        self.active = None

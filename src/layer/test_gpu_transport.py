"""Host-only same-device control protocol and imported-resource lifecycle checks."""
from __future__ import annotations
import contextlib
import ctypes
import io
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path[:0] = [str(Path(__file__).resolve().parent),
               str(Path(__file__).resolve().parents[1] / "gpu"),
               str(Path(__file__).resolve().parents[1] / "ref")]
import nr_gpu_transport as T

DEVICE_UUID = bytes([12]) * 16
DRIVER_UUID = bytes([34]) * 16


def packet(action=T.HELLO, **changes):
    p = dict(action=action, pid=42, width=1280, height=720, format=44,
             input_memory_type=1, output_memory_type=2, version=1,
             session=0x9876543210203040, input_alloc=1280 * 720 * 4 + 256,
             output_alloc=1280 * 720 * 4 + 256, bytes=1280 * 720 * 4,
             input_memory=100, output_memory=101, ready_sem=102, done_sem=103,
             device_uuid=DEVICE_UUID, driver_uuid=DRIVER_UUID)
    p.update(changes)
    return p


def encoded(p):
    return T.PACKET.pack(*p.values())


class Connection:
    def __init__(self, p, events):
        self.raw, self.events, self.replies = encoded(p), events, []

    def sendall(self, raw):
        reply = T.REPLY.unpack(raw)
        self.replies.append(reply)
        self.events.append(("reply", reply[0]))


class Buffer:
    def __init__(self, ident, events):
        self.id, self.events, self.freed = ident, events, False

    def free(self):
        self.events.append(("free", self.id))
        self.freed = True


class Library:
    def __init__(self, events):
        self.events, self.external, self.lost = events, 1, 0
        self.next_sem, self.close_error = 0, 0
        self.buffers = {}
        self.destroy_error = 0
        self.pin_buffers = False

    def xmx_device_ids(self, out):
        out[:] = DEVICE_UUID + DRIVER_UUID
        return 0

    def xmx_external_available(self):
        return self.external

    def xmx_device_lost(self):
        return self.lost

    def xmx_sem_import_win32(self, pid, handle):
        self.events.append(("import_sem", pid, handle))
        self.next_sem += 1
        return self.next_sem

    def xmx_sem_destroy(self, ident):
        self.events.append(("destroy_sem", ident))
        return self.close_error

    def xmx_buf_destroy(self, ident):
        self.events.append(("free", ident))
        if not self.destroy_error and not self.pin_buffers:
            self.buffers.pop(ident, None)
        return self.destroy_error

    def xmx_buf_bytes(self, ident):
        return 4 if ident in self.buffers else 0

    def xmx_external_acquire(self, a, b, ready):
        self.events.append(("acquire", a, b, ready))
        return 0

    def xmx_external_release(self, a, b, done):
        self.events.append(("release", a, b, done))
        return 0


class Runtime:
    def __init__(self, lib, events):
        self.lib, self.events = lib, events

    def begin(self):
        self.events.append(("begin",))

    def copy(self, source, target, count):
        self.events.append(("copy", source.id, target.id, count))

    def submit(self):
        self.events.append(("submit",))


class Backend:
    def __init__(self, lib, events):
        self.runtime = Runtime(lib, events)


class Images:
    def __init__(self, events):
        self.events, self.reason, self.error = events, None, False
        self.last = {"network_ms": 3.5}
        self.before_process = None

    def supported(self, *_args, **_kwargs):
        return self.reason

    def process_device(self, raw, *_args):
        self.events.append(("process", raw.id))
        if self.before_process:
            self.before_process()
        if isinstance(self.error, Exception):
            raise self.error
        if self.error:
            raise ValueError("controlled pipeline failure")
        return Buffer(99, self.events)

    def reset_history(self):
        self.events.append(("reset_history",))

    def close(self):
        self.events.append(("close_images",))


class Process:
    def __init__(self, pid, events):
        self.pid, self.events = pid, events
        self.dead = self.closed = False
        self.wait_error = None

    def exited(self):
        if self.wait_error is not None:
            raise self.wait_error
        return self.dead

    def close(self):
        self.events.append(("close_process", self.pid))
        self.closed = True


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.events, self.imports, self.processes = [], [], []
        self.lib = Library(self.events)
        self.images = Images(self.events)
        self.patches = [
            patch.object(T, "peer_pid", return_value=42),
            patch.object(T, "GPUImagePipeline", return_value=self.images),
            patch.object(T.X.Buffer, "import_win32", side_effect=self.import_buffer),
            patch.object(T, "_ProcessLifetime", side_effect=self.process),
            patch.object(T.X, "failure", side_effect=lambda _lib, what:
                         T.X.DeviceLost(what) if _lib.lost else RuntimeError(what)),
        ]
        for p in self.patches:
            p.start()
            self.addCleanup(p.stop)
        self.transport = T.GPUSharedTransport(Backend(self.lib, self.events), enabled=True)

    def process(self, pid):
        value = Process(pid, self.events)
        self.processes.append(value)
        return value

    def import_buffer(self, *arguments):
        self.imports.append(arguments)
        self.events.append(("import_buffer", *arguments))
        buffer = Buffer(len(self.imports), self.events)
        self.lib.buffers[buffer.id] = buffer
        return buffer

    def request(self, p):
        connection = Connection(p, self.events)

        def read(connection, length):
            self.assertEqual(length, 128)
            return connection.raw

        with contextlib.redirect_stdout(io.StringIO()):
            result = self.transport.handle(connection, p["width"], p["height"], p["format"], read, object())
        return connection, result

    def test_memory_indices_and_packet_layout(self):
        p = packet()
        self.assertEqual(T.PACKET.size, 128)
        got = T.unpack_packet(encoded(p), 1280, 720, 44)
        self.assertEqual(got["input_memory_type"], 1)
        self.assertEqual(got["output_memory_type"], 2)
        connection, result = self.request(p)
        self.assertIsNone(result)
        self.assertEqual(self.imports, [(42, 100, p["bytes"], p["input_alloc"], 1),
                                      (42, 101, p["bytes"], p["output_alloc"], 2)])
        reply = connection.replies[-1]
        self.assertEqual(reply[:2], (0, os.getpid()))
        self.assertEqual((reply[3] << 32) | reply[2], p["session"])

    def test_bad_extent_and_allocation_rejected_before_import(self):
        for changed in ({"bytes": 4}, {"input_alloc": 1}, {"output_alloc": (1 << 30) + 1},
                        {"session": 0}, {"version": 2}, {"format": 64}, {"input_memory": 0},
                        {"input_memory_type": 32}, {"output_memory_type": 0xffffffff},
                        {"input_memory": 101}, {"ready_sem": 103}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                self.request(packet(**changed))
        self.assertFalse(self.imports)

    def test_client_pid_is_authenticated(self):
        with patch.object(T, "peer_pid", return_value=43), self.assertRaisesRegex(ValueError, "process"):
            self.request(packet())
        self.assertFalse(self.imports)
        self.assertEqual(self.events[-1], ("reply", 1))

    def test_v2_fence_protocol_imports_memory_without_semaphores(self):
        p = packet(version=2, ready_sem=0, done_sem=0)
        parsed = T.unpack_packet(encoded(p), 1280, 720, 44)
        self.assertEqual(parsed["version"], 2)
        connection, _ = self.request(p)
        self.assertEqual(connection.replies[-1][0], 0)
        context = self.transport.contexts[(42, p["session"])]
        self.assertEqual((context.ready, context.done), (-1, -1))
        self.assertNotIn("import_sem", [e[0] for e in self.events])
        self.assertEqual(len(self.imports), 2)

    def test_protocol_versions_require_their_own_semaphore_contract(self):
        for changes in ({"version": 1, "ready_sem": 0}, {"version": 1, "done_sem": 0},
                        {"version": 2, "ready_sem": 0}, {"version": 2, "done_sem": 0},
                        {"version": 2}, {"version": 3, "ready_sem": 0, "done_sem": 0}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.request(packet(**changes))
        self.assertFalse(self.imports)

    def test_v2_frame_fences_release_ownership_before_ack_without_semaphores(self):
        self.request(packet(version=2, ready_sem=0, done_sem=0))
        self.events.clear()
        connection, result = self.request(packet(T.FRAME, version=2, ready_sem=0, done_sem=0))
        self.assertEqual([e[0] for e in self.events],
                         ["reset_history", "acquire", "process", "begin", "copy", "submit", "release", "reply"])
        self.assertEqual(self.events[1], ("acquire", 1, 2, -1))
        self.assertEqual(self.events[-2], ("release", 1, 2, -1))
        self.assertEqual(result["sync"], "fence")
        self.assertEqual(connection.replies[-1][0], 0)

    def test_v2_release_checks_memory_cleanup_and_imported_version_is_immutable(self):
        self.request(packet(version=2, ready_sem=0, done_sem=0))
        self.events.clear()
        with self.assertRaisesRegex(ValueError, "metadata"):
            self.request(packet(T.FRAME))
        self.assertEqual(self.events, [("reply", 2)])
        self.events.clear()
        connection, _ = self.request(packet(T.RELEASE, version=2, ready_sem=0, done_sem=0))
        self.assertEqual([e[0] for e in self.events], ["free", "free", "close_process", "reply"])
        self.assertEqual(connection.replies[-1][0], 0)
        self.assertFalse(self.transport.contexts)

    def test_device_and_driver_mismatch_decline_before_import(self):
        for changed in ({"device_uuid": bytes(16)}, {"driver_uuid": bytes(16)}):
            connection, result = self.request(packet(**changed))
            self.assertIsNone(result)
            self.assertEqual(connection.replies[-1][0], 1)
        self.assertFalse(self.imports)

    def test_hello_retry_is_idempotent(self):
        self.request(packet())
        connection, _ = self.request(packet())
        self.assertEqual(len(self.imports), 2)
        self.assertEqual(connection.replies[-1][0], 0)

    def test_hello_changed_handles_do_not_acknowledge_stale_imports(self):
        self.request(packet())
        self.events.clear()
        try:
            connection, _ = self.request(packet(input_memory=555))
            self.assertNotEqual(connection.replies[-1][0], 0)
        except ValueError:
            self.assertNotIn(("reply", 0), self.events)
        self.assertEqual(len(self.imports), 2)

    def test_frame_has_ordered_acquire_process_release_and_ack(self):
        self.request(packet())
        self.events.clear()
        connection, result = self.request(packet(T.FRAME))
        self.assertEqual([e[0] for e in self.events],
                         ["reset_history", "acquire", "process", "begin", "copy", "submit", "release", "reply"])
        self.assertEqual(result["network_ms"], 3.5)
        self.assertEqual(connection.replies[-1][0], 0)

    def test_failed_pipeline_releases_ownership_before_failure_ack(self):
        self.request(packet())
        self.images.error = True
        self.events.clear()
        with self.assertRaisesRegex(ValueError, "controlled pipeline"):
            self.request(packet(T.FRAME))
        self.assertEqual(self.events[-2][0], "release")
        self.assertEqual(self.events[-1], ("reply", 2))

    def test_frame_metadata_must_match_imported_session(self):
        self.request(packet())
        self.events.clear()
        with self.assertRaises(ValueError):
            self.request(packet(T.FRAME, output_memory=555))
        self.assertNotIn("acquire", [e[0] for e in self.events])
        self.assertEqual(self.events[-1], ("reply", 2))

    def test_release_frees_imports_before_acknowledgement(self):
        self.request(packet())
        self.transport.active = (42, packet()["session"])
        self.events.clear()
        connection, _ = self.request(packet(T.RELEASE))
        self.assertEqual([e[0] for e in self.events],
                         ["destroy_sem", "destroy_sem", "free", "free", "close_process", "reset_history", "reply"])
        self.assertEqual(connection.replies[-1][0], 0)
        self.assertFalse(self.transport.contexts)

    def test_release_on_lost_runtime_never_acknowledges_safe_cleanup(self):
        p = packet()
        self.request(p)
        self.lib.lost = 1
        self.events.clear()
        with self.assertRaises(RuntimeError):
            self.request(packet(T.RELEASE))
        self.assertNotIn(("reply", 0), self.events)
        self.assertIn((p["pid"], p["session"]), self.transport.contexts)

    def test_failed_semaphore_cleanup_never_acknowledges_release(self):
        p = packet()
        self.request(p)
        self.lib.close_error = -1
        self.events.clear()
        with self.assertRaises(RuntimeError):
            self.request(packet(T.RELEASE))
        self.assertNotIn(("reply", 0), self.events)
        self.assertIn((p["pid"], p["session"]), self.transport.contexts)

    def test_disabled_transport_is_opt_in_and_allocates_no_gpu_pipeline(self):
        self.transport = T.GPUSharedTransport(Backend(self.lib, self.events))
        connection, result = self.request(packet())
        self.assertIsNone(result)
        self.assertEqual(connection.replies[-1][0], 1)
        self.assertIsNone(self.transport.images)
        self.assertFalse(self.imports)
        self.assertFalse(self.processes)

    def test_changed_release_metadata_preserves_original_imports(self):
        p = packet()
        self.request(p)
        self.events.clear()
        with self.assertRaisesRegex(ValueError, "metadata"):
            self.request(packet(T.RELEASE, ready_sem=999))
        self.assertIn((42, p["session"]), self.transport.contexts)
        self.assertEqual(self.events, [("reply", 2)])

    def test_buffer_destroy_failure_retains_id_and_cleanup_can_be_retried(self):
        p = packet()
        self.request(p)
        context = self.transport.contexts[(42, p["session"])]
        buffer = context.input
        self.lib.destroy_error = -1
        with self.assertRaisesRegex(RuntimeError, "buffer cleanup"):
            self.request(packet(T.RELEASE))
        self.assertIs(context.input, buffer)
        self.assertEqual(buffer.id, 1)
        self.assertFalse(context.usable)
        self.assertFalse(context.process.closed)
        self.lib.destroy_error = 0
        connection, _ = self.request(packet(T.RELEASE))
        self.assertEqual(connection.replies[-1][0], 0)
        self.assertEqual(buffer.id, -1)
        self.assertFalse(self.transport.contexts)

    def test_pinned_buffer_with_zero_destroy_result_never_acknowledges_release(self):
        p = packet()
        self.request(p)
        self.lib.pin_buffers = True
        self.events.clear()
        with self.assertRaisesRegex(RuntimeError, "pinned"):
            self.request(packet(T.RELEASE))
        context = self.transport.contexts[(42, p["session"])]
        self.assertEqual(context.input.id, 1)
        self.assertNotIn(("reply", 0), self.events)

    def test_lost_device_during_import_is_propagated_with_context_retained(self):
        p = packet()

        def lost_import(*args):
            self.lib.lost = 1
            raise T.X.DeviceLost("controlled import loss")

        self.events.clear()
        with patch.object(T.X.Buffer, "import_win32", side_effect=lost_import), \
                self.assertRaises(T.X.DeviceLost):
            self.request(p)
        self.assertIn((42, p["session"]), self.transport.contexts)
        self.assertNotIn(("reply", 0), self.events)
        self.assertFalse(self.processes[0].closed)

    def test_lost_device_is_not_masked_by_disconnected_failure_reply(self):
        self.request(packet())
        self.images.error = T.X.DeviceLost("controlled pipeline loss")
        self.images.before_process = lambda: setattr(self.lib, "lost", 1)
        connection = Connection(packet(T.FRAME), self.events)
        with patch.object(connection, "sendall", side_effect=BrokenPipeError("gone")), \
                self.assertRaises(T.X.DeviceLost):
            self.transport.handle(connection, 1280, 720, 44, lambda c, n: c.raw, object())
        self.assertNotIn("release", [e[0] for e in self.events])

    def test_failed_import_cleans_partial_resources_before_decline(self):
        original = self.import_buffer

        def fail_second(*args):
            if self.imports:
                raise RuntimeError("controlled second import failure")
            return original(*args)

        with patch.object(T.X.Buffer, "import_win32", side_effect=fail_second):
            connection, _ = self.request(packet())
        self.assertEqual(connection.replies[-1][0], 1)
        self.assertFalse(self.transport.contexts)
        self.assertFalse(self.lib.buffers)
        self.assertTrue(self.processes[0].closed)

    def test_partial_import_cleanup_failure_keeps_context_without_success_reply(self):
        original = self.import_buffer

        def fail_second(*args):
            if self.imports:
                raise RuntimeError("controlled second import failure")
            return original(*args)

        self.lib.destroy_error = -1
        with patch.object(T.X.Buffer, "import_win32", side_effect=fail_second), \
                self.assertRaisesRegex(RuntimeError, "cleanup"):
            self.request(packet())
        self.assertTrue(self.transport.contexts)
        self.assertFalse(self.processes[0].closed)
        self.assertNotIn(("reply", 0), self.events)

    def test_exited_process_context_is_reaped_before_new_hello(self):
        self.request(packet(session=1))
        self.request(packet(session=2))
        self.transport.active = (42, 1)
        self.processes[0].dead = True
        connection, _ = self.request(packet(session=3))
        self.assertEqual(connection.replies[-1][0], 0)
        self.assertEqual(set(self.transport.contexts), {(42, 2), (42, 3)})
        self.assertTrue(self.processes[0].closed)
        self.assertIsNone(self.transport.active)

    def test_live_process_sessions_are_never_pruned_to_make_room(self):
        self.request(packet(session=1))
        self.request(packet(session=2))
        connection, _ = self.request(packet(session=3))
        self.assertEqual(connection.replies[-1][0], 1)
        self.assertEqual(set(self.transport.contexts), {(42, 1), (42, 2)})
        self.assertFalse(any(p.closed for p in self.processes))

    def test_failed_exit_query_retains_context_and_never_acknowledges_request(self):
        self.request(packet())
        self.processes[0].wait_error = OSError("controlled process wait failure")
        self.events.clear()
        with self.assertRaisesRegex(OSError, "wait"):
            self.request(packet(session=2))
        self.assertEqual(len(self.transport.contexts), 1)
        self.assertEqual(self.events, [("reply", 2)])

    def test_pid_reuse_reimports_resources_from_new_authenticated_process(self):
        self.request(packet())
        first = self.processes[0]
        first.dead = True
        self.request(packet())
        self.assertEqual(len(self.imports), 4)
        self.assertTrue(first.closed)
        self.assertIs(self.transport.contexts[(42, packet()["session"])].process, self.processes[1])

    def test_cleanup_of_inactive_session_preserves_active_history(self):
        self.request(packet(session=1))
        self.request(packet(session=2))
        self.transport.active = (42, 2)
        self.processes[0].dead = True
        self.events.clear()
        self.transport.cleanup_stale()
        self.assertEqual(self.transport.active, (42, 2))
        self.assertNotIn(("reset_history",), self.events)


class Call:
    def __init__(self, result):
        self.result = result
        self.args = []

    def __call__(self, *args):
        self.args.append(args)
        return self.result


class Kernel:
    def __init__(self):
        self.OpenProcess = Call(501)
        self.WaitForSingleObject = Call(258)
        self.CloseHandle = Call(1)


class ProcessLifetimeTests(unittest.TestCase):
    def fake_process(self, kernel):
        with patch.object(T.os, "name", "nt"), \
                patch.object(ctypes, "WinDLL", return_value=kernel, create=True):
            return T._ProcessLifetime(42)

    def test_authenticated_process_handle_uses_only_synchronize_access(self):
        kernel = Kernel()
        process = self.fake_process(kernel)
        self.assertEqual(kernel.OpenProcess.args, [(0x00100000, False, 42)])
        self.assertFalse(process.exited())
        kernel.WaitForSingleObject.result = 0
        self.assertTrue(process.exited())
        self.assertEqual(kernel.WaitForSingleObject.args, [(501, 0), (501, 0)])
        process.close()
        process.close()
        self.assertEqual(kernel.CloseHandle.args, [(501,)])

    def test_failed_process_query_is_not_treated_as_exit(self):
        kernel = Kernel()
        process = self.fake_process(kernel)
        kernel.WaitForSingleObject.result = 0xffffffff
        with patch.object(ctypes, "get_last_error", return_value=5, create=True), \
                self.assertRaises(OSError):
            process.exited()
        self.assertEqual(process.handle, 501)

    def test_failed_close_retains_process_handle_for_retry(self):
        kernel = Kernel()
        process = self.fake_process(kernel)
        kernel.CloseHandle.result = 0
        with patch.object(ctypes, "get_last_error", return_value=5, create=True), \
                self.assertRaises(OSError):
            process.close()
        self.assertEqual(process.handle, 501)
        kernel.CloseHandle.result = 1
        process.close()
        self.assertIsNone(process.handle)

    @unittest.skipUnless(os.name == "nt", "uses Win32 process lifetime handles")
    def test_real_owned_cpu_child_exit_is_detected_through_retained_handle(self):
        child = subprocess.Popen([getattr(sys, "_base_executable", sys.executable), "-c",
                                  "import sys; sys.stdin.buffer.read(1)"],
                                 stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
        process = None
        try:
            process = T._ProcessLifetime(child.pid)
            self.assertFalse(process.exited())
            child.communicate(b"x", timeout=10)
            self.assertEqual(child.returncode, 0)
            self.assertTrue(process.exited())
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=10)
            if process is not None:
                process.close()


if __name__ == "__main__":
    unittest.main()

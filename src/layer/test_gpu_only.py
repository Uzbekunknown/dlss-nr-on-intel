"""CPU-only strict daemon policy: reject host pixels before receipt/processing."""
import os
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "src/layer"), str(ROOT / "src/gpu"), str(ROOT / "src/ref")]
import nr_daemon as D


class Request:
    def __init__(self, header, payload=b"", *, forbid_payload=False):
        self.header, self.payload = header, payload
        self.reads, self.reply, self.forbid_payload = [], None, forbid_payload

    def recv(self, count):
        self.reads.append(count)
        if self.header:
            value, self.header = self.header, b""
            return value
        if self.forbid_payload:
            raise AssertionError("strict daemon read host pixels")
        value, self.payload = self.payload[:count], self.payload[count:]
        return value

    def sendall(self, reply):
        self.reply = reply


class PolicyTests(unittest.TestCase):
    def test_plain_and_masked_frames_stop_after_header(self):
        for magic in (D.MAGIC, D.MAGIC_MASKED):
            request = Request(struct.pack("<4I", magic, 1920, 1080, 44), forbid_payload=True)
            with patch.dict(os.environ, {"NR_GPU_ONLY": "1"}), self.assertRaisesRegex(ValueError, "GPU-only"):
                D.process_connection(request, None, None)
            self.assertEqual(request.reads, [16])
            self.assertIsNone(request.reply)

    def test_direct_process_is_guarded_before_payload_or_backend_access(self):
        request = Request(b"", forbid_payload=True)
        with patch.dict(os.environ, {"NR_GPU_ONLY": "1"}), self.assertRaisesRegex(ValueError, "GPU-only"):
            D.process(request, None, None, 2560, 1440, 44)
        self.assertFalse(request.reads)

    def test_unset_and_zero_preserve_legacy_passthrough(self):
        for value in (None, "0"):
            request = Request(struct.pack("<4I", D.MAGIC, 4, 4, 999), bytes(range(64)))
            with patch.dict(os.environ):
                if value is None:
                    os.environ.pop("NR_GPU_ONLY", None)
                else:
                    os.environ["NR_GPU_ONLY"] = value
                D.process_connection(request, None, SimpleNamespace())
            self.assertEqual(request.reads, [16, 64])
            self.assertEqual(request.reply, bytes(range(64)))

    def test_shared_control_request_is_not_mistaken_for_host_pixels(self):
        calls = []

        class Shared:
            def cleanup_stale(self):
                calls.append("cleanup")

            def reset_history(self):
                calls.append("reset")

            def handle(self, connection, width, height, fmt, read, live, **kwargs):
                calls.append("shared")
                self.control = read(connection, 128)
                return None

        shared = Shared()
        live = SimpleNamespace(refresh=lambda: calls.append("refresh"))
        args = SimpleNamespace(gpu_transport=shared, live=live, dump=None, meter=None)
        request = Request(struct.pack("<4I", D.MAGIC_GPU, 1920, 1080, 44), bytes(128))
        with patch.dict(os.environ, {"NR_GPU_ONLY": "1"}):
            D.process_connection(request, None, args)
        self.assertEqual(request.reads, [16, 128])
        self.assertIn("shared", calls)
        self.assertEqual(len(shared.control), 128)


if __name__ == "__main__":
    unittest.main()

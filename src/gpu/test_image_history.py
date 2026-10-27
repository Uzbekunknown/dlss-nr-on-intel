"""The GPU image path's motion history against the daemon's own `History.follow`.

No weights required. ``--host-only`` checks the constant and the passes' arguments without
Vulkan; the device check holds the bytes on Intel's Windows driver, where the GPU image path
is held to the CPU's (xmxres.image_path_exact).
"""
from __future__ import annotations

import pathlib
import sys
import unittest
from unittest.mock import patch

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src/ref"))
sys.path.insert(0, str(ROOT / "src/layer"))
import nr_daemon
import nr_image
import xmxres as X
import gpu_image_history as G
from gpu_image_stats import GPUImageStats
from test_image_stats import FakeRuntime


class Recorder:
    def __init__(self, path):
        self.calls = []

    def record(self, push, gx, gy=1, gz=1):
        self.calls.append((list(push.buffers), list(push.params), gx))


class HostChecks(unittest.TestCase):
    def test_stale_is_the_daemons(self):
        self.assertEqual(np.float32(G.STALE), np.float32(nr_daemon.STALE))

    def test_two_passes_on_the_pipelines_buffers(self):
        rt = FakeRuntime()
        with patch.object(X, "ImagePipeline", Recorder), \
                patch.object(type(rt.buffer(1)), "zero", lambda self: self, create=True):
            follower = G.GPUImageFollower(rt, frame_size=(30, 40), crop=(2, 28, 4, 36), inner_size=(13, 16))
            with self.assertRaises(ValueError):
                follower.record(6, neural=None, previous=None, active=None, previous_inner=None, inner=None)
            images = [rt.buffer(32 * 26 * 3) for _ in range(3)] + [rt.buffer(16 * 13 * 3) for _ in range(2)]
            follower.record(4096, neural=images[0], previous=images[1], active=images[2],
                            previous_inner=images[3], inner=images[4])
        (active, params, groups), (inner, inner_params, inner_groups) = follower.pipeline.calls
        stale = int(np.float32(nr_daemon.STALE).view(np.uint32))
        self.assertEqual(params[:9], [40, 4, 2, 32, 26, G.ACTIVE, 32, 26, stale])
        self.assertEqual(inner_params[:9], [40, 4, 2, 32, 26, G.INNER, 16, 13, stale])
        self.assertEqual((groups, inner_groups), (-(-32 * 26 // 256), 1))
        self.assertEqual(active[:7], [4096, images[0].address, images[1].address, images[2].address,
                                      follower.full.address, follower.pixels.address, follower.known.address])
        self.assertEqual(inner[:7], [4096, follower.resampled.address, images[3].address, images[4].address,
                                     follower.inner.address, follower.moved.address,
                                     follower.known_inner.address])
        follower.close()
        with self.assertRaises(RuntimeError):
            follower.record(4096, neural=images[0], previous=images[1], active=images[2],
                            previous_inner=images[3], inner=images[4])


def resample(image, size):
    return image.copy() if image.shape[:2] == tuple(size) else nr_daemon.resample(image, size)


class DeviceChecks(unittest.TestCase):
    def test_follow_holds_the_daemons_bytes(self):
        if nr_image.library() is None:
            self.skipTest("native CPU image library is missing")
        rt = X.Runtime()
        if not X.image_path_exact(rt.lib):
            self.skipTest(X.IMAGE_PATH_EXACT_ONLY)
        stats = GPUImageStats(rt)
        rng = np.random.default_rng(271027)
        checks = 0
        for frame, crop, inner in (((23, 37), (0, 23, 0, 37), (23, 37)),
                                   ((23, 37), (2, 21, 3, 34), (11, 17)),
                                   ((90, 160), (10, 80, 0, 160), (35, 64))):
            top, bottom, left, right = crop
            size = (bottom - top, right - left)
            output = rng.random((*size, 3), dtype=np.float32)
            previous = rng.random((*size, 3), dtype=np.float32)
            # This frame: the last one, a little off over most of it (a surface that moved and
            # kept its look) and another surface over a band (past STALE).
            colour = np.clip(previous + rng.normal(0, .02, previous.shape).astype(np.float32), 0, 1)
            colour[::5] = rng.random(colour[::5].shape, dtype=np.float32)
            motion = rng.uniform(-6, 6, (*frame, 2)).astype(np.float32)
            motion[rng.random(frame) < .1] = np.nan
            motion[rng.random(frame) < .05, 0] = np.float32(-80)     # back past the edge
            motion[rng.random(frame) < .02, 1] = np.inf
            # Then nothing moved, the identity read; and, where the network's extent is smaller,
            # motion known only in an active column the network reads none of.
            variants = [motion, np.zeros_like(motion)]
            read = {min((2 * q + 1) * size[1] // (2 * inner[1]), size[1] - 1) for q in range(inner[1])}
            unread = [x for x in range(size[1]) if x not in read]
            if unread:
                lone = np.full_like(motion, np.nan)
                lone[top:bottom, left + unread[0]] = (.5, .25)
                variants.append(lone)
            for motion in variants:
                history = nr_daemon.History()
                history.key, history.output, history.pixels = "shot", output, previous
                history.source = resample(previous, inner)
                history.inner = resample(output, inner)
                now_inner = resample(colour, inner)
                expected = history.follow(now_inner, output, motion[top:bottom, left:right], colour, 10.0)
                buffers = [rt.buffer_from(a) for a in (motion, output, previous, colour, history.source,
                                                       now_inner)]
                follower = G.GPUImageFollower(rt, frame_size=frame, crop=crop, inner_size=inner)
                follower.resampled.upload(history.inner)
                rt.begin()
                follower.record(buffers[0].address, neural=buffers[1], previous=buffers[2],
                                active=buffers[3], previous_inner=buffers[4], inner=buffers[5])
                rt.submit()
                got = (follower.inner.download().reshape(*inner, 3),
                       follower.full.download().reshape(*size, 3),
                       follower.pixels.download().reshape(*size, 3))
                for name, a, b in zip(("inner", "full", "pixels"), got, expected):
                    np.testing.assert_array_equal(a.view(np.uint32), b.view(np.uint32), err_msg=name)
                    checks += 1
                cut = follower.cut(stats, buffers[5], 10.0)
                self.assertAlmostEqual(cut, history.cut, delta=1e-6 * max(1.0, history.cut))
                checks += 1
                for buffer in buffers:
                    buffer.free()
                follower.close()
        stats.close()
        print(f"image history: {checks} exact comparisons with History.follow")

    def test_known_nowhere_drops_the_history(self):
        if nr_image.library() is None:
            self.skipTest("native CPU image library is missing")
        rt = X.Runtime()
        if not X.image_path_exact(rt.lib):
            self.skipTest(X.IMAGE_PATH_EXACT_ONLY)
        stats = GPUImageStats(rt)
        frame, crop, inner = (12, 20), (0, 12, 0, 20), (12, 20)
        motion = rt.buffer_from(np.full((*frame, 2), np.nan, np.float32))
        image = rt.buffer_from(np.full((12, 20, 3), .5, np.float32))
        follower = G.GPUImageFollower(rt, frame_size=frame, crop=crop, inner_size=inner)
        rt.begin()
        follower.record(motion.address, neural=image, previous=image, active=image,
                        previous_inner=image, inner=image)
        rt.submit()
        self.assertIsNone(follower.cut(stats, image, .15))
        for buffer in (motion, image):
            buffer.free()
        follower.close()
        stats.close()


if __name__ == "__main__":
    host_only = "--host-only" in sys.argv
    sys.argv = [v for v in sys.argv if v != "--host-only"]
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(HostChecks)
    if not host_only:
        suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(DeviceChecks))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())

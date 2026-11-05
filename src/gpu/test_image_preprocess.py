#!/usr/bin/env python3
"""GPU SDR preparation versus the existing NumPy/native feature contract.

Default executes the small GPU parity tests. --cpu-only verifies the recorder
contract without creating a Vulkan runtime (safe during other GPU tests).
GPU readbacks occur only in this diagnostic, never in the production wrapper.
"""
from __future__ import annotations

import argparse
import contextlib
import ctypes
import os
import pathlib
import sys
import unittest
from unittest.mock import patch

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src" / "ref"))
sys.path.insert(0, str(ROOT / "src" / "layer"))
import nr_frame
import nr_daemon
import nr_image
import xmxres
from gpu_image_preprocess import (GPUImagePreprocessor, GPUImageResampler,
                                   ImagePreprocessPush, BGRA, HISTORY, SOURCE_RGB32,
                                   OUTPUT_RGB32)

CPU_ONLY = False


@contextlib.contextmanager
def host_native(enabled):
    with patch.dict(os.environ, {"NR_HOST_NATIVE": "1" if enabled else "0"}):
        yield


class FakeBuffer:
    def __init__(self, runtime, nbytes):
        self.id = runtime.next_id
        runtime.next_id += 1
        self.nbytes, self._lib = nbytes, runtime.lib
        self.address = self.id * 4096
        self.uploads = []

    def upload(self, data):
        self.uploads.append(np.array(data, copy=True))

    def free(self):
        self.id = -1


class FakeRuntime:
    def __init__(self):
        self.next_id, self.recorded, self.lib = 1, 0, object()

    def buffer(self, elements, dtype=np.float32, kind=0):
        return FakeBuffer(self, int(elements) * np.dtype(dtype).itemsize)


class FakePipeline:
    def __init__(self, path):
        self.path, self.calls = pathlib.Path(path), []

    def record(self, push, gx, gy=1, gz=1):
        self.calls.append((ImagePreprocessPush.from_buffer_copy(bytes(push)), gx, gy, gz))


class RecorderContract(unittest.TestCase):
    def setUp(self):
        self.patch = patch.object(xmxres, "ImagePipeline", FakePipeline, create=True)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.rt = FakeRuntime()

    def test_push_abi(self):
        self.assertEqual(ctypes.sizeof(ImagePreprocessPush), 104)
        for name, offset in (("raw", 0), ("history", 8), ("features", 16), ("noise", 24),
                             ("source_w", 32), ("inner_h", 60), ("network_w", 64),
                             ("flags", 72), ("ratio_y", 76), ("style", 80), ("ratio_x", 92),
                             ("skin", 96), ("automatic", 100)):
            self.assertEqual(getattr(ImagePreprocessPush, name).offset, offset)

    def test_features_no_host_frame_access_and_cached_noise(self):
        with GPUImagePreprocessor(self.rt, 90, 50, 50, inner_size=(20, 36),
                                  network_size=(128, 128), crop=(5, 45, 9, 81),
                                  frame_index=19) as prep:
            raw = self.rt.buffer(90 * 50 * 4, np.uint8)
            target = prep.allocate_features()
            history = self.rt.buffer(20 * 36 * 3)
            self.assertEqual(len(prep.noise.uploads), 1)
            expected = nr_frame.deterministic_noise(128, 128, 19).astype(np.float16)
            np.testing.assert_array_equal(prep.noise.uploads[0].view(np.uint16), expected.view(np.uint16))
            self.assertIs(prep.record(raw, target, history=history, normalized_style=1 / 128,
                                      local_tone_strength=.75, local_structure_strength=1.5), target)
            prep.record(raw, target)
            self.assertEqual(len(prep.noise.uploads), 1)
            self.assertEqual(self.rt.recorded, 2)
            first, groups, gy, gz = prep.pipeline.calls[0]
            self.assertEqual((first.raw, first.history, first.features, first.noise),
                             (raw.address, history.address, target.address, prep.noise.address))
            self.assertEqual((first.source_w, first.source_h, first.left, first.top,
                              first.active_w, first.active_h, first.inner_w, first.inner_h),
                             (90, 50, 9, 5, 72, 40, 36, 20))
            self.assertEqual(first.flags, BGRA | HISTORY)
            self.assertEqual((first.style, first.tone, first.structure), (1 / 128, .75, 1.5))
            # No automatic mask: its two lanes say so.
            self.assertEqual((first.skin, first.automatic), (-1, -1))
            self.assertEqual((groups, gy, gz), (256, 1, 1))
            self.assertEqual(prep.pipeline.calls[1][0].history, 0)
            self.assertEqual(prep.pipeline.calls[1][0].flags, BGRA)
            # Hogwarts Legacy's profile: the mask on, so the structure lane is 1 and the two
            # lanes carry the skin strength and, following it, the local structure, in half.
            prep.record(raw, target, **nr_frame.PROFILES["hogwarts"])
            third = prep.pipeline.calls[2][0]
            self.assertEqual((third.style, third.tone, third.structure, third.skin, third.automatic),
                             (0.0, 0.5, 1.0, float(np.float16(0.27)), float(np.float16(0.84))))
        self.assertIsNone(prep.noise)
        with self.assertRaises(RuntimeError):
            prep.record(raw, target)

    def test_resample_raw_and_rgb(self):
        for fmt, flags, source_bytes in ((37, OUTPUT_RGB32, 4),
                                         (44, OUTPUT_RGB32 | BGRA, 4),
                                         (None, OUTPUT_RGB32 | SOURCE_RGB32, 12)):
            resample = GPUImageResampler(self.rt, (55, 93), (19, 31), vk_format=fmt,
                                         crop=(2, 53, 3, 90))
            source = self.rt.buffer(55 * 93 * source_bytes, np.uint8)
            target = self.rt.buffer(19 * 31 * 3)
            resample.record(source, target)
            pc, groups, _, _ = resample.pipeline.calls[0]
            self.assertEqual(pc.flags, flags)
            self.assertEqual((pc.inner_w, pc.inner_h, pc.network_w, pc.network_h), (31, 19, 31, 19))
            self.assertEqual((pc.history, pc.noise), (0, 0))
            self.assertEqual(groups, 10)

    def test_invalid_regions_formats_and_aliases(self):
        for kwargs in ({"vk_format": 97}, {"width": 0}, {"width": 3.0},
                       {"crop": (0, 9, 0, 10)}, {"inner_size": (16, 9)},
                       {"network_size": (5, 5)}, {"crop": (-1, 8, 0, 8)}):
            values = dict(width=8, height=8, vk_format=37)
            values.update(kwargs)
            with self.assertRaises(ValueError):
                GPUImagePreprocessor(self.rt, **values)
        with GPUImagePreprocessor(self.rt, 8, 8, 37) as prep:
            raw, target = self.rt.buffer(256, np.uint8), prep.allocate_features()
            for kwargs in ({"control_mask": object()},
                           {"automatic_mask": nr_frame.AutomaticMask(np.nan, -1.0)},
                           {"automatic_mask": nr_frame.AutomaticMask(0.27, 70000.0)},
                           {"local_tone_strength": np.nan}, {"normalized_style": np.inf},
                           {"local_structure_strength": 65505},
                           {"history": self.rt.buffer(1)}, {"history": target}):
                with self.assertRaises(ValueError):
                    prep.record(raw, target, **kwargs)
            with self.assertRaises(ValueError):
                prep.record(raw, self.rt.buffer(1))
            with self.assertRaises(ValueError):
                prep.record(target, target)
            target._lib = object()
            with self.assertRaises(ValueError):
                prep.record(raw, target)
            self.assertEqual(prep.pipeline.calls, [])


class GPUParity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if CPU_ONLY:
            raise unittest.SkipTest("CPU-only recorder checks requested")
        cls.rt = xmxres.Runtime()
        if not xmxres.image_path_exact(cls.rt.lib):
            raise unittest.SkipTest(xmxres.IMAGE_PATH_EXACT_ONLY)

    def compare_features(self, *, shape, inner, field, fmt, crop=None, frame_index=0,
                         history=False, controls=None):
        rng = np.random.default_rng(638 + frame_index)
        raw = rng.integers(0, 256, (*shape, 4), dtype=np.uint8)
        # Exercise exact zeros, one, chromatic edges and nontrivial alpha.
        raw[:2, :2, :3] = np.array([0, 127, 255], dtype=np.uint8)
        controls = controls or dict(normalized_style=0.0, local_tone_strength=1.0,
                                   local_structure_strength=1.0)
        with host_native(False):
            colour = nr_daemon.decode(raw.tobytes(), shape[1], shape[0], fmt)
            top, bottom, left, right = crop or (0, shape[0], 0, shape[1])
            colour = nr_daemon.resample(colour[top:bottom, left:right], inner)
            old = rng.random((*inner, 3), dtype=np.float32) if history else None
            if old is not None:
                old[0, 0] = [0.5, np.nextafter(np.float32(.5), np.float32(1)), 0]
            geometry = nr_frame.NetworkGeometry(inner[1], inner[0], field[1], field[0])
            expected = nr_frame.build_features(colour, geometry=geometry, history=old,
                                                frame_index=frame_index, **controls).astype(np.float16)
        if nr_image.library() is not None:
            with host_native(True):
                native = nr_frame.build_features(colour, geometry=geometry, history=old,
                                                 frame_index=frame_index, **controls).astype(np.float16)
                np.testing.assert_array_equal(native.view(np.uint16), expected.view(np.uint16))
        raw_buffer = self.rt.buffer(raw.size, np.uint8, kind=xmxres.DEVICE).upload(raw)
        history_buffer = (self.rt.buffer(old.size, kind=xmxres.DEVICE).upload(old)
                          if old is not None else None)
        prep = GPUImagePreprocessor(self.rt, shape[1], shape[0], fmt, inner_size=inner,
                                    network_size=field, crop=crop, frame_index=frame_index)
        target = prep.allocate_features()
        try:
            self.assertFalse(raw_buffer.mapped)
            self.assertFalse(target.mapped)
            if history_buffer is not None:
                self.assertFalse(history_buffer.mapped)
            for turn in range(2):
                self.rt.begin()
                prep.record(raw_buffer, target, history=history_buffer, **controls)
                self.rt.submit()
                got = target.download(np.float16).reshape(expected.shape)
                np.testing.assert_array_equal(got.view(np.uint16), expected.view(np.uint16),
                                              err_msg=f"shape={shape} inner={inner} field={field} fmt={fmt}")
                # Replay new pixels through the same pipeline/buffer without stale input.
                raw ^= np.uint8(255)
                raw_buffer.upload(raw)
                with host_native(False):
                    colour = nr_daemon.decode(raw.tobytes(), shape[1], shape[0], fmt)
                    colour = nr_daemon.resample(colour[top:bottom, left:right], inner)
                    expected = nr_frame.build_features(colour, geometry=geometry, history=old,
                                                       frame_index=frame_index, **controls).astype(np.float16)
        finally:
            prep.close()
            target.free()
            raw_buffer.free()
            if history_buffer is not None:
                history_buffer.free()

    def test_feature_modes_exact_half_bits(self):
        cases = [
            dict(shape=(64, 80), inner=(64, 80), field=(128, 128), fmt=37),
            dict(shape=(54, 82), inner=(25, 36), field=(128, 128), fmt=44,
                 crop=(2, 52, 5, 77), history=True, frame_index=7),
            dict(shape=(79, 121), inner=(31, 48), field=(128, 128), fmt=43,
                 history=True, controls=dict(normalized_style=2 / 128, local_tone_strength=.33333,
                                             local_structure_strength=1.9999)),
            dict(shape=(61, 91), inner=(61, 37), field=(128, 128), fmt=50),
            dict(shape=(63, 93), inner=(29, 93), field=(128, 128), fmt=37),
            dict(shape=(1, 1), inner=(1, 1), field=(128, 128), fmt=44, history=True),
            dict(shape=(256, 320), inner=(128, 160), field=(128, 192), fmt=37, history=True),
            dict(shape=(189, 207), inner=(63, 69), field=(128, 128), fmt=44, history=True),
            dict(shape=(180, 240), inner=(36, 48), field=(128, 128), fmt=37),
            # The automatic mask on: Hogwarts Legacy's profile, and a mask whose skin lane
            # follows the local structure while its other lane does not.
            dict(shape=(79, 121), inner=(31, 48), field=(128, 128), fmt=44, history=True,
                 controls=dict(nr_frame.PROFILES["hogwarts"])),
            dict(shape=(64, 80), inner=(64, 80), field=(128, 128), fmt=37,
                 controls=dict(normalized_style=1 / 128, local_tone_strength=1.0,
                               local_structure_strength=1.5,
                               automatic_mask=nr_frame.AutomaticMask(-1.0, 0.6))),
        ]
        for case in cases:
            with self.subTest(case=case):
                self.compare_features(**case)

    def test_rgb_resampling_exact_float_bits(self):
        rng = np.random.default_rng(903)
        for shape, target_size, fmt, crop in (
                ((80, 96), (40, 48), 44, None),
                ((79, 101), (33, 41), 37, (2, 77, 3, 98)),
                ((189, 207), (63, 69), 44, None),
                ((180, 240), (36, 48), 37, None),
                ((83, 97), (83, 43), None, None),
                ((77, 95), (37, 95), None, None),
                ((33, 41), (77, 95), None, None)):
            source = (rng.integers(0, 256, (*shape, 4), dtype=np.uint8) if fmt is not None
                      else rng.random((*shape, 3), dtype=np.float32))
            with host_native(False):
                colour = nr_daemon.decode(source.tobytes(), shape[1], shape[0], fmt) if fmt is not None else source
                top, bottom, left, right = crop or (0, shape[0], 0, shape[1])
                expected = nr_daemon.resample(colour[top:bottom, left:right], target_size)
            resampler = GPUImageResampler(self.rt, shape, target_size, crop=crop, vk_format=fmt)
            src = self.rt.buffer(source.size, source.dtype, kind=xmxres.DEVICE).upload(source)
            dst = self.rt.buffer(np.prod(target_size) * 3, kind=xmxres.DEVICE)
            try:
                self.rt.begin()
                resampler.record(src, dst)
                self.rt.submit()
                got = dst.download().reshape(expected.shape)
                np.testing.assert_array_equal(got.view(np.uint32), expected.view(np.uint32),
                                              err_msg=f"resample {shape}->{target_size} fmt={fmt}")
            finally:
                src.free()
                dst.free()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpu-only", action="store_true")
    args, remainder = parser.parse_known_args()
    CPU_ONLY = args.cpu_only
    unittest.main(argv=[sys.argv[0], *remainder], verbosity=2)

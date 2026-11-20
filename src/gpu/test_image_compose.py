"""GPU SDR composition against native CPU bytes and temporal history.

No weights required. ``--host-only`` runs validation/API checks without Vulkan;
the default additionally exercises the real shader on the resident device.
"""
import ctypes
import pathlib
import sys
import unittest
from unittest import mock

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "ref"))
import gpu_image_compose as G
import nr_frame
import nr_image
import xmxres


class FakeBuffer:
    serial = 0

    def __init__(self, count, dtype=np.float32, kind=None):
        type(self).serial += 1
        self.id = type(self).serial
        self.address = self.id * 4096
        self.nbytes = count * np.dtype(dtype).itemsize
        self.data = bytearray(self.nbytes)
        self.uploads = []

    def upload(self, array, offset=0):
        raw = np.ascontiguousarray(array).tobytes()
        self.uploads.append((offset, len(raw)))
        self.data[offset:offset + len(raw)] = raw
        return self

    def free(self):
        self.id = -1


class FakeRuntime:
    buffer = staticmethod(FakeBuffer)


class HostChecks(unittest.TestCase):
    def composer(self):
        patch = mock.patch.object(xmxres, "ImagePipeline")
        pipeline = patch.start()
        self.addCleanup(patch.stop)
        composer = G.GPUImageComposer(FakeRuntime(), frame_size=(12, 17),
                                      head_size=(6, 9), vk_format=37)
        self.addCleanup(composer.close)
        return composer, pipeline.return_value

    def test_identical_controls_skip_transfer_but_changes_update(self):
        composer, _ = self.composer()
        self.assertEqual(composer.settings.uploads, [(0, composer.settings.nbytes)])
        initial_lut = bytes(composer.settings.data[64:])
        for _ in range(3):
            composer.configure()
        self.assertEqual(len(composer.settings.uploads), 1)
        controls = {}
        for name, value, field in (("intensity", 1.5, 0), ("history_confidence", .7, 1),
                                   ("history_hold", .8, 3), ("history_release", 24, 5)):
            controls[name] = value
            before = len(composer.settings.uploads)
            composer.configure(**controls)
            self.assertEqual(len(composer.settings.uploads), before + 1)
            self.assertEqual(composer.settings.uploads[-1], (0, 64))
            self.assertEqual(bytes(composer.settings.data[64:]), initial_lut)
            config = np.frombuffer(composer.settings.data, np.float32)
            expected = nr_frame.release_slope(value) if name == "history_release" else np.float32(value)
            self.assertEqual(config[field], expected)
            composer.configure(**controls)
            self.assertEqual(len(composer.settings.uploads), before + 1)
        grade = np.float32([.95, .15, 1.05])
        composer.configure(**controls, grade=grade)
        self.assertEqual(composer.settings.uploads[-1], (0, 64))
        config = np.frombuffer(composer.settings.data, np.float32)
        np.testing.assert_array_equal(config[6:9], grade)
        before = len(composer.settings.uploads)
        composer.configure(**controls, grade=grade)
        self.assertEqual(len(composer.settings.uploads), before)
        grade[0] = .85
        composer.configure(**controls, grade=grade)
        self.assertEqual(len(composer.settings.uploads), before + 1)
        self.assertEqual(config[6], grade[0])

    def test_blend_updates_full_lut_then_caches_it(self):
        composer, _ = self.composer()
        blend = .2 if nr_frame.BLEND_SCALE != .2 else .3
        composer.configure(blend_scale=blend)
        self.assertEqual(composer.settings.uploads[-1], (0, composer.settings.nbytes))
        config = np.frombuffer(composer.settings.data, np.float32)
        np.testing.assert_array_equal(config[272:], nr_frame.gate_table(blend))
        before = len(composer.settings.uploads)
        composer.configure(blend_scale=blend)
        self.assertEqual(len(composer.settings.uploads), before)
        composer.configure(blend_scale=blend, intensity=1.25)
        self.assertEqual(composer.settings.uploads[-1], (0, 64))
        np.testing.assert_array_equal(config[272:], nr_frame.gate_table(blend))
        composer.configure()
        self.assertEqual(composer.settings.uploads[-1], (0, composer.settings.nbytes))
        np.testing.assert_array_equal(config[272:], nr_frame.gate_table(nr_frame.BLEND_SCALE))

    def test_flags_are_independent_of_identical_setting_bytes(self):
        composer, pipeline = self.composer()
        raw = FakeBuffer(12 * 17, np.uint32)
        head = FakeBuffer(6 * 9 * 4)
        composer.record(raw, head)
        self.assertEqual(pipeline.record.call_args.args[0].params[10], G.KEEP | G.COMPOSED)
        # without the float32 composition (the change metric off) the flag and its buffer go
        composer.record(raw, head, composed=False)
        push = pipeline.record.call_args.args[0]
        self.assertEqual(push.params[10], G.KEEP)
        self.assertEqual(push.buffers[6], 0)
        # The identity grade has the same three scalar bits as no grade, but
        # enables grading and pre-grade half truncation through its push flag.
        composer.configure(grade=(1, 0, 1), keep_neural=False)
        self.assertEqual(len(composer.settings.uploads), 1)
        composer.record(raw, head)
        self.assertEqual(pipeline.record.call_args.args[0].params[10], G.GRADE | G.COMPOSED)
        composer.configure(keep_neural=False)
        self.assertEqual(len(composer.settings.uploads), 1)
        composer.record(raw, head)
        self.assertEqual(pipeline.record.call_args.args[0].params[10], G.COMPOSED)
        composer.configure(keep_neural=True, history_confidence=2)
        # Confidence 2 is clipped to the already-stored 1 before comparison.
        self.assertEqual(len(composer.settings.uploads), 1)
        composer.record(raw, head)
        self.assertEqual(pipeline.record.call_args.args[0].params[10], G.KEEP | G.COMPOSED)

    def test_failed_transfer_is_retried_and_signed_zero_is_not_lost(self):
        composer, _ = self.composer()
        with mock.patch.object(composer.settings, "upload", side_effect=RuntimeError("transfer failed")):
            with self.assertRaisesRegex(RuntimeError, "transfer failed"):
                composer.configure(intensity=1.5)
        composer.configure(intensity=1.5)
        self.assertEqual(composer.settings.uploads[-1], (0, 64))
        self.assertEqual(np.frombuffer(composer.settings.data, np.float32)[0], np.float32(1.5))
        composer.configure(intensity=-0.)
        before = len(composer.settings.uploads)
        self.assertTrue(np.signbit(np.frombuffer(composer.settings.data, np.float32)[0]))
        composer.configure(intensity=0.)
        self.assertEqual(len(composer.settings.uploads), before + 1)
        self.assertFalse(np.signbit(np.frombuffer(composer.settings.data, np.float32)[0]))
        blend = .2 if nr_frame.BLEND_SCALE != .2 else .3
        with mock.patch.object(composer.settings, "upload", side_effect=RuntimeError("LUT transfer failed")):
            with self.assertRaisesRegex(RuntimeError, "LUT transfer failed"):
                composer.configure(blend_scale=blend)
        composer.configure(blend_scale=blend)
        self.assertEqual(composer.settings.uploads[-1], (0, composer.settings.nbytes))
        composer.close()
        with self.assertRaisesRegex(RuntimeError, "closed"):
            composer.configure(blend_scale=blend)

    def test_fallback_and_geometry(self):
        self.assertIsNone(G.fallback_reason(vk_format=37))
        self.assertIn("SDR", G.fallback_reason(vk_format=64))
        # Detail and colour strength are composed here since 2026-11-21; only a strength that is
        # not a number is still a reason.
        self.assertIsNone(G.fallback_reason(vk_format=44, detail_strength=.7, colour_strength=1.3))
        self.assertIn("finite", G.fallback_reason(vk_format=44, detail_strength=float("nan")))
        self.assertIn("masks", G.fallback_reason(vk_format=44, interface=True))
        with mock.patch.object(xmxres, "ImagePipeline"):
            with self.assertRaises(ValueError):
                G.GPUImageComposer(FakeRuntime(), frame_size=(12, 17), head_size=(13, 17), vk_format=37)
            with self.assertRaises(ValueError):
                G.GPUImageComposer(FakeRuntime(), frame_size=(12, 17), head_size=(6, 9),
                                   vk_format=37, crop=(-1, 12, 0, 17))

    def test_push_controls_and_lifetime(self):
        with mock.patch.object(xmxres, "ImagePipeline") as pipeline:
            composer = G.GPUImageComposer(FakeRuntime(), frame_size=(12, 17), head_size=(6, 9),
                                          vk_format=44, head_row_width=16,
                                          crop=(0, 12, 0, 17))
            config = np.frombuffer(composer.settings.data, np.float32)
            np.testing.assert_array_equal(config[16:272], np.arange(256, dtype=np.float32) / np.float32(255))
            np.testing.assert_array_equal(config[272:], nr_frame.gate_table(nr_frame.BLEND_SCALE))
            raw, head, history, previous = (FakeBuffer(12 * 17, np.uint32), FakeBuffer(6 * 16 * 4),
                                            FakeBuffer(12 * 17 * 3), FakeBuffer(12 * 17 * 3))
            composer.configure(intensity=1.66, history_confidence=.7, history_hold=.8,
                               history_release=24, grade=nr_frame.grade_for(**nr_frame.PROFILES["natural"]))
            composer.record(raw, head, history=history, previous=previous)
            push = pipeline.return_value.record.call_args.args[0]
            self.assertEqual(ctypes.sizeof(push), 128)
            self.assertEqual(push.params[10],
                             G.BGRA | G.HISTORY | G.PREVIOUS | G.KEEP | G.GRADE | G.COMPOSED)
            self.assertEqual(list(push.buffers)[:4], [v.address for v in (raw, head, history, previous)])
            with self.assertRaises(ValueError):
                composer.record(raw, head, previous=previous)
            with self.assertRaises(ValueError):
                composer.record(raw, head, history=composer.neural)
            composer.close()
            with self.assertRaises(RuntimeError):
                composer.record(raw, head)

    def test_detail_passes_only_when_a_strength_moves(self):
        """At strength 1 one dispatch, as before; away from it the composition hands on its change
        and two more passes follow, the Gaussian's weights uploaded once for the radius."""
        with mock.patch.object(xmxres, "ImagePipeline") as pipeline:
            composer = G.GPUImageComposer(FakeRuntime(), frame_size=(12, 17), head_size=(6, 9),
                                          vk_format=44, crop=(1, 11, 2, 15))
            self.addCleanup(composer.close)
            raw, head = FakeBuffer(12 * 17, np.uint32), FakeBuffer(6 * 9 * 4)
            record = pipeline.return_value.record
            composer.record(raw, head, composed=False)
            self.assertEqual(record.call_count, 1)
            self.assertIsNone(composer.weights)
            composer.configure(detail_strength=1.4, colour_strength=.6)
            weights = np.frombuffer(composer.weights.data, np.float32)[:25]
            np.testing.assert_array_equal(weights, nr_frame.composition_mod.gaussian_kernel(4.0))
            uploads = len(composer.weights.uploads)
            composer.configure(detail_strength=.3, colour_strength=1.0)
            self.assertEqual(len(composer.weights.uploads), uploads, "the same radius uploaded again")
            record.reset_mock()
            encoded = composer.record(raw, head, composed=False)
            self.assertEqual(record.call_count, 3)
            pushes = [call.args[0] for call in record.call_args_list]
            self.assertTrue(pushes[0].params[10] & G.DETAIL_CHANGE)
            self.assertFalse(pushes[0].params[10] & G.COMPOSED)
            self.assertEqual(pushes[0].buffers[6], composer.composed.address)
            for push, mode in zip(pushes[1:], (G.DETAIL_ROWS, G.DETAIL_COLUMNS)):
                self.assertEqual(push.params[10], mode | G.BGRA)
                self.assertEqual(list(push.params[:6]), [17, 12, 2, 1, 13, 10])
                self.assertEqual((push.params[13], push.params[14]), (25, 12))
                self.assertEqual(np.uint32(push.params[12]).view(np.float32), np.float32(.3))
                self.assertEqual(np.uint32(push.params[11]).view(np.float32), np.float32(1.0))
                self.assertEqual([push.buffers[i] for i in (0, 2, 4, 5, 6)],
                                 [raw.address, composer.rows.address, encoded.address,
                                  composer.weights.address, composer.composed.address])
            # Back at 1, nothing but the composition again.
            composer.configure()
            record.reset_mock()
            composer.record(raw, head, composed=False)
            self.assertEqual(record.call_count, 1)
            self.assertFalse(record.call_args.args[0].params[10] & G.DETAIL_CHANGE)


class DeviceChecks(unittest.TestCase):
    def test_exact_composition_and_temporal_sequence(self):
        if nr_image.library() is None:
            self.skipTest("native CPU image library is missing")
        rt = xmxres.Runtime()
        if not xmxres.image_path_exact(rt.lib):
            self.skipTest(xmxres.IMAGE_PATH_EXACT_ONLY)
        rng = np.random.default_rng(110013)
        checks = 0
        for vk_format in (37, 43, 44, 50):
            for crop, head_size, row_width, channels in (
                    ((0, 23, 0, 37), (23, 37), 37, 4),
                    ((2, 21, 3, 34), (8, 13), 16, 16),
                    ((1, 22, 2, 35), (9, 17), 20, 4)):
                composer = G.GPUImageComposer(rt, frame_size=(23, 37), head_size=head_size,
                                              vk_format=vk_format, crop=crop,
                                              head_row_width=row_width, head_channels=channels)
                top, bottom, left, right = crop
                active_size = (bottom - top, right - left)
                raw = rng.integers(0, 256, (23, 37, 4), dtype=np.uint8)
                source = nr_image.decode8(raw.tobytes(), 37, 23, composer.bgra)
                colour = source[top:bottom, left:right]
                padded = np.full((*head_size[:1], row_width, channels), 2000, np.float32)
                head = rng.normal(0, .65, (*head_size, 4)).astype(np.float32)
                # FP16 ties/subnormals/signs plus wide logits in the gate's LUT.
                edge = np.array([0, -0., 2 ** -25, -2 ** -25, 2 ** -24,
                                 1 + 2 ** -11, 1 + 3 * 2 ** -11, -.99975586], np.float32)
                head.reshape(-1, 4)[:len(edge), :3] = edge[:, None]
                head[..., 3] = rng.uniform(-25, 25, head_size).astype(np.float32)
                padded[:, :head_size[1], :4] = head
                raw_device = rt.buffer_from(raw.view(np.uint32), np.uint32)
                head_device = rt.buffer_from(padded)
                previous = colour.copy()
                # Max channel movement covers no move, half hold ramp, release
                # boundary and a large change at the same scene geometry.
                previous[::4, ::3] = np.clip(previous[::4, ::3] - np.float32(2 / 255), 0, 1)
                previous[1::4, ::3] = np.clip(previous[1::4, ::3] + np.float32(24 / 255), 0, 1)
                previous[2::4, ::3] = 1 - previous[2::4, ::3]
                history = nr_frame.truncate_half(rng.random((*active_size, 3), dtype=np.float32))
                previous_device = rt.buffer_from(previous)
                history_device = rt.buffer_from(history)
                for temporal, keep, intensity, profile in (
                        (False, True, 0, "standard"), (False, False, 1, "standard"),
                        (False, True, 1.66, "natural"), (True, True, .5, "standard"),
                        (True, True, 1, "natural"), (True, True, 1.66, "cinematic")):
                    grade = nr_frame.grade_for(**nr_frame.PROFILES[profile])
                    controls = dict(intensity=intensity, history_confidence=.7,
                                    history_hold=.8, history_release=24, grade=grade)
                    composer.configure(**controls, keep_neural=keep)
                    encoded = raw.copy()
                    neural = np.empty((*active_size, 3), np.float32) if keep else None
                    expected = nr_frame.compose_encode(
                        head, colour, encoded, top=top, left=left, bgra=composer.bgra,
                        history=history if temporal else None,
                        history_previous=previous if temporal else None, neural=neural, **controls)
                    self.assertIsNotNone(expected, "native composition did not run")
                    rt.begin()
                    composer.record(raw_device, head_device,
                                    history=history_device if temporal else None,
                                    previous=previous_device if temporal else None)
                    rt.submit()
                    result = composer.encoded.download(np.uint8).reshape(raw.shape)
                    label = f"fmt={vk_format},crop={crop},head={head_size},temporal={temporal},style={profile}"
                    np.testing.assert_array_equal(result, encoded, err_msg=label + " wire bytes")
                    np.testing.assert_array_equal(composer.composed.download().reshape(expected.shape),
                                                  expected, err_msg=label + " composition")
                    if keep:
                        np.testing.assert_array_equal(composer.neural.download().reshape(neural.shape),
                                                      neural, err_msg=label + " pre-grade history")
                    # The same frame without the float32 composition (NR_CHANGE_METRIC=0): the same
                    # wire bytes and history, and the composition's buffer left as it was.
                    sentinel = np.full(expected.size, -7.0, np.float32)
                    composer.composed.upload(sentinel)
                    composer.encoded.upload(np.zeros(raw.size, np.uint8))
                    if keep:
                        composer.neural.upload(np.full(neural.size, -3.0, np.float32))
                    rt.begin()
                    composer.record(raw_device, head_device,
                                    history=history_device if temporal else None,
                                    previous=previous_device if temporal else None, composed=False)
                    rt.submit()
                    np.testing.assert_array_equal(composer.encoded.download(np.uint8).reshape(raw.shape),
                                                  encoded, err_msg=label + " wire bytes, no composition")
                    np.testing.assert_array_equal(composer.composed.download(), sentinel,
                                                  err_msg=label + " composition written unasked")
                    if keep:
                        np.testing.assert_array_equal(composer.neural.download().reshape(neural.shape),
                                                      neural, err_msg=label + " history, no composition")
                    checks += 1
                for buf in (raw_device, head_device, previous_device, history_device):
                    buf.free()
                composer.close()

        # An actual continued temporal sequence; grading/intensity must never
        # enter the history carried to the next frame. A cut is an explicit
        # history omission, equivalent to the daemon's reset decision.
        raw = rng.integers(0, 256, (31, 47, 4), dtype=np.uint8)
        composer = G.GPUImageComposer(rt, frame_size=(31, 47), head_size=(13, 19), vk_format=44)
        head = rng.normal(0, .3, (13, 19, 4)).astype(np.float32)
        head_device = rt.buffer_from(head)
        raw_device = rt.buffer(raw.size, np.uint8)
        history_device = rt.buffer(31 * 47 * 3)
        previous_device = rt.buffer(31 * 47 * 3)
        history, previous = None, None
        for frame in range(6):
            if frame == 2:
                raw[::2, ::3, :3] = np.minimum(raw[::2, ::3, :3].astype(np.uint16) + 1, 255).astype(np.uint8)
            if frame == 4:
                raw = 255 - raw
                history, previous = None, None
            colour = nr_image.decode8(raw.tobytes(), 47, 31, True)
            raw_device.upload(raw)
            controls = dict(intensity=1.66, history_confidence=1, history_hold=1,
                            history_release=24, grade=nr_frame.grade_for(**nr_frame.PROFILES["natural"]))
            composer.configure(**controls)
            encoded, neural = raw.copy(), np.empty_like(colour)
            expected = nr_frame.compose_encode(head, colour, encoded, bgra=True, history=history,
                                              history_previous=previous, neural=neural, **controls)
            rt.begin()
            composer.record(raw_device, head_device, history=history_device if history is not None else None,
                            previous=previous_device if previous is not None else None)
            rt.submit()
            np.testing.assert_array_equal(composer.encoded.download(np.uint8).reshape(raw.shape), encoded)
            np.testing.assert_array_equal(composer.neural.download().reshape(neural.shape), neural)
            # Copy device next history, never round-trip it via the host.
            rt.begin()
            rt.copy(composer.neural, history_device, neural.nbytes)
            rt.submit()
            previous_device.upload(colour)
            history, previous = neural, colour
            checks += 1
        for buf in (raw_device, head_device, previous_device, history_device):
            buf.free()
        composer.close()
        print(f"image composition: {checks} exact CPU/device byte/history comparisons")

    def test_detail_and_colour_strength(self):
        """`compose_detail` on the device, against the CPU's: the composition's change, a Gaussian
        of 25 taps by rows and then by columns with the active rectangle's edges repeated, and the
        strengths' sum, each in NumPy's float32 order - the CPU path's blur where OpenCV is not
        installed, as in the release. Until 2026-11-21 the GPU path refused these strengths, and a
        DirectX 12 game, which has no CPU path, got no NR while either was moved."""
        if nr_image.library() is None:
            self.skipTest("native CPU image library is missing")
        rt = xmxres.Runtime()
        if not xmxres.image_path_exact(rt.lib):
            self.skipTest(xmxres.IMAGE_PATH_EXACT_ONLY)
        # MLX-DLSS's `blur` takes OpenCV's sepFilter2D where cv2 imports, and that sums the taps in
        # its own order. The release's Python has no OpenCV, so the CPU side here is NumPy's blur
        # on every machine: with cv2 hidden, `import cv2` raises ImportError inside `blur`.
        hidden = mock.patch.dict(sys.modules, {"cv2": None})
        hidden.start()
        self.addCleanup(hidden.stop)
        rng = np.random.default_rng(20261121)
        checks = anchored = 0
        for vk_format in (37, 44):
            for crop, head_size, row_width, channels in (
                    ((0, 23, 0, 37), (23, 37), 37, 4),
                    ((2, 21, 3, 34), (8, 13), 16, 16),
                    ((1, 40, 2, 70), (39, 68), 68, 4)):
                height, width = (23, 37) if crop[1] <= 23 else (41, 71)
                composer = G.GPUImageComposer(rt, frame_size=(height, width), head_size=head_size,
                                              vk_format=vk_format, crop=crop,
                                              head_row_width=row_width, head_channels=channels)
                top, bottom, left, right = crop
                active_size = (bottom - top, right - left)
                raw = rng.integers(0, 256, (height, width, 4), dtype=np.uint8)
                colour = nr_image.decode8(raw.tobytes(), width, height, composer.bgra)[top:bottom, left:right]
                head = rng.normal(0, .65, (*head_size, 4)).astype(np.float32)
                head[..., 3] = rng.uniform(-25, 25, head_size).astype(np.float32)
                padded = np.full((head_size[0], row_width, channels), 2000, np.float32)
                padded[:, :head_size[1], :4] = head
                raw_device = rt.buffer_from(raw.view(np.uint32), np.uint32)
                head_device = rt.buffer_from(padded)
                history = nr_frame.truncate_half(rng.random((*active_size, 3), dtype=np.float32))
                previous = np.clip(colour + np.float32(3 / 255), 0, 1)
                history_device, previous_device = rt.buffer_from(history), rt.buffer_from(previous)
                for detail, colour_strength, intensity, profile, temporal in (
                        (0.0, 1.0, 1, "standard", False), (1.8, 0.4, 1.66, "natural", False),
                        (1.0, 2.0, .5, "cinematic", True), (0.5, 0.5, 1, "standard", True),
                        (1.25, 1.0, 1, "natural", False)):
                    grade = nr_frame.grade_for(**nr_frame.PROFILES[profile])
                    controls = dict(intensity=intensity, history_confidence=.7, history_hold=.8,
                                    history_release=24, grade=grade)
                    composer.configure(**controls, detail_strength=detail, colour_strength=colour_strength)
                    # The composition before the strengths, which the test above holds the device
                    # to exactly, then the CPU's compose_detail over it and the daemon's encode.
                    neural = np.empty((*active_size, 3), np.float32)
                    before = nr_frame.compose_encode(
                        head, colour, raw.copy(), top=top, left=left, bgra=composer.bgra,
                        history=history if temporal else None,
                        history_previous=previous if temporal else None, neural=neural, **controls)
                    expected = nr_frame.composition_mod.compose_detail(
                        colour, before, detail_strength=detail, colour_strength=colour_strength,
                        radius=G.DETAIL_RADIUS)
                    quantised = np.multiply(expected, np.float32(255.0), dtype=np.float32)
                    np.add(quantised, np.float32(0.5), out=quantised)
                    np.clip(quantised, 0.0, 255.0, out=quantised)
                    encoded = raw.copy()
                    window = encoded[top:bottom, left:right]
                    (window[..., 2::-1] if composer.bgra else window[..., :3])[:] = quantised.astype(np.uint8)
                    rt.begin()
                    composer.record(raw_device, head_device,
                                    history=history_device if temporal else None,
                                    previous=previous_device if temporal else None, composed=False)
                    rt.submit()
                    label = (f"fmt={vk_format},crop={crop},head={head_size},detail={detail},"
                             f"colour={colour_strength},temporal={temporal}")
                    np.testing.assert_array_equal(composer.encoded.download(np.uint8).reshape(raw.shape),
                                                  encoded, err_msg=label + " wire bytes")
                    np.testing.assert_array_equal(composer.composed.download().reshape(expected.shape),
                                                  expected, err_msg=label + " composition")
                    # The history the next frame runs on is the prediction before the strengths.
                    np.testing.assert_array_equal(composer.neural.download().reshape(neural.shape),
                                                  neural, err_msg=label + " pre-grade history")
                    if head_size == active_size:
                        # And that is the daemon's CPU path: `nr_frame.compose` with the strengths.
                        cpu = nr_frame.compose(
                            head, colour, detail_strength=detail, colour_strength=colour_strength,
                            history=history if temporal else None,
                            history_previous=previous if temporal else None, **controls)
                        np.testing.assert_array_equal(cpu, expected, err_msg=label + " CPU path")
                        anchored += 1
                    checks += 1
                for buf in (raw_device, head_device, history_device, previous_device):
                    buf.free()
                composer.close()
        print(f"detail and colour strength: {checks} exact device/CPU comparisons, "
              f"{anchored} of them against nr_frame.compose itself")


if __name__ == "__main__":
    host_only = "--host-only" in sys.argv
    sys.argv = [v for v in sys.argv if v != "--host-only"]
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(HostChecks)
    if not host_only:
        suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(DeviceChecks))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())

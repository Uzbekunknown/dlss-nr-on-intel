#!/usr/bin/env python3
"""The raw host pipeline against decoded input, including multi-frame daemon state."""
import contextlib
import hashlib
import io
import os
import pathlib
import struct
import sys
from types import SimpleNamespace

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'src/layer'), str(ROOT / 'src/ref')]
import nr_daemon as D
import nr_frame as F
import nr_image
from test_native_image import numpy_only
from test_bgra_compose import decoded, exact


def resample_checks():
    rng = np.random.default_rng(98)
    cases = 0
    for bgra in (False, True):
        for layout in ('crop', 'reverse', 'strided'):
            wire = rng.integers(0, 256, (128, 192, 4), dtype=np.uint8)
            pixels = wire[4:124, 8:184]
            if layout == 'reverse':
                pixels = pixels[::-1, ::-1]
            elif layout == 'strided':
                pixels = pixels[::2, ::2]
            h, w = pixels.shape[:2]
            rgb = decoded(pixels, bgra)
            for size in ((h, w), (h // 2, w // 2), (h // 3, w // 4),
                         (round(h * .55), round(w * .55)), (h, w // 2),
                         (h // 2, w), (h * 2, w * 2), (1, 1)):
                with numpy_only():
                    want = D.resample(rgb, size)
                exact(nr_image.resample8(pixels, size, bgra), want)
                cases += 1
    print(f'raw resize: {cases} cases byte-identical to NumPy')


def letterbox_checks():
    for level in range(8):
        wire = np.full((96, 128, 4), level, np.uint8)
        wire[12:-12, 8:-8, :3] = 128
        wire[..., 3] = 255
        rgb = decoded(wire, True)
        for tolerance in (0, 1 / 255, 2 / 255, 2.49 / 255, 3 / 255):
            assert D.active_region(wire[..., :3], tolerance=tolerance) == D.active_region(
                rgb, tolerance=tolerance)
    raw_box, float_box = D.Letterbox(), D.Letterbox()
    for level in (0, 1, 2, 3, 2, 0):
        wire = np.full((96, 128, 4), level, np.uint8)
        wire[12:-12, 8:-8, :3] = 128
        assert raw_box.region(wire[..., :3], 'same') == float_box.region(decoded(wire, True), 'same')
    print('raw letterbox: threshold boundaries and cached changes match')


class Request:
    def __init__(self, data):
        self.data, self.offset, self.reply = data, 0, None

    def recv_into(self, view, count):
        take = min(count, len(self.data) - self.offset)
        view[:take] = self.data[self.offset:self.offset + take]
        self.offset += take
        return take

    def sendall(self, data):
        self.reply = bytes(data)


def digest(array):
    return None if array is None else (array.shape, array.dtype.str,
                                       hashlib.sha256(array.tobytes()).hexdigest())


class Backend:
    """A deterministic head depending on the actual features; no GPU or weights."""
    def __init__(self):
        self.inputs = []

    def run_features(self, features):
        self.inputs.append(digest(features))
        head = np.empty(features.shape[:2] + (4,), np.float32)
        head[..., :3] = features[..., 4:7] * np.float32(.75) + features[..., 7:10] * np.float32(.25)
        head[..., 3] = features[..., 4] * np.float32(2)
        return head


def arguments():
    live = SimpleNamespace(profile='standard', intensity=.8, detail_strength=1.,
                           colour_strength=1., render_scale=.5, min_extent=128,
                           temporal=1., cut_limit=.15, hold=1., release=24., refresh=lambda: None)
    return SimpleNamespace(live=live, letterbox=D.Letterbox(), history=D.History(),
                           max_pixels=1920 * 1080, dump=None, meter=None)


@contextlib.contextmanager
def host_mode(enabled):
    before = os.environ.get('NR_HOST_BGRA8')
    os.environ['NR_HOST_BGRA8'] = str(int(enabled))
    try:
        yield
    finally:
        if before is None:
            os.environ.pop('NR_HOST_BGRA8', None)
        else:
            os.environ['NR_HOST_BGRA8'] = before


def daemon_checks(expect_raw=True):
    rng = np.random.default_rng(8)
    cases = 0
    packed_calls = 0
    compose = nr_image.compose_encode

    def observed_compose(*args, **kwargs):
        nonlocal packed_calls
        if kwargs.get('decode_colour'):
            packed_calls += 1
        return compose(*args, **kwargs)

    nr_image.compose_encode = observed_compose
    try:
        for fmt in (37, 43, 44, 50, 64):
            for boxed in (False, True):
                a, b = arguments(), arguments()
                back_a, back_b = Backend(), Backend()
                wire = rng.integers(20, 240, (360, 640, 4), dtype=np.uint8)
                if boxed:
                    wire[:32, :, :3] = wire[-32:, :, :3] = 2
                    wire[:, :16, :3] = wire[:, -16:, :3] = 2
                for frame in range(9):
                    settings = {'render_scale': .5 if frame < 3 else .7,
                                'profile': 'natural' if frame >= 4 else 'standard',
                                'detail_strength': .75 if frame == 6 else 1.,
                                'temporal': 0. if frame == 7 else 1.}
                    if frame == 5:
                        settings['render_scale'] = 1.
                    for name, value in settings.items():
                        setattr(a.live, name, value); setattr(b.live, name, value)
                    current = wire.copy()
                    if frame in (1, 2):
                        current[80:90, 100:120, :3] ^= np.uint8(1)
                    if frame == 8:
                        current[32:-32, 16:-16, :3] ^= np.uint8(127)
                    mask = np.zeros(current.shape[:2], np.uint8)
                    mask[100:150, 50:250] = 255
                    masked = frame in (0, 2, 4)
                    stream = struct.pack('<4I', D.MAGIC_MASKED if masked else D.MAGIC,
                                         640, 360, fmt) + current.tobytes()
                    if masked:
                        stream += mask.tobytes()
                    ra, rb = Request(stream), Request(stream)
                    with contextlib.redirect_stdout(io.StringIO()):
                        with host_mode(False):
                            D.process_connection(ra, back_a, a)
                        with host_mode(True):
                            D.process_connection(rb, back_b, b)
                    assert ra.reply == rb.reply, (fmt, boxed, frame, 'wire differs')
                    assert back_a.inputs[-1] == back_b.inputs[-1], (fmt, boxed, frame, 'features differ')
                    for field in ('source', 'pixels', 'output', 'inner'):
                        assert digest(getattr(a.history, field)) == digest(getattr(b.history, field)), (
                            fmt, boxed, frame, field)
                    assert a.history.cut == b.history.cut
                    assert a.letterbox.bounds == b.letterbox.bounds
                    cases += 1
    finally:
        nr_image.compose_encode = compose
    if expect_raw:
        assert packed_calls > 0, 'the test did not exercise raw composition'
    else:
        assert packed_calls == 0, 'disabled native code unexpectedly ran raw composition'
    print(f'daemon raw host: {cases} frames byte-identical, {packed_calls} raw calls; state matches')


def main():
    assert nr_image.library() is not None and hasattr(nr_image.library(), 'nr_resample8')
    resample_checks()
    letterbox_checks()
    daemon_checks()
    with numpy_only():
        daemon_checks(expect_raw=False)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Packed input composition vs the float path, including bytes and temporal state."""
import pathlib
import sys
import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'layer'))
import nr_daemon as D
import nr_frame as F
import nr_image
from test_native_image import numpy_only


def decoded(raw, bgra):
    rgb = raw[..., 2::-1] if bgra else raw[..., :3]
    return np.divide(rgb, np.float32(255), dtype=np.float32)


def exact(a, b):
    np.testing.assert_array_equal(np.ascontiguousarray(a).view(np.uint8),
                                  np.ascontiguousarray(b).view(np.uint8))


def main():
    assert nr_image.library() is not None, 'build work/libnr_image.so first'
    rng = np.random.default_rng(81)
    cases = 0
    for bgra in (False, True):
        for view in ('crop', 'reverse', 'strided'):
            wire = rng.integers(0, 256, (48, 72, 4), dtype=np.uint8)
            raw = wire[4:44, 8:64]
            if view == 'reverse':
                raw = raw[::-1, ::-1]
            elif view == 'strided':
                raw = raw[::2, ::2]
            raw.flags.writeable = False
            h, w = raw.shape[:2]
            colour = decoded(raw, bgra)
            prev_raw = np.array(raw, copy=True)
            prev_raw[:h // 2, :, :3] ^= np.uint8(3)
            if view == 'reverse':
                prev_raw = prev_raw[::-1, ::-1]
            elif view == 'strided':
                buffer = np.empty((h * 2, w * 2, 4), np.uint8)
                buffer[::2, ::2] = prev_raw
                prev_raw = buffer[::2, ::2]
            previous = decoded(prev_raw, bgra)
            history = rng.random((h, w, 3), dtype=np.float32)
            mask = rng.random((h, w, 3), dtype=np.float32)
            for hh, ww in ((max(1, h // 3), max(1, w // 3)), (h, w), (h, max(1, w // 2))):
                storage = rng.normal(0, .5, (hh + 2, ww + 2, 16)).astype(np.float32)
                head = storage[1:hh + 1, 1:ww + 1]
                for temporal, hold, release, control, intensity in (
                        (False, 0, 0, None, 0.6), (False, 0, 0, None, 1.66),
                        (True, 1, 24, None, 1), (True, .5, 8, mask, 1.66),
                        (True, 0, 0, None, .6), (True, 1, 0, None, 1)):
                    opts = dict(intensity=intensity, control_mask=control, samples=8)
                    if temporal:
                        opts.update(history=history, history_previous=previous,
                                    history_hold=hold, history_release=release,
                                    history_confidence=.7)
                    for grade in (None, F.grade_for(1 / 128), F.grade_for(2 / 128)):
                        full = rng.integers(0, 256, (h + 6, w + 8, 4), dtype=np.uint8)
                        channels = 4 if temporal else 3
                        up = D.resample(head[..., :channels], (h, w))
                        ref_kept = np.empty_like(colour)
                        compose_opts = {k: v for k, v in opts.items() if k != 'samples'}
                        with numpy_only():
                            expected = F.compose(up, colour, grade=grade, neural=ref_kept,
                                                 **compose_opts)
                            ref_bytes = D.encode(expected, full[3:3+h, 4:4+w].tobytes(),
                                                 44 if bgra else 37)
                        ref_wire = full.copy()
                        ref_wire[3:3+h, 4:4+w] = np.frombuffer(ref_bytes, np.uint8).reshape(h, w, 4)
                        encoded = full.copy()
                        kept = np.empty_like(colour)
                        baseline, sample = F.compose_encode(
                            head, colour, encoded, top=3, left=4, bgra=bgra,
                            grade=grade, neural=kept, **opts)
                        exact(baseline, expected); exact(kept, ref_kept)
                        exact(encoded, ref_wire); exact(sample, up[::8, ::8])
                        # Both packed previous pixels and a float previous frame are valid.
                        for use_previous8 in ((False, True) if temporal else (False,)):
                            packed = full.copy()
                            packed_kept = np.empty_like(colour)
                            got, points = F.compose_encode(
                                head, colour, packed, top=3, left=4, bgra=bgra,
                                colour8=raw, previous8=prev_raw if use_previous8 else None,
                                grade=grade, neural=packed_kept, **opts)
                            exact(got, expected); exact(points, sample); exact(packed, ref_wire)
                            exact(packed_kept, ref_kept)
                            # All alpha and all pixels outside the active region are guards.
                            np.testing.assert_array_equal(packed[..., 3], full[..., 3])
                            np.testing.assert_array_equal(packed[:3], full[:3])
                            cases += 1
                            in_place = full.copy()
                            in_place[3:3+h, 4:4+w] = raw
                            aliased_raw = in_place[3:3+h, 4:4+w]
                            in_place_kept = np.empty_like(colour)
                            result, points = F.compose_encode(
                                head, colour, in_place, top=3, left=4, bgra=bgra,
                                colour8=aliased_raw, previous8=prev_raw if use_previous8 else None,
                                grade=grade, neural=in_place_kept, **opts)
                            wanted_wire = ref_wire.copy()
                            wanted_wire[3:3+h, 4:4+w, 3] = raw[..., 3]
                            exact(result, expected); exact(points, sample)
                            exact(in_place, wanted_wire); exact(in_place_kept, ref_kept)
                            cases += 1
    raw = np.zeros((8, 8, 4), np.uint8)
    head = np.zeros((8, 8, 3), np.float32)
    try:
        F.compose_encode(head, decoded(raw, True), raw, colour8=raw[::-1])
    except ValueError as error:
        assert 'alias' in str(error)
    else:
        raise AssertionError('encoded/source alias accepted')
    # A negative control: raw pixels must be read, even when the float source is unchanged.
    changed = raw.copy()
    changed[..., :3] = 255
    a = F.compose_encode(head, decoded(raw, True), raw.copy(), colour8=raw)
    b = F.compose_encode(head, decoded(raw, True), raw.copy(), colour8=changed)
    assert not np.array_equal(a, b), 'packed inputs were ignored'
    print(f'packed composition: {cases} cases byte-identical to NumPy '
          '(RGB floats, wire bytes, head samples, neural history); guards OK')


if __name__ == '__main__':
    main()

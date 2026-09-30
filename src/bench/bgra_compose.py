#!/usr/bin/env python3
"""Paired CPU composition experiment; includes the packed path's answer copy.

--pipeline also times decode, resize and feature assembly. No network, game, socket
or FPS measurement is made here. Inputs and histories are identical in both modes.
"""
import argparse
import ctypes
import os
import pathlib
import statistics
import sys
import time

os.environ.setdefault('OMP_NUM_THREADS', '8')
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'src/layer'), str(ROOT / 'src/ref')]
import numpy as np
import nr_daemon as D
import nr_frame as F
import nr_image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--size', nargs=2, type=int, default=(720, 1280), metavar=('H', 'W'))
    parser.add_argument('--pairs', type=int, default=21)
    parser.add_argument('--scale', type=float, default=.55)
    parser.add_argument('--pipeline', action='store_true')
    parser.add_argument('--raw-pipeline', action='store_true',
                        help='fuse input decode/resize and decode the full colour during composition')
    parser.add_argument('--profile', choices=F.PROFILES, default='standard')
    parser.add_argument('--packed-history', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--baseline-library', type=pathlib.Path,
                        help='use a separately built stable library for float composition')
    args = parser.parse_args()
    h, w = args.size
    if min(h, w, args.pairs) < 1 or not 0 < args.scale <= 1:
        parser.error('positive dimensions/pairs and scale in (0,1] required')
    if args.raw_pipeline:
        if not args.pipeline:
            parser.error('--raw-pipeline requires --pipeline')
        args.packed_history = False
    native = nr_image.library()
    if native is None or not hasattr(native, 'nr_compose_encode8'):
        parser.error('build the experimental work/libnr_image.so first')
    if args.baseline_library:
        baseline = ctypes.CDLL(str(args.baseline_library.resolve()))
        entry = baseline.nr_compose_encode
        entry.argtypes, entry.restype = native.nr_compose_encode.argtypes, None
        native.nr_compose_encode = entry
    rng = np.random.default_rng(81)
    raw = rng.integers(0, 256, (h, w, 4), dtype=np.uint8)
    raw_bytes = raw.tobytes()
    previous8 = raw.copy()
    previous8[::5, :, :3] ^= np.uint8(3)
    previous = D.decode(previous8.tobytes(), w, h, 44)
    colour = D.decode(raw_bytes, w, h, 44)
    history = rng.random((h, w, 3), dtype=np.float32)
    hh, ww = max(1, round(h * args.scale)), max(1, round(w * args.scale))
    history_inner = D.resample(history, (hh, ww))
    geometry = F.network_geometry(ww, hh)
    head = rng.normal(0, .25, (hh, ww, 4)).astype(np.float32)
    grade = F.grade_for(**F.PROFILES[args.profile])

    def run(packed):
        # Equivalent to a freshly received request; outside the measured interval.
        payload = bytearray(raw_bytes)
        start = time.perf_counter()
        current = colour
        if args.pipeline:
            if packed and args.raw_pipeline:
                current = np.empty_like(colour)
                inner = nr_image.resample8(np.frombuffer(payload, np.uint8).reshape(h, w, 4),
                                           (hh, ww), True)
            else:
                current = D.decode(payload, w, h, 44)
                inner = D.resample(current, (hh, ww))
            F.build_features(inner, geometry=geometry, history=history_inner,
                             **F.PROFILES[args.profile])
        # Keeping packed history requires a fresh answer. With float history, the
        # current bytes may be read and overwritten in the same per-pixel pass.
        answer = bytearray(payload) if packed and args.packed_history else payload
        encoded = np.frombuffer(answer, np.uint8).reshape(h, w, 4)
        neural = np.empty((h, w, 3), np.float32)
        out, samples = F.compose_encode(
            head, current, encoded, history=history, history_previous=previous,
            history_hold=1, history_release=24, samples=8,
            colour8=np.frombuffer(payload, np.uint8).reshape(h, w, 4) if packed else None,
            previous8=previous8 if packed and args.packed_history else None,
            grade=grade, neural=neural, decode_colour=packed and args.raw_pipeline)
        return (time.perf_counter() - start) * 1000, out, encoded, samples, neural, current

    expected = run(False)[1:]
    for mode in (False, True):
        run(mode)
    times = {False: [], True: []}
    for pair in range(args.pairs):
        for mode in ((False, True) if pair % 2 == 0 else (True, False)):
            elapsed, *values = run(mode)
            for got, want in zip(values, expected):
                np.testing.assert_array_equal(got.view(np.uint8), want.view(np.uint8))
            times[mode].append(elapsed)
    print(f'{w}x{h}, scale {args.scale}, threads {os.environ["OMP_NUM_THREADS"]}, '
          f'{args.pairs} alternating pairs, pipeline={args.pipeline}, '
          f'profile={args.profile}, packed_history={args.packed_history}, '
          f'raw_pipeline={args.raw_pipeline}')
    for mode in (False, True):
        t = times[mode]
        print(f'{"packed BGRA8" if mode else "float RGB"}: median {statistics.median(t):.3f} ms, '
              f'range {min(t):.3f}..{max(t):.3f}')
    before, after = (statistics.median(times[m]) for m in (False, True))
    print(f'saved {before - after:.3f} ms, speedup {before / after:.3f}x; all outputs byte-identical')
    differences = [a - b for a, b in zip(times[False], times[True])]
    print(f'paired median saving {statistics.median(differences):.3f} ms, '
          f'packed wins {sum(d > 0 for d in differences)}/{len(differences)} pairs')


if __name__ == '__main__':
    main()

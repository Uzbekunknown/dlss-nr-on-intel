#!/usr/bin/env python3
"""Paired in-process daemon request-to-answer timing, optionally using the actual GPU.

The request transport is a memory stand-in, not a socket or game FPS measurement.
The real daemon, temporal state and optional resident model are exercised.
"""
import argparse
import contextlib
import io
import os
import pathlib
import statistics
import struct
import sys
import time

os.environ.setdefault('OMP_NUM_THREADS', '8')
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'src/layer'), str(ROOT / 'src/ref')]
import numpy as np
import nr_daemon as D
import nr_frame as F
from test_bgra_pipeline import Request, Backend, arguments, host_mode, digest


class TimedRequest(Request):
    def sendall(self, data):
        super().sendall(data)
        self.sent_at = time.perf_counter()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--size', nargs=2, type=int, default=(720, 1280), metavar=('H', 'W'))
    parser.add_argument('--pairs', type=int, default=21)
    parser.add_argument('--warmup', type=int, default=3)
    parser.add_argument('--scale', type=float, default=.55)
    parser.add_argument('--profile', choices=F.PROFILES, default='standard')
    parser.add_argument('--engine', choices=('mock', 'xmx'), default='mock')
    args = parser.parse_args()
    h, w = args.size
    if min(h, w, args.pairs) < 1 or args.warmup < 0 or not 0 < args.scale <= 1:
        parser.error('positive dimensions/pairs, nonnegative warmup, scale in (0,1] required')
    backend = F.ResidentBackend(F.WEIGHTS) if args.engine == 'xmx' else Backend()
    holders = {mode: arguments() for mode in (False, True)}
    for holder in holders.values():
        holder.live.render_scale, holder.live.min_extent = args.scale, 320
        holder.live.profile = args.profile
    rng = np.random.default_rng(19)
    frame = rng.integers(8, 248, (h, w, 4), dtype=np.uint8)
    times = {False: [], True: []}
    whole = {False: [], True: []}
    runtime_note = ''
    try:
        for pair in range(args.pairs + args.warmup):
            current = frame.copy()
            current[h//3:h//2, w//3:w//2, :3] ^= np.uint8(pair % 3)
            stream = struct.pack('<4I', D.MAGIC, w, h, 44) + current.tobytes()
            replies = {}
            for mode in ((False, True) if pair % 2 == 0 else (True, False)):
                request = TimedRequest(stream)
                with host_mode(mode), contextlib.redirect_stdout(io.StringIO()):
                    start = time.perf_counter()
                    D.process_connection(request, backend, holders[mode])
                    finished = time.perf_counter()
                replies[mode] = request.reply
                if pair >= args.warmup:
                    times[mode].append((request.sent_at - start) * 1000)
                    whole[mode].append((finished - start) * 1000)
            assert replies[False] == replies[True], f'pair {pair}: encoded output differs'
            for field in ('source', 'pixels', 'output', 'inner'):
                assert digest(getattr(holders[False].history, field)) == digest(
                    getattr(holders[True].history, field)), (pair, field)
        if args.engine == 'xmx':
            runtime_note = (f'runtime staging={int(backend.runtime.staging)} '
                            f'input_fp16={int(backend.runtime.input_fp16)}')
            if os.environ.get('XMX_STAGING') == '1':
                assert backend.runtime.staging, 'requested staging did not activate'
            if os.environ.get('NR_INPUT_FP16') == '1':
                assert backend.runtime.input_fp16, 'requested half input did not activate'
    finally:
        if hasattr(backend, 'close'):
            backend.close()
    print(f'{w}x{h}, engine={args.engine}, profile={args.profile}, scale={args.scale}, '
          f'{args.pairs} alternating pairs after {args.warmup} warmup; all bytes/state match')
    if runtime_note:
        print(runtime_note)
    for mode in (False, True):
        print(f'{"raw host" if mode else "float host"}: answer median '
              f'{statistics.median(times[mode]):.3f} ms, range '
              f'{min(times[mode]):.3f}..{max(times[mode]):.3f}; full call '
              f'{statistics.median(whole[mode]):.3f} ms')
    differences = [a-b for a, b in zip(times[False], times[True])]
    print(f'paired median saving {statistics.median(differences):.3f} ms; '
          f'raw wins {sum(d > 0 for d in differences)}/{len(differences)} pairs')


if __name__ == '__main__':
    main()

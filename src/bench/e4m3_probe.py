#!/usr/bin/env python3
"""GPU comparison against current publish.glsl; isolated timings, not model FPS.

Build: glslangValidator --target-env vulkan1.3 -Isrc/gpu
       -o work/e4m3_probe.spv src/bench/e4m3_probe.comp
"""
import json
import os
import pathlib
import sys
import time
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
os.environ['XMX_UNARY_SPV'] = str(ROOT / 'work/e4m3_probe.spv')
# publish.glsl's e4m3 is the magic one where libxmx turns constant 5 on (Mesa): compare the
# candidate against the scale-and-roundEven form, which the constant off keeps.
os.environ['XMX_E4M3_MAGIC'] = '0'
sys.path.insert(0, str(ROOT / 'src/gpu'))
import xmxres

rng = np.random.default_rng(20261013)
half = np.arange(65536, dtype=np.uint16).view(np.float16).astype(np.float32)
random = rng.integers(0, 2**32, 1 << 20, dtype=np.uint32).view(np.float32)
# Neighbours of every E4M3 midpoint in its normal/subnormal grid.
grid = np.unique(np.concatenate([np.arange(8)/512,
    *(np.arange(8,16)*2.0**(e-3) for e in range(-6,9))]).astype(np.float32))
grid = grid[grid <= 448]
mid = (grid[:-1] + grid[1:]) * np.float32(0.5)
edges = np.concatenate([mid, np.nextafter(mid, np.float32(-np.inf)),
                        np.nextafter(mid, np.float32(np.inf))])
special = np.array([0, 0x80000000, 0x7f800000, 0xff800000,
                    0x7fc00001, 0xffc00001, 0x7f800001], np.uint32).view(np.float32)
values = np.concatenate([half, random, edges, -edges, special])
rt = xmxres.Runtime()
src = rt.buffer_from(values)
old, new = rt.buffer(values.size), rt.buffer(values.size)
rt.begin(); rt.unary(2, src, new, values.size, second=old); rt.submit()
a, b = old.view()[:values.size].copy(), new.view()[:values.size].copy()
bad = a.view(np.uint32) != b.view(np.uint32)
# The initial CPU oracle differed only for FP32 subnormal inputs. Keep these
# in GPU-vs-GPU comparison; exclude them from the CPU gradual-underflow oracle.
finite = np.isfinite(values)
cpu_mask = finite & ((np.abs(values) >= np.finfo(np.float32).tiny) | (values == 0))
v = values[cpu_mask]
magnitude = np.minimum(np.abs(v), np.float32(448))
exponent = np.maximum((magnitude.view(np.uint32) >> 23).astype(np.int32), 121) - 3
step = (exponent.astype(np.uint32) << 23).view(np.float32)
reciprocal = ((254-exponent).astype(np.uint32) << 23).view(np.float32)
want = np.rint(magnitude*reciprocal)*step
want = np.where(v < 0, -want, want)
reference_bad = int(np.count_nonzero(a[cpu_mask].view(np.uint32) != want.view(np.uint32)))
result = {'inputs': int(values.size), 'bit_mismatches': int(bad.sum()),
          'current_vs_cpu_normal_or_zero_mismatches': reference_bad,
          'cpu_oracle_excluded_fp32_subnormals': int((finite & ~cpu_mask).sum()),
          'nan_inputs': int(np.isnan(values).sum()),
          'nonfinite_inputs': int((~np.isfinite(values)).sum())}
if bad.any():
    i = int(np.flatnonzero(bad)[0])
    result['first'] = {key: hex(int(val.view(np.uint32)))
                       for key, val in [('input', values[i]), ('old', a[i]), ('new', b[i])]}
elif reference_bad == 0:
    # Alternating order controls some clock/order bias. Each time includes one
    # submit/fence and 32 dependent full-buffer passes; these are not graph timings.
    elapsed = {0: [], 1: []}
    for round_index in range(35):
        for mode in ((0, 1) if round_index % 2 == 0 else (1, 0)):
            start = time.perf_counter()
            rt.begin()
            for _ in range(32):
                rt.unary(mode, src, new, values.size)
            rt.submit()
            if round_index >= 25:
                elapsed[mode].append(1000 * (time.perf_counter()-start)/32)
    result['isolated_ms_per_pass'] = {
        label: {'median': float(np.median(elapsed[mode])),
                'min': float(min(elapsed[mode])), 'max': float(max(elapsed[mode]))}
        for mode, label in [(0, 'current'), (1, 'candidate')]}
(ROOT/'work/e4m3-result.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
raise SystemExit(1 if bad.any() or reference_bad else 0)

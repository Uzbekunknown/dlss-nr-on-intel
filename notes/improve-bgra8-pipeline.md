# BGRA8 input and deferred full-color decode

2026-09-30. Branch `experiment-bgra8-pipeline`, following the direct-composition
experiment `34dfcc5`. This is opt-in via `NR_HOST_BGRA8=1`, off by default.

## The work removed

The reduced model input is now decoded/resized directly from wire bytes. Bilinear
interpolation keeps its separable float32 rounding order, but its first-axis
intermediate is a worker's row, not a whole image. Whole-factor downscales keep
the existing area mean and its accumulation order. Original RGB color is filled
in a separate output array while composition reads the wire bytes, after inference.
It is still retained for temporal history, meters and statistics; the separate
full-frame decode before inference has gone.

Letterbox discovery and cached edge checks operate on the three color bytes,
excluding alpha, with the exact cutoff of the previous decoded-float comparison.
Composition can read and write the same wire region, with all source channels
read before the pixel is encoded. The alpha and bars remain original.

The production float path remains selected without the environment flag. Unsupported
formats, unavailable native code, full-scale input, nondefault strength operations
and dumps use its decode. A first frame with an interface mask can also fall back
after raw input preparation: the fused composer has returned without writing any
bytes, so the original input remains available. History remains float; keeping
packed previous pixels would force a copy and did not pay in the first experiment.

Raw conversion uses the exact 256-entry float lookup by default; compile
`NR_PACKED_DIVISION` to reproduce direct division instead. The earlier note's
`NR_PACKED_LOOKUP` applies to the earlier commit. Existing float decoding is unchanged.

## Validation

- All 44 CTest entries pass in 114.91 seconds on the local Intel LNL device.
- 1,080 direct-composition cases match independent NumPy bit for bit.
- 48 raw resize cases match NumPy, including area/bilinear, identity, single-axis,
  reversed/strided inputs and upscaling. Letterbox threshold/cached-change checks pass.
- 90 daemon frames match the decoded pipeline: output bytes, features, cut values,
  crop bounds and every history array. This covers four 8-bit formats, HDR fallback,
  boxes, masks, render-scale/profile changes, strengths and temporal on/off. Another
  90 frames pass with native processing disabled and zero raw-composition calls.
- AddressSanitizer/UndefinedBehaviorSanitizer find no errors in both new tests and
  the native image suite; Python leak detection is disabled.
- Actual resident-model pairs also produce identical bytes/history. A forced
  staging/half-input check passes and verifies both modes from runtime state.

## Performance: small and inconsistent

The initial CPU segment comparison favored 1080p by 1.445 ms but made 720p slower.
That isolated segment is insufficient to judge a game: GPU execution changes when
CPU caches cool, and the full daemon also performs history and logging work.

The in-process request benchmark runs the actual resident model and the real daemon
handler, with a memory transport replacing the socket. It measures to the answer,
checks every output and history, alternates modes, and excludes warmup frames.
It does not measure game FPS. On Intel LNL, scale 0.55, standard profile, eight
OpenMP threads, the repeated run with 31 pairs and five warmup pairs gave:

| Size | Float answer median, ms | Raw answer median, ms | Paired median saving, ms | Raw wins |
| --- | ---: | ---: | ---: | ---: |
| 1280x720 | 55.737 | 55.290 | 0.624 | 18/31 |
| 1920x1080 | 107.014 | 106.708 | 0.049 | 16/31 |
| 800x450 | 31.226 | 30.970 | 0.163 | 16/31 |

An earlier 15-pair GPU run saved about 1.9 ms by paired medians; the longer repeat
did not sustain that. There is therefore no demonstrated substantial throughput
gain, especially at the 800x450 size used by the Arc testers. The flag stays off;
this branch is preserved for comparison on other CPUs/discrete cards, not adopted
as a default optimization. No B570/B580 hardware measurement was made here.

## Reproduce

```sh
make all
OMP_NUM_THREADS=8 python3 src/ref/test_bgra_pipeline.py
OMP_NUM_THREADS=8 python3 src/bench/bgra_daemon.py --engine xmx --size 450 800 --pairs 31 --warmup 5
```

The benchmark internally toggles `NR_HOST_BGRA8` and keeps independent matching
histories. It reports actual staging/half-input modes and checks a requested mode
is active. `--engine mock` isolates a deterministic stand-in head, and
`src/bench/bgra_compose.py --pipeline --raw-pipeline` times only the CPU segment.
Use a separately built unmodified image library through `--baseline-library`
when comparing that segment against the stable float implementation.

The experimental daemon can be started with `NR_HOST_BGRA8=1` in its environment;
the flag does not change an already-running process. Logs and binaries stay in
ignored `work/`. The existing `improve` worktree and Claude worktree are untouched.

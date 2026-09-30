# BGRA8 composition: direct reads still do not pay

2026-09-30. Experimental branch `experiment-bgra8-direct`, based on `79b2730`.
This branch is not integrated into the daemon or `improve`.

## What changed

The first prototype decoded whole rows into float scratch before composing. This
one reads each pixel's bytes directly in the existing composition arithmetic,
including the vendor history and color grade added since that prototype. It
supports both BGRA and RGBA, cropped and reversed/strided views, and an optional
packed previous source. There is no decoded source-row allocation.

An optional `NR_PACKED_LOOKUP` compile define replaces `/255.0f` with a table of
exactly those 256 float values. The original float entry point and arithmetic are
retained. Each thread still needs the existing head/grade scratch. Scratch failure
is reported by the new packed entry point.

The current source may also be the exact output region: a pixel is read before it
is overwritten. The Python boundary rejects other overlaps and any overlap with
packed previous pixels. Current bytes and encoded bytes are deliberately not
`restrict` pointers. Alpha and pixels outside the active region remain unchanged.

## Correctness

`test_bgra_compose.py` checks 1,080 packed cases against an independent NumPy
composition and encoder, not only another invocation of the same native loop.
It compares composition floats, encoded bytes, sampled head values and neural
history bit for bit. Cases include both byte orders, three input-view layouts,
three head extents, masks, temporal hold/release, three grades, separate and
in-place output, and float or packed previous sources. A negative control changing
only the packed pixels proves they are read. Invalid overlap is rejected.

The native image regression suite also passes. AddressSanitizer and
UndefinedBehaviorSanitizer report no errors for the packed test and the native
image suite (Python leak detection disabled). Both default division and lookup
implementations pass the packed comparisons. Make and CMake register the new
test; the build parity check passes.

## Performance: no adoption

Local CPU measurements, eight OpenMP threads, scale 0.55, 41 alternating pairs.
The measured segment includes decode, resize, feature assembly, composition,
neural history allocation and encoding. It excludes GPU inference, sockets,
letterbox discovery and other daemon bookkeeping. Every timed pair's results
are compared outside the timer. These are not game FPS or B580 measurements.

The final comparison uses a separately compiled, unmodified `79b2730` image
library for float composition, with the same compiler flags. The packed candidate
uses the lookup table. This avoids timing a modified baseline through the new
composition dispatcher.

| Profile | Size | Packed history/output | Stable float, ms | Packed, ms |
| --- | --- | --- | ---: | ---: |
| standard | 1280x720 | packed / copy | 3.465 | 4.613 |
| standard | 1280x720 | float / in place | 4.045 | 4.359 |
| standard | 1920x1080 | packed / copy | 11.128 | 11.735 |
| standard | 1920x1080 | float / in place | 10.939 | 11.181 |
| natural | 1280x720 | packed / copy | 4.701 | 5.204 |
| natural | 1280x720 | float / in place | 5.064 | 5.725 |
| natural | 1920x1080 | packed / copy | 12.900 | 14.126 |
| natural | 1920x1080 | float / in place | 13.032 | 13.303 |

Earlier trials with direct division, lookup, and in-place writing also did not
show a repeatable gain. One in-place run was less than 1% faster at 1080p and
slower at 720p; the comparison against the unmodified baseline above did not
repeat that result. Frame times vary substantially on this shared host, so small
differences are not evidence of improvement.

The remaining decoded full frame is the architectural constraint: other consumers
still need it. Reading bytes again only changes composition's source reads and
adds conversion work; it does not eliminate the earlier decode or float storage.
Keeping packed previous pixels also needs the original request preserved, costing
an answer copy. Using float previous pixels removes that copy but keeps the float
history. The current implementation is therefore preserved as an experiment,
with no change to the production daemon.

## Reproduce

Build the default division candidate with `make work/libnr_image.so`, then run:

```sh
OMP_NUM_THREADS=8 python3 src/ref/test_bgra_compose.py
OMP_NUM_THREADS=8 python3 src/ref/test_native_image.py
OMP_NUM_THREADS=8 python3 src/bench/bgra_compose.py --pipeline --pairs 41 --size 720 1280
```

For the lookup candidate, compile `src/ref/nr_image.c` into `work/libnr_image.so`
with Make's native image flags and the additional `-DNR_PACKED_LOOKUP`. To compare
against the frozen baseline, build the source from
`git show 79b2730:src/ref/nr_image.c` with the same native flags into a separate
library, then pass `--baseline-library work/libnr_image-stable.so`.
`--no-packed-history` selects the in-place experiment; `--profile natural`
selects the color grade. Raw logs and local binaries remain under ignored `work/`.

A future raw-byte pipeline would need to remove the initial full-frame decode:
fuse decode into the reduced model input, process letterbox/motion statistics
from bytes, and decode full color only for fallback paths. That is a broader
experiment, not a speedup established by this one.

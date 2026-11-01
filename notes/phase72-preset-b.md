# Phase 72 — preset B, NVIDIA's faster model: read out of `.text`, unpacked, running (2026-10-17)

`nvngx_dlssnr.dll` 310.9.16.0 (sha256 `2bc19bc0…28789f`, HANDOFF archive, 10-17) carries two networks.
**Preset B is preset A's graph with every encoder and decoder stage cut to depth 2**: the same
block families, the same eight-block bottleneck, 31 blocks against 71. One function in `.text`
builds both from five stage depths. Unpacked through MLX-DLSS's own decoders and run through
this port, it renders a frame much as A does. On this Arc 140V's resident path it takes
0.54-0.58 of A's time: the full-resolution blocks and the bottleneck are A's and cost the same.

Everything below was read with REA 6.5.0 and Ghidra 12.1.2 on the DLL (image base
`0x180000000`), or measured here. What was not established is listed at the end.

## The builder: one function, two depth arrays

`FUN_1800602e0` makes the descriptor list. It calls the builder `FUN_18005c5a0(descriptor, 0,
depths)` twice:

| preset | depths, `.rdata` | name the descriptor gets | blocks |
| --- | --- | --- | --- |
| A (id 1) | `{4, 4, 6, 8, 8}` at `0x1800de990` | `CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_2026_07_04_22_30_weights` | 71 |
| B (id 2) | `{2, 2, 2, 2, 2}` at `0x1800de9a8` | `hnet-crazy-cuckoo-small-fp8` | 31 |

The builder is 9 KB. It names the network `CCNetwork` and its family `crazy-cuckoo`, and
gives it the name `hnet-vigilant-squid`, which the caller overwrites with the preset's.
It is generic over the depths:

- the stem, a `single_layer_block` with `layer0.dw_weight`;
- four encoder stages at 32, 64, 128 and 256 channels, `depths[0..3]` blocks each. The last
  block of each also registers `layer0.dw_weight` and `layer0.conv_weight`: the transition
  that downsamples;
- the 512 stage, `depths[4]` split blocks, the last flagged to carry the projection to 1024;
- **eight ViT blocks, a loop fixed at 8** (flags for the first and the last), for both presets;
- the merge (`0x400 -> 0x200`: `conv_weight`, `dw_weight`, `sin`), then the decoder mirrored:
  `depths[4]` split blocks, and at each lower level an upsampling block and `depth - 1`
  window blocks; then the head (`FUN_18009e080(.., 1, 1)`).

**The shifted windows** come from two helpers. `FUN_180059c80(i)` for the encoder and
`FUN_1800568f0(depth, i)` for the decoder give a phase, `i & 3` and `(depth + i) & 3`. The
phase picks none, both, x or y (`(-1,-1)`, `(1,1)`, `(1,0)`, `(0,1)`). The stem is phase 0
and the head phase 1. That rule reproduces the table this port took from MLX-DLSS for every
one of A's 71 blocks, including the decoder's offset of two in the depth-6 stage
(`src/ref/test_topology.py`). For B it gives the encoder's two blocks of a stage phases 0
and 1, and the decoder's phases 2 and 3.

**The skips** are wired by role, as for A. Each decoder level's first block takes the block
before it and that level's encoder transition. The merge takes the last ViT block and the
last 512 block, and the head takes the last decoder block and block 0.

**The registry** that maps presets to weights (`FUN_180023b80` walks it) holds one 656-byte
entry per preset, at `0x1800dbe30` (A) and `0x1800dc0c0` (B). They differ in the name, the
id, the resource (`WEIGHTS_HT` / `WEIGHTS_CC_SMALL_FP8`) and the short name
(`CC_SILVER_AARDWOLD` / `CC_SMALL_FP8`), and in nothing else. The style grades of `phase70`
(-0.1, -0.25, -0.1, -0.15) sit in both, unchanged. The debug overlay (`FUN_18001c120`)
calls B `hnet-almond-eagle-11m`.

`src/ref/nr_topology.py` is that builder: block indices, roles and window phases from five
depths, and the depths read back from a set of weight names.

## The weights

`WEIGHTS_CC_SMALL_FP8` is 113 893 343 bytes in the same container as `WEIGHTS_HT`: 77
records against 153. **Every record has the byte size of one of A's record families**
(B's block 1 is A's 1-3, 2 is 4, 9 is 23-29, 10 is 30, 11-18 are 31-38, 19 is 39, and so
on), and **none is byte-identical to any of A's**: B is trained separately, not cut down
from A.

MLX-DLSS's unpacker decodes record families by A's block numbers, so
`src/tools/unpack_preset.py` decodes each of B's records as the A block of the same role
and renames the tensors back. Its `--preset A` reproduces all 649 of
`dlssnr-logical.safetensors`'s tensors exactly, which checks the renaming end to end. B
comes out as 281 tensors over 31 blocks, each through that family's size and trailer
checks.

- **113 342 375 parameters**, counted as `phase61` counted A's 145 755 123: 22% fewer.
  NVIDIA reported "24% smaller" and "roughly 118 million". The bottleneck is 100 679 944
  in both, which is 89% of B.
- Each matrix has its family's `1/sqrt(K)` spread, as in A (sd 0.1767 at K=32, 0.0625 at
  256, 0.0442 at 512, 0.0312 at 1024). The gates lie in [0, 1] and the scales are positive.
- `blend_scale`, the history gate's ceiling, is **0.76708984375** (A: 0.73974609375).
  The daemon reads it from the weights now (`nr_frame.blend_scale_of`).
- The bottleneck's `attention_scalar` is about 1e-6 in B (0.109 and -558.5 in A).
  Neither MLX-DLSS's graph nor ours uses it.

## The kernels of 310.9.16

The PTX is FP8 only now: 120 entries, 112 names, every one of them present in 310.8.
With labels and symbol names made canonical, **66 are instruction for instruction 310.8's**.
The other 54 differ by register numbering, scheduling, two memory fences, a store loop's
bounds halved from 256 to 128, or integer addressing. Float arithmetic changed in
one place, the pre-block (`cc_tinlayout_fused_pre_block_*`): its parameter struct grew by a
pair of fields at +160, with new multiplies and selects around constants 0.5 and 1.0, and
the only new hint string in the DLL is `DLSSNR.MaskBlendMode`. That is the hint's
arithmetic, and the default path is unchanged (below). The bottleneck's
`cc_vit_1d_attention` gained a loop bound of `ceil(tokens / 128)`. Both kernels serve both
presets, so these are differences between 310.8 and 310.9.16, not between A and B.

## Running it

| | A | B |
| --- | --- | --- |
| CPU reference, 320x320 | 10.4 s | 5.7 s |
| device against the CPU reference, 320x320 (`test_resident.py`) | corr 0.9809, spread 1.0067, 415 passes | corr 0.9870, spread 1.0068, 179 passes |
| `nr_frame.py --gpu`, DOA5 1280x720, whole run with the load | 41.5 s | 23.8 s |

The resident path, presets alternating, median of 7 after 5 warm-up frames:

| network field | A | B | B/A |
| --- | --- | --- | --- |
| 576x320 | 34.0 ms | 19.2 ms | 0.56 |
| 640x384 | 41.4 ms | 22.5 ms | 0.54 |
| 896x512 | 64.8 ms | 37.2 ms | 0.57 |
| 1280x736 | 124.3 ms | 70.3 ms | 0.57 |
| 1920x1088 | 267.6 ms | 154.1 ms | 0.58 |

Why not the 0.44 the block count suggests: at 1280x736, one submit per block, A's 137 ms is
86.8 window stages, 18.1 split, 10.9 bottleneck, 10.8 last block and head and 10.6 stem and
block 0. B's 72 ms is 35.7, 4.3, 10.9, 10.9 and 10.5. **The full-resolution blocks and the
bottleneck do not shrink, and they are 45% of B's frame.** They are the next place to look
for B's speed. (Looked at on 10-18, below: what moved B's frame there was the GPU's clock.)

On Dead or Alive 5's 1280x720 frame (`work/doa5/in.png`, through `--gpu`), B changes the
frame by 0.0351 on average against A's 0.0364. On the two faces, luma with fine detail as
the residual of a sigma-2 blur, level-normalised as `phase42` asks:

| face | level: game, A, B | fine / level: game, A, B |
| --- | --- | --- |
| Bayman | 0.419, 0.381, 0.368 | 0.0990, 0.1080, 0.1114 |
| the left one | 0.589, 0.511, 0.492 | 0.0597, 0.0695, 0.0682 |

Both presets pull the level down by 9-16% and add 9-16% of fine detail for it. B sits
within A's range on both faces, a little further on the level. The images stay local.

## `DLSSNR.MaskBlendMode`, and whether faces are milder

News of 10-13 say NVIDIA's revised comparison images show milder changes to faces, alongside
the smaller model. The owner asked whether `DLSSNR.MaskBlendMode`, the only new hint, is
behind it.

**What the hint does.** 310.9.16's parameter parser (`FUN_18001a020`) reads it as 0-3, and
anything else becomes 0, the default. 310.8's (`FUN_180019f30`) had no such hint: a control
mask always switched the automatic mask off. 310.9.16 does so only in mode 0. The pre-block
kernel then fills channels 11-14 per pixel; its feature filler is
`HNetCpp::cc_fused_pre_swin_impl::CCPreBlockShmemFiller`:

- tone is the mask's green times the tone strength, and structure the mask's blue times the
  structure strength, with 1 where there is no mask;
- with the automatic mask off, the skin and automatic-mask lanes are -1;
- with the automatic mask on, structure is 1 and the lanes carry the two strengths;
- with a control mask and mode 1, 2 or 3, the automatic mask stays on, and each strength is
  averaged with, lowered to or raised to the masked structure.

That is all of the pre-block's new float arithmetic: the two 0.5s, the selects and the
compares. Both DLLs default to the automatic mask off and a skin strength of -1, so **310.9.16's
default output is 310.8's**. The mode matters only to a game that sends a control mask and
turns the automatic mask on.

**A bug in our port that it exposed.** With a control mask, the vendor puts -1 in the two
automatic lanes, in both versions. MLX-DLSS's feature builder, which `nr_frame.py
--control-mask` used, put 0. An all-ones mask should change nothing, but with the 0 it
moved Dead or Alive 5's frame by 0.025 on average under A (0.022 under B), max 0.40, which
is two thirds of the pass. With -1 it reproduces the unmasked frame exactly.
`nr_frame.make_features` now fills the lanes as the vendor does and takes the blend mode
(`--mask-blend-mode`), and `nr_temporal.py` follows it. The live daemon never puts a mask
into the features, so it is unaffected.

The kernel writes -1 there only while a flag in its parameters (+200) is set. That the flag
is set is an inference: OpenDLSS-NR, which claims the vendor's inputs bit for bit, writes -1
or the strengths, never 0.

**Is B milder on faces?** Measured here on five frames of Dead or Alive 5 and Mortal Kombat 1,
with eight faces boxed by hand. The figures are the mean |change| in luma on the faces and on
the rest of the frame, through the resident path:

| setting | faces, A → B | rest, A → B | faces / rest, A and B |
| --- | --- | --- | --- |
| standard, no automatic mask (the live default) | 0.0751 → 0.0717 (−5 %) | 0.0372 → 0.0333 (−11 %) | 1.71-3.00, 1.70-3.21 |
| the vendor panel's: structure 1.5, automatic mask, skin 2.0 | 0.0743 → **0.0553 (−26 %)** | 0.0377 → 0.0259 (−31 %) | 1.64-2.95, 1.73-2.99 |
| automatic mask, skin structure 0 | 0.0504 → 0.0522 (+4 %) | 0.0338 → 0.0307 (−9 %) | 1.09-2.49, 1.13-2.73 |

B is not milder on faces in particular. Under the vendor panel's settings it changes the
whole frame about 30 % less than A, faces and environment alike, which a comparison of faces
would show as milder faces. The skin control works in both, a little more selectively in A.
Five frames from two games is a small sample, and the vendor's own output was not compared.

(Both DLLs also map the app's quality mode to the network's scale, MaxQuality and DLAA 1.0
and UltraQuality 0.85, in `ComputeScalingRatioCommon`. 310.8's per-frame parser read
`DLSSNR.ScalingRatio` and overwrote it with 1.0, and 310.9.16's does not read it. Not
followed further.)

## Through the daemon, and the GPU's clock (2026-10-18)

**B through the daemon.** `speed_test.py --preset B` (the test takes no `NR_` setting from its
own environment, so the preset is asked for), presets alternating, two runs each, network and
round trip in ms. This was before the clock hint below, which these back-to-back frames do
not need:

| window | scale | network field | A | B |
| --- | --- | --- | --- | --- |
| 640x360 | 0.4 | 320x320 | 22-23 / 25-26 | 15-18 / 19-22 |
| 800x450 | 0.4 | 320x320 | 22 / 27 | 13-16 / 15-19 |
| 1280x720 | 0.4 | 512x320 | 29 / 34-35 | 18-21 / 24-28 |
| 1920x1080 | 0.4 | 768x448 | 52 / 63-65 | 28 / 40-41 |
| 640x360 | 1.0 | 640x384 | 41 / 45-46 | 22 / 26 |
| 800x450 | 1.0 | 832x512 | 61 / 66-67 | 33 / 39-40 |
| 1280x720 | 1.0 | 1344x768 | 139-140 / 151 | 75 / 85 |
| 1920x1080 | 1.0 | 1920x1152 | 270-271 / 298 | 152 / 175-178 |

Over the network field, four runs a preset later in the day: A is **11 ms + 118 ms a
megapixel**, B **4.8 ms + 67**. B's fixed part is under half of A's, so at the vendor's
smallest field, 320x320, B's network takes 11 ms against A's 22 when the clock stays up. Its
scatter at the small fields, 11 to 18 ms in one day, is the clock, below.

**Where B's frame goes**, `frame_profile.py --weights` after 10-20 warm frames:

- **320x320, 11.6 ms of device time.** The bottleneck is 3.6 ms, 31 %: eight blocks of four
  GEMMs at 64 tokens, 162 + 128 + 88 + 48 us a block, and 17 us of attention. The two
  full-resolution blocks are 2.4 ms: the window blocks 0.74 and 0.72, their feed-forwards
  0.48 and 0.43. The rest is the sixteen blocks of the levels between, over 179 passes.
- **1280x736, 65.7 ms.** The full-resolution blocks are 21-22 ms (window blocks 6.3 and 6.2,
  feed-forwards 4.1 and 3.9, the last upsample 0.75), the bottleneck 8.8, the four
  half-resolution window blocks 6.2.

The full-resolution window block runs ~2.5 TFLOP/s of multiply-adds and its feed-forward
~3.75, where the earlier work left them: the window block is its element-wise work, the cosine
trees, the E4M3 publishes and the softmax's transform (the HANDOFF archive, 09-26). The
bottleneck at 64 tokens waits on its K loop, not on its weights (`notes/improve-b.md`, where
E4M3 weights, a kernel without the shared-memory round trip and other block shapes were
measured). Of what improve-b measured and did not route, the two N = 1024 GEMMs on 32-row
blocks with a 64-deep step (0.25 ms a frame at 64 tokens) are now worth twice the share of
B's frame that they were of A's: 2 %. Not done.

**The clock.** `live_rates.py 640x360@0.4` with 25 warm-up frames and 60 timed, and the GPU's
`act_freq` read every 2 ms (`/sys/class/drm/card0/device/tile0/gt0/freq0`): **A ran at 1636 MHz
on average while busy, B at 1157.** The Arc 140V's maximum is 1950 (`rp0_freq`), its
efficient frequency 500 (`rpe_freq`), and `throttle/reasons` said `none` throughout: nothing
throttled. The GuC's governor (power profile `base`) reads the GPU's idle time between
frames, the client making its next frame and the daemon's host passes, as time to spare,
and B leaves more of it. The same plan, B at 1280x720 and 0.4 after 640x360: 42-53 ms a frame.

Mesa's Intel driver has a driconf option for this, `force_guc_low_latency`
(`src/intel/vulkan/anv_drirc_gen.py`). With it, it puts `DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT`
on its exec queues where the kernel offers the hint (`xe/anv_queue.c`, which the kernel's
`xe_drm.h` says will cost power), and driconf reads its options from the environment when an
instance is made (`util/xmlconfig.c`). Set for the daemon alone, the busy mean was 1908-1936
MHz for both presets, and B's 1280x720 at 0.4 took 21-22 ms.

So **libxmx sets `force_guc_low_latency=true` before it makes its instance** (`xmx_init`),
in its own process only, keeps a value already in the environment, and leaves the driver's
default with `XMX_LOW_LATENCY=0`; the daemon's log says `GPU clock: the driver's low-latency
strategy`. Nothing is computed differently: only the clock moves. `test_device_pick.py` checks
the four cases. `live_rates.py --gap MS` waits that long before each frame, as a game takes
for its own frame; making each test frame takes 5 ms at 640x360 and 23 at 1280x720 already.

Back-to-back frames do not need it (`speed_test.py`, four runs a preset: the same times with
and without, to a millisecond), and neither does preset A on the README's table, whose
medians moved by at most 2 ms (`nr_knobs.RATES`, re-measured with it). Where the GPU waits:

| the client between frames | case | A: default → hint | B: default → hint |
| --- | --- | --- | --- |
| making the frame (5 or 23 ms) | 640x360 at 0.4 | 27 → 26 ms | 14 → 14 |
| | 1280x720 at 0.4 | 62 → 35 | 30 → 22 |
| making it, then waiting 25 ms | 640x360 at 0.4 | 33 → 28 | 27 → 23 |
| | 1280x720 at 0.4 | 91 → 37 | 38 → 27 |

`live_rates.py --gap 0` and `25`, 25 warm-up frames and 30 timed, one run a cell, B's before
A's. Without the hint the same case reads differently from run to run: in an earlier run with
the frames made in advance, A's 1280x720 with the 25 ms wait took 62 ms and B's 55, against
91 and 38 here. With it, a case's frames spread over a few milliseconds, and B's round trip at
1280x720 and 0.4 is 0.6-0.7 of A's.

**Not tested:** what it costs in power and heat, and whether it holds beside a game. On a
laptop the CPU and the GPU share one power budget, and a game that leans on the CPU could
lose by it. In a game the GPU also draws the game's frames, so the governor sees less idle
time there than here.

**Flicker.** The owner saw a slight flicker of the network's own in a game, and put it down
to the network's low resolution. `flicker.py work/good` replays a 22-frame live capture
(1024x768, 1024x576 of it active) through the daemon and takes, on the pixels the game left
byte for byte as they were, the change the network made up. In levels of 255, without history
and with it (`temporal` 0 and 1; 1 is the live default, with `hold` 1):

| network field | A | B |
| --- | --- | --- |
| the 320 floor (scale 0.4, run as 0.56) | 3.35 / 0.82 | 1.55 / 0.39 |
| scale 0.7 | 2.99 / 0.74 | 2.24 / 0.48 |
| scale 1.0 | 3.02 / 0.75 | 2.03 / 0.44 |

**The resolution is not it**: from the floor to the full frame A's figure falls by a tenth and
B's does not fall at all. B makes up about half of A's, but it also moves these frames about
half as far (9.3-9.8 levels from the game's own frame for A, 4.5-6.6 for B, with history), so
per unit of effect the two are alike. The history is what holds it down, four times. What it
leaves is the network drawing its detail again when a pixel changes by a level or two: those
come back changed 2.0-2.2 times as much under A and 1.2-1.4 times under B. The hold floor
covers only the byte-identical pixels (2.3 % of this capture, 7.7 % more within one level);
whether a tolerance there would help is not tried. One capture, one game.

## On Windows: B's graph, its speed, and the same clock (2026-10-18)

Intel's driver 101.9150, mains power, Windows' "best performance", quiet (`quiet-check.ps1`).

**The weights and the graph.** `unpack_preset.py` on the same DLL gives Linux's tensors by
content (B 281, `72669f54…`; A 649, `6ea1305b…`), and so does the setup window's Install NR
from a release built here. `test_resident.py` on Intel's driver: B tracks its CPU reference at
corr 0.987896 over 179 passes, A at 0.981917 over 415.

**Speed**, `speed_test.py --preset` on the MSVC build with a release's Python 3.12, presets
alternating, two runs each, network and round trip in ms:

| window | scale | A | B |
| --- | --- | --- | --- |
| 640x360 | 0.4 | 24-27 / 28-31 | 12 / 15 |
| 800x450 | 0.4 | 24-28 / 28-32 | 12 / 16 |
| 1280x720 | 0.4 | 32-34 / 39-41 | 16 / 23 |
| 1920x1080 | 0.4 | 60-64 / 75-79 | 31 / 45 |
| 640x360 | 1.0 | 43-50 / 48-53 | 23 / 27 |
| 800x450 | 1.0 | 69-73 / 75-79 | 36 / 42 |
| 1280x720 | 1.0 | 169-185 / 181-198 | 87 / 99-100 |
| 1920x1080 | 1.0 | 381-404 / 406-429 | 187-202 / 212-227 |

B's network takes 0.46-0.53 of A's here, against 0.54-0.58 on Linux. At the small fields B
matches Linux (12 ms against 11-15). At full scale both presets are slower than on Linux, A by
1.4x at 1080p, as the graph has been on Intel's compiler (phase71).

**The clock: Intel's Windows driver also clocks the GPU down between frames, and further for
B.** `live_rates.py` speaks AF_UNIX only, so `work/tools-win/live_rates_pipe.py` (scratch, not
tracked) runs its frames, warm-up and median over the daemon's named pipe. The daemon is
started as `speed_test.py` starts it. The GPU's frequency and busy time come from Level Zero's
sysman in Intel's driver (`ze_loader.dll`: `zesFrequencyGetState`, `zesEngineGetActivity`; GPU
domain 100-1950 MHz). 1280x720 and 640x360 at 0.4, 25 warm-up frames and 30 timed, two runs
each:

| between frames | A: round trip, MHz while busy | B: round trip, MHz while busy |
| --- | --- | --- |
| none: frames made first, sent back to back | 39-42 / 30-32 ms, 1908-1950 / 1872-1950 | 22-23 / 15-16 ms, 1950 / 1949-1950 |
| making each one (23 / 5 ms) | 40-43 / 28 ms, 1729-1878 / 1947-1950 | 24-34 / 17-36 ms, 1105-1950 / 656-1950 |
| making it, then 25 ms | 42-45 / 30-32 ms, 1533-1740 / 1752-1896 | 37-39 / 25-28 ms, 865-956 / 901-1029 |

Two runs without the clock reader, made earlier, gave the same round trips: at 1280x720, A 41-42
→ 49-50 ms with the 25 ms wait and B 33-39 → 32-38. The daemon's own split there put B's
network at 26-30 ms against 16 back to back. With the gap, B's round trip at 1280x720 is 0.9 of
A's instead of 0.55. As on Linux without the hint, a case reads differently from run to run:
B with fresh frames held 1950 MHz in one run and fell to 1105 / 656 in the other.

**B in a game, from a release built at `26c5637`.** Install NR with the 310.9.16 DLL took
both presets out of it. Mortal Kombat 11 at 1280x720, render scale 0.4 (network 512x320), was
started from Steam with NR enabled. Frames the daemon processed in 40 s, two runs a cell, presets
alternating:

| frame path | A: processed/s, daemon frame, network | B |
| --- | --- | --- |
| host (the default) | 19.5-19.7, 48-53 ms, 32-35 ms | 18.5-20.0, 44-45 ms, 27-29 ms |
| GPU image path (the window's option) | 22.8-23.9, 39-41 ms, 31-33 ms | 37.3-37.9, 24 ms, 16 ms |

On the host path B's network is only 0.85 of A's: between the game's frames the GPU clocks
down, as above. B saves 5-9 ms of the daemon's frame there, but the frames processed a second
do not rise, and why is not established. On the GPU image path B's network takes its
back-to-back time, 16 ms, and **B processes 1.6x A's frames**. Every run took every frame on its
path, refused none, ended its daemon with the game, and left the game's folder as it was.
Dead or Alive 5 (32-bit, DirectX 9) on B: 22.0 a second at 1024x768, network 22 ms.

No per-process hint like Mesa's `force_guc_low_latency` was found for Intel's Windows driver.
Vulkan has none. Level Zero's `zesFrequencySetRange` would set the device's range for every
program, and is a system setting. Not addressed. The reader stalls single frames by 0.5-1 s
(the maxima of its runs), so its runs are for the clock, and the earlier runs for the times.

## Not established

- **The vendor's own output for B.** Nothing here was compared with NVIDIA's B. That needs
  the RTX 5060 Ti with `DLSSNR.Hint.Render.Preset` set to B.
- Whether the attention's new loop bound changes what the default path computes. The port
  follows 310.8 there, for A and for B. (The pre-block's new arithmetic does not: it is the
  mask blend modes, above.)
- B in a game. The daemon runs it with `NR_PRESET=B`, which has not been tried in a game, and
  neither has the clock hint.
- Why B does not raise Mortal Kombat 11's frames on Windows' host path, where its daemon frame
  is 5-9 ms shorter.
- Holding the clock up on Windows.
- How B looks in a game: the owner to judge.

## Reproduce

```
python3 src/tools/unpack_preset.py work/dl-310.9.16/nvngx_dlssnr.dll \
    work/mlxw/dlssnr-b-logical.safetensors                       # preset B
python3 src/tools/unpack_preset.py work/dl-310.9.16/nvngx_dlssnr.dll /tmp/a.safetensors \
    --preset A --check work/mlxw/dlssnr-logical.safetensors      # the renaming, end to end
python3 src/ref/test_topology.py
python3 src/gpu/test_resident.py                                 # both presets' whole graph
python3 src/ref/nr_frame.py IN.png OUT.png --gpu --weights work/mlxw/dlssnr-b-logical.safetensors
python3 src/bench/frame_profile.py --weights work/mlxw/dlssnr-b-logical.safetensors
NR_PRESET=B ...                                                  # the daemon, on B
python3 src/bench/speed_test.py --preset B
NR_PRESET=B XMX_LOW_LATENCY=0 python3 src/bench/live_rates.py 1280x720@0.4 --gap 25 --warmup 25
cat /sys/class/drm/card0/device/tile0/gt0/freq0/act_freq             # while it runs
```

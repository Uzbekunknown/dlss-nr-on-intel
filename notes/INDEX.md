# Index of the notes

Ninety-one files, sixty-two phases. This is what each one settles, so a reader arriving
cold can go straight to the answer instead of the archaeology.

**Read `notes/HANDOFF.md` first** — it carries the current state, the standing conclusions and
the traps, and **`docs/ARCHITECTURE.md`** for the recovered network as a specification
rather than as archaeology. This index is for going deeper on one question.

Notes are the record of what was *measured*, including measurements that turned out
wrong. Where a note is superseded it says so at the top and is kept anyway: several of
this project's worst hours went into re-deriving something a withdrawn note already
disproved.

## Start here

| note | what it settles |
| --- | --- |
| `phase45-frame-profile.md` | where every millisecond of a frame goes, per pass, from GPU timestamps |
| `phase47-live-mode.md` | the live mode, 10.6 fps at 512x288, and the bottleneck leaving the graph |
| `phase38-there-was-no-bug.md` | the "driver bug" that shaped three phases does not exist |
| `phase9-numerics.md` | why bit-identical agreement with a CPU reference is impossible here |
| `opendlss-reference.md` | a second implementation that claims the vendor's own arithmetic, runnable here: where ours differs from it by specification, and how far our head is from its on the same features (RGB corr 0.97-0.99, 1.1-2.7 levels of 255) |
| `reviewing.md` | how to run `/ultrareview` on this repo without wasting a run |
| `reproduce.md` | how to run the resident path from a clean checkout |

## The binary, the weights, the graph

| note | what it settles |
| --- | --- |
| `phase0-acquire.md` | provenance of the DLL, cryptographically |
| `phase1-binary-map.md` | 89 % of the file is weights; no CUDA runtime imports |
| `phase2-graph-rtti.md` | the layer taxonomy, out of intact MSVC RTTI |
| `phase3-weight-format.md` | FP16, not FP8 — the figure that was wrong for days |
| `phase3-ptx-unlock.md` | the `.data` containers are Zstandard and decompress to PTX |
| `phase3-architecture.md` | five encoder/decoder stages, ViT-1D bottleneck, 32 per head |
| `phase3-execution-order.md` | our decode is byte-identical to a capture from real RTX hardware |
| `phase6-mlx-dlss-unpack.md` | **the container is packed backend payloads, not dense FP16** |
| `phase3-block-layout.md`, `-block-internals.md` | the ordered layout inside each block family |
| `phase3-bias-region.md` | the 128C region is the attention bias, in a 12-bit permutation |
| `phase5-softmax-found.md` | the attention is a softmax, hand-rolled in `f16x2` |
| `phase5-attn-scale-fp32.md` | `attn_scale` is FP32 per head |
| `phase5-*` (the rest) | stem, output head, channel order, resampling, I/O structs, narrow blocks |
| `phase5-no-softmax.md` | **withdrawn** — kept so nobody re-derives it |

## Running it

| note | what it settles |
| --- | --- |
| `hw-coopmat.md` | six cooperative-matrix configs, all M=8 N=16, subgroup scope |
| `phase4-subnormal-flush.md` | XMX flushes subnormal FP16 operands (premise later corrected) |
| `phase4-accumulation-choice.md` | NVIDIA accumulates in FP16; we use FP32 and are more accurate |
| `phase7-first-render.md` | the first real frame, with its two adversarial controls |
| `phase8-xmx-graph.md` | the whole graph on XMX, and the graph is chaotic |
| `phase15-residency.md` | activations never return to the host |
| `phase16-hdr.md` | the display codec, so an HDR frame survives the round trip |
| `phase12-temporal.md` | the temporal gate discriminates: 0.705 right, 0.032 wrong motion |
| `phase28-frame-replay.md` | one submission a frame, commands reused |
| `phase27-pipeline-specialization.md` | specialize before believing a register ceiling |
| `phase32-scratch-and-qk.md` | the scratch arena: 720p from 5041 to 2303 MiB |

## Performance, and the levers that are closed

| note | what it settles |
| --- | --- |
| `phase20-machine-limits.md` | 70-91 GB/s of 136.5, the clock ceiling held, 64 XMX engines |
| `phase26-the-register-ceiling.md` | every engine busy, each ~90 % idle; the register file is the wall |
| `phase45-frame-profile.md` | per-pass timings; everything that moves data is at the memory ceiling — which did not make the graph finished |
| `improve-fusions.md` | residuals, attention and its head merge, the glue, the narrow feed-forward folded into fewer passes; window attention in 2 KB; the staged loader's loads issued together; what was measured and dropped |
| `improve-shared-memory.md` | a Mesa quirk — the core's shared-memory partition sized from the declared bytes, each workgroup's share rounded up — that makes some smaller declarations slower and costs this frame nothing; the 128 KB cap that had the staged GEMM on half its threads, 10 % of a frame; and why window attention and the fused feed-forward, with no L1, fetched the same operands once per subgroup from L2 — and ran at 2x and 1.5x once they shared them |
| `improve-b.md` | the bottleneck's small-M GEMMs: why a direct kernel wins only on cached weights, why the weights' bytes are not the limit, that 320x320 has 64 tokens; the staged loop's 16-bit B loads and the bit-identical operand swap that was slower; other block shapes; the 32-row block that was kept |
| `improve-qkv-epilogue.md` | Q/K normalised in the QKV projection's own epilogue, 22 %; all fusions together 38 %; why joint QKV was slower; shared memory comes in powers of two; measure with empty swap |
| `phase21`, `phase22`, `phase23`, `phase31`, `phase33` | tiling, staging, integer weights, the accumulator, OpenCL — all measured, all closed |
| `phase25-the-frame-rate-wall.md` | `17 ms + 488 ms per megapixel`, and what that forbids — `9 + 205` since the fusions and the shared-memory fix |
| `phase51-output-resolution-costs.md` | what costs the output extent rather than the network's, and a profile that was measuring swap |
| `phase50-what-the-model-computes-in.md` | 36 % of the shipped model's mma is already FP8; what FP4 would and would not change |
| `phase37-neural-upstream.md` | half the extent is 3x faster and keeps 62 % of the high band |
| `phase46-cpu-share.md` | four busy E-cores cost the GPU 7 % for a theoretical +2 % |
| `phase14-npu-and-rounding.md` | the NPU is not worth using; the rounding point was |
| `phase13-torch-and-blas.md` | the system numpy is netlib; every early CPU baseline was 67x too slow |

## In a game

| note | what it settles |
| --- | --- |
| `phase17-integration-landscape.md` | why a Vulkan layer and not NGX |
| `phase19-photo-mode.md` | the loop closed on `vkcube` |
| `phase24-a-real-game.md` | the layer loads inside a Wine prefix |
| `phase34-doa5.md` | a face, from a real game |
| `phase42-doa5-fight-scene.md` | the pass measured on a live fight; the tone shift that fools the eye |
| `phase35-what-the-operator-does.md` | the interface-mask detector, on three real frames |
| `phase43-mask-mottling.md` | the mask making a worse artefact than it prevents, and both fixes |
| `phase44-profile-tradeoff.md` | profiles trade speculars for skin texture; one knob runs backwards |
| `phase41-doa6-and-vkd3d.md` | the layer under VKD3D-Proton; why DOA6LR will not start |
| `phase47-live-mode.md` | live mode, the control tool, 10.6 fps |
| `phase48-feature-inputs.md` | all sixteen feature channels, and two costs that were pure waste |
| `phase52-live-in-a-game.md` | live neural rendering inside a running game, measured |
| `phase53-why-it-flickers.md` | why a static pixel moves 3.3 levels: the network is global |
| `phase54-flicker-fix.md` | the temporal path in live mode; 3.7x less flicker for 3.7 % of the frame |
| `phase55-a-switch-on-a-key.md` | one key over a fullscreen game, and the compositor crash from binding it the wrong way |
| `phase56-the-panel-and-the-manual.md` | every knob on one screen, one table behind the tools and the README, three bugs in a terminal's input path |
| `phase57-native-host-passes.md` | the passes around the network in C, taken from the parallel tree; host 74 -> 28 ms, byte-identical |
| `phase58-before-publishing.md` | the licence, what a repository carries with it, and the four things the check found |
| `phase59-tekken7.md` | 64-bit D3D11 through DXVK, 10.5 fps at 640x360, and the highest history gate yet |
| `phase60-performance-mode.md` | the laptop's performance mode buys nothing: the GPU is frequency-capped at 1950 MHz, not power-capped |
| `phase61-the-parameter-count.md` | **145 755 123 parameters, not 73 841 889** — the published count was the byte count halved; GQA and 27 % subnormals fall with it |
| `phase62-mortal-kombat-1.md` | the first D3D12 title with a picture; full scale does not fit in memory; on MK1 the pass changes colour and removes fine detail rather than adding it |
| `phase63-a-discrete-gpu.md` | the first report from other hardware: an Arc B580 runs 50x slower than this iGPU because `memtype()` preferred host-cached memory, which on a discrete card is system RAM; and a benchmark that never checked its calls printed 495 TFLOP/s for a dispatch that failed |
| `phase64-auditing-the-readme.md` | seven wrong claims on the published page, including a pinned commit that never existed and a frame-time table a third too slow; the rates table and the file references are now generated and checked |
| `phase65-the-discrete-memory-path.md` | the graph's buffers no longer have to be addressable by the host, so a card without resizable BAR keeps its operands in its own memory; forced on this iGPU by `XMX_STAGING=1`, and the frame is bit-identical |
| `improve-present-fences.md` | **how the layer synchronises now**: one path — the present's own semaphores consumed once, copies finished on private fences, no queue drained — and the real MK1 check |
| `phase69-the-stand-that-discriminates.md` | the present test rebuilt so it can fail — frames in flight, a semaphore per image, a real stall, a present queue of another family — and what it found first: the old default waited on the presenting queue, not the drawing one. Its fix never shipped; `improve-present-fences.md` replaced both paths |
| `phase68-what-validation-settles.md` | the present test under the Khronos validation layer: both old sync modes clean, two real bugs in the harness fixed, and a negative control showing the test could not yet prove the wait load-bearing |
| `phase66-the-present-has-a-test.md` | the present's own semaphores instead of a queue idle, behind `NR_LAYER_SYNC`; a headless swapchain makes the layer's copy out and back testable without a game, and the first run found `present_now` calling itself |
| `phase30-control-atlas.md` | what each vendor slider does, and one that does nothing |

## Reviews

| note | what it settles |
| --- | --- |
| `phase39-layer-review.md` | eight findings in `src/layer`; one was a feature that never worked |
| `phase40-gpu-review.md` | four nits in `src/gpu`, and why that area was already right |
| `phase49-from-the-other-tree.md` | seven layer guards and one real bug taken from `ProjectsCodex` |

## Withdrawn or superseded, kept deliberately

`phase5-no-softmax.md`, `phase18-fusion.md` and `phase36-the-bug-is-narrower.md` (both
superseded by `phase38`), the FP8 reading in the early part of `phase3-weight-format.md`,
the dense-FP16 decode behind `phase4-subnormal-flush.md`, and the shortcut-installing
recipe at the top of `phase55-a-switch-on-a-key.md`. Each says so at the top.
**A withdrawn finding lives on wherever it was written down** — `phase40` found five
places in the *code* still asserting the phase-18 bug as fact, months after the note
retired it.

# DLSS 5 Neural Rendering on Intel Xe2 (Lunar Lake) — the brief

The facts that hold and the rules. The current state, the open work and the traps are in
`notes/HANDOFF.md`, which overrides this file where they disagree; `notes/INDEX.md` maps every
note to the question it settles. The brief as written on 2026-09-07, with its roadmap, its open
questions and every correction made to it in place, is in
`notes/archive/CLAUDE-until-2026-11-04.md`, word for word: older notes that cite
"notes/CLAUDE.md" with a phase number, an open question or a hard constraint mean that file.

## What this project is

NVIDIA's DLSS 5 Neural Rendering (DLSS-NR, `nvngx_dlssnr.dll`) reimplemented to run on an Intel
Xe2 iGPU, whose matrix units have no FP8. Done on 2026-09-10: a frame of Dead or Alive 5 went out
of the running game, through the recovered 71-block graph on this iGPU and back into its
swapchain, correlating 0.981311 with the CPU reference (`notes/phase34-doa5.md`). Games reach it
not through NGX, which needs an NVIDIA GPU, but through a Vulkan layer at `vkQueuePresentKHR`
that hands each presented frame to a daemon holding the model and writes the answer back into
the swapchain: DirectX 8-11 through DXVK, DirectX 12 through VKD3D-Proton on Linux and through
the game's own XeSS on Windows (`src/xess`). Releases are for Windows since 2026-10-09; on Linux
it is built from source (`docs/DEVS.md`).

## Hardware (probed — trust these values)

- Acer Aspire A14-52M: Lunar Lake, Arc 140V-class iGPU (Xe2). Arch Linux with Mesa ANV, and
  Windows with Intel's driver, on the same laptop.
- **15 GiB of RAM, shared with the iGPU**; no VRAM; 7.6 GiB swap.
- Cooperative matrix: subgroup scope, compute stage only, one shape, **M=8 N=16** (K=16 float,
  K=32 int8). Mesa exposes six configs, fp16 and bf16 each into itself or fp32 and int8 into
  int32 (`notes/hw-coopmat.md`); Intel's Windows driver four, with no bf16
  (`notes/phase71-intel-windows-driver.md`). **The path is fp16 × fp16 → fp32**: config 1 on
  Mesa, 0 on Windows.
- Subgroup 32, which every pipeline requires. `cooperativeMatrixRobustBufferAccess` is false on
  Mesa (true on Windows), so the buffers carry the edge padding.
- The matrix units have no FP8, and Xe3 does not add it. TF32, INT4 and INT2 are not reachable
  from Vulkan (TF32 is, through OpenCL). `VK_NV_cooperative_matrix2` gives nothing: no flexible
  configs on Mesa, absent on Windows.

## The weight format

- **145 755 123 parameters** (`notes/phase61-the-parameter-count.md`): the large matrices, 143.0 M
  of them, stored as **FP8 E4M3**, one byte each; the small tensors FP16; `attn_scale` FP32. They
  fill the 147.7 MB `.rsrc` section. Older notes' 73 841 889 is that section's bytes halved.
- The container holds **packed backend payloads**, in `mma` fragment order and partly E4M3. Read
  as dense FP16 they correlate −0.02 with the true tensors. **Use
  `work/mlxw/dlssnr-logical.safetensors`**: 649 named, shaped tensors, matching MLX-DLSS's
  `weight_spec.json` exactly (`notes/phase6-mlx-dlss-unpack.md`; how to make it: `docs/DEVS.md`).
- **No FP8 anywhere in the compute path.** E4M3 is decoded to FP16 at extraction — that is a
  dequantisation step — and computed in FP16 with FP32 accumulate. **Do not convert to BF16**: it
  drops 3 of 10 mantissa bits for a format the hardware likes no better
  (`notes/phase22-staging-and-storage.md`), and Windows has no bf16 at all.
- The decode is byte-identical to a model arena captured from a running RTX 50
  (`skchen17/dlssnr-amd-lab`, `notes/phase3-execution-order.md`): the weights are anchored to real
  hardware; the activations are not.
- **Per-element agreement with a reference is impossible by construction**: the graph is chaotic,
  with ~100 E4M3 publishes at a 6.25 % step between input and output. Judge a change on the
  composed image and the controls (`notes/phase9-numerics.md`, `notes/phase7-first-render.md`).

## The target binary

- `nvngx_dlssnr.dll` 310.8.0.0, 165 840 496 B, sha256
  `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`, from NBA 2K27's early access
  and Authenticode-signed by NVIDIA; driver 616.64, unpacked in full, does not carry it
  (`notes/phase0-acquire.md`).
  In `ref/`, mode 0444.
- 310.9.16.0 (2026-10-16) adds preset B: A's graph with every stage at depth 2, 31 blocks against
  71, with its own weights (`dlssnr-b-logical.safetensors`, `notes/phase72-preset-b.md`). Its
  preset A is 310.8's model byte for byte.
- 89.1 % of the file is weights (`.rsrc`). The CUDA code is 15 Zstandard fatbins of PTX and 15
  cubins in `.data`, for `sm_120`; the imports are only ADVAPI32, KERNEL32, USER32 and VERSION
  (`notes/phase1-binary-map.md`, `notes/phase3-ptx-unlock.md`).
- A one-step pixel-space diffusion model, by NVIDIA's research page. Recovered: a U-Net of 71
  blocks, Swin stages at 32-512 channels and 32 per head around an eight-block ViT bottleneck,
  with full multi-head attention. The graph is MLX-DLSS's (Apache-2.0), aligned with the
  vendor's arithmetic since (`notes/opendlss-reference.md`); `docs/ARCHITECTURE.md` is the
  specification.

## Hard constraints

1. **Memory.** 15 GiB for the whole system. Tiling was never needed: a scratch arena shared by
   roles with disjoint lifetimes, each sized by what a recording touches, holds 720p in 0.7 GiB
   and 1920x1088 in 1.2 GiB, weights included (`notes/phase32-scratch-and-qk.md`; the sizing,
   HANDOFF archive, 09-26). Do not retrofit tiling.
2. **No NVIDIA GPU on this laptop.** Ground truth is our CPU reference (`src/ref`), cross-checked
   against a second implementation that claims the vendor's arithmetic
   (`notes/opendlss-reference.md`). The owner has an RTX 5060 Ti in another machine; a capture
   from it waits until the owner opens it (HANDOFF, Open items).
3. **Cooperative matrix is compute-only**, so everything runs in compute shaders.

## Phases — all done

| phase | done | where |
| --- | --- | --- |
| 0 Acquire the DLL | 09-07 | `phase0-acquire.md` |
| 1 Confirm the weight blob | 09-07 | `phase1-binary-map.md` |
| 2 Recover the graph | 09-07; MLX-DLSS's since | `phase2-graph-rtti.md`, `phase3-architecture.md`, `phase6-mlx-dlss-unpack.md` |
| 3 CPU reference | 09-09 | `src/ref/nr_model.py`, `src/ref/nr_frame.py`; `phase7-first-render.md` |
| 4 GPU path, XMX in FP16 | 09-09 | `src/gpu/nr_xmx.py`; `phase8-xmx-graph.md` |
| 4b Temporal | 09-09 | `src/ref/nr_temporal.py`; `phase12-temporal.md` |
| 4c Residency | 09-09 | `src/gpu/nr_frame_resident.py`; `phase15-residency.md` |
| 5 Integration | 09-10 | `src/layer/`; `phase34-doa5.md`, `phase41-doa6-and-vkd3d.md`, `phase62-mortal-kombat-1.md` |

## Settled — measured, do not re-derive

- **FP32 accumulate.** NVIDIA's kernels accumulate in FP16: none of 218 PTX kernels has an FP32
  accumulator (`notes/phase4-accumulation-choice.md`). The vendor's FP16 accumulator is kept
  behind `-DACC16`: 1.36x on an isolated GEMM and nothing in a frame at phase 31, a verdict
  `notes/phase38-there-was-no-bug.md` says rested on a driver bug that was not there; no later
  note measures it again.
- **OpenCL** reaches DPAS and is slower: 3533 GFLOP/s against Vulkan's 3828 on the same shape, and
  it would be a second backend, not a flag (`notes/phase33-opencl-measured.md`).
- **The detail is drawn at the network's scale.** Half the extent, enlarged after, keeps only 62 %
  of the high-frequency band (`notes/phase37-neural-upstream.md`).
- **Optical flow** does not help live mode's history (`notes/phase54-flicker-fix.md`).
- Measured and not kept: register tiling, operand staging, integer weights, storage width,
  shared-memory bank padding (1.11x, not the textbook 32x), work on the E-cores (−7 %)
  (`notes/phase45-frame-profile.md`, `notes/phase46-cpu-share.md`). Before an optimisation, look
  for it in the `improve-*.md` notes and the HANDOFF archive.

## Repo layout

```
ref/     immutable originals: the DLL and its recorded hashes. NEVER modified, NEVER committed.
work/    working copies, extracted weights, dumps, builds
notes/   findings; INDEX.md maps them, archive/ holds what moved out of HANDOFF and this file
docs/    for readers: ARCHITECTURE.md is the network as a specification, DEVS.md the build
src/     the code
```

## Rules

- **Never commit or redistribute `nvngx_dlssnr.dll` or any weights derived from it.** They are
  NVIDIA's; users supply the DLL themselves. The repo is code only.
- Never modify anything in `ref/`. Copy into `work/` first.
- Distinguish *reported* from *verified* in `notes/`.
- When something contradicts this file, the machine is right and this file is wrong — update it.

*The owner runs Arch Linux and Windows on this laptop, is comfortable at kernel and driver level,
prefers C for low-level work, and does not need concepts explained from scratch. Shortened on
2026-11-04; the full text before that is in `notes/archive/CLAUDE-until-2026-11-04.md`.*

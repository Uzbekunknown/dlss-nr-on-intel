# HANDOFF's release and branch history, and its reference sections 0, 2, 3 and 5

Moved out of `notes/HANDOFF.md` word for word on 2026-11-11, when that file was cut from 96 KB
to 87 KB. The two bullets of "Where things stand" are as they stood on 11-11, edited in place
since 11-04; sections 0, 2, 3 and 5 are as they stood since September, with the corrections made
to them. HANDOFF keeps a short pointer under each section's number, because other files cite them
("HANDOFF section 0", "section 3", "§6"). Sections 1 and 6 are in `HANDOFF-until-2026-10-07.md`.

---

## "Where things stand": the releases and the branches

- **The releases are Windows-only so far**: v0.0.1 (`7f9c364`), v0.0.1.1 (`f9ec5f1`), v0.0.1.2
  (`2f90064`, 10-15), v0.0.1.3 (`d89563f`, 10-18), v0.0.2 (`81b0a59`, 10-31), v0.0.3
  (`86a5799`, 11-01), v0.0.3.1 (`c0b7f9d`, 11-01), v0.0.4 (`8232603`, 11-05) and **v0.0.5**
  (`bb9317b`, 11-06, the latest),
  a ZIP with the setup window, DXVK 3.1.1, both layers, a `vulkan-1.dll` proxy, NR's `libxess.dll`
  as `nr_xess.dll` and, since 0.0.4, its own Python. 0.0.2 brings DirectX 12 through
  XeSS, the in-game panel (PR #20), network B and frames on the GPU by default, and an About tab
  that checks GitHub for a newer release (archive, 10-31). 0.0.3 brings the panel inside DirectX 12
  games, its keys and clicks kept from the game, and `nr_gpu_probe.exe`, the GPU report for Arc
  A-series owners (archive, 11-01). 0.0.3.1 is what the first A-series reports asked for: the speed
  test measures both networks and names each, and setup finds a game's XeSS in a subfolder (archive,
  11-01 later). 0.0.4 brings Python with the release, so a player installs none (below, 11-05),
  and the staged GEMM's walk, which takes 9-19 % off the network on Intel's Windows driver (archive,
  11-04 night). 0.0.5 brings the `hogwarts` profile, Hogwarts Legacy's own tuning, the panel's
  frame counter and OptiScaler's XeSS in setup (below, 11-06, morning). All five went out on the
  owner's word before Linux's `make test` at their heads, as 0.0.1.3 did: 0.0.2's ran green after,
  at `eb9b281`, and 0.0.3's and 0.0.3.1's at `d28e841` (archive, 11-04 later); 0.0.4's and 0.0.5's
  are in Open items. Linux users build from source (README).
- **Branches**, on GitHub since 10-30:
  - `master`, the default: 0.0.1.2 (`2f90064`) until 10-30, then fast-forwarded to `newnr`, and
    with it to 0.0.2 (`81b0a59`, 10-31), 0.0.3 (`86a5799`, 11-01), 0.0.3.1 (`c0b7f9d`, 11-01),
    the README for players (`d5bef2e`, 11-02) and `alchemist`'s head, at `72fb922` and again that
    night (11-04, archive), and with it to 0.0.4 (`8232603`) and its HANDOFF entry (`9b73d96`, 11-05);
  - `newnr`: everything since, with PR #20 merged at `097d6d4` and `dx12` on top of it
    (`cc2b8b7`, 10-30); level with `master`;
  - `media`: the README's pictures;
  - `improve-int8`, kept for INT8 experiments;
  - `alchemist`: the Arc A-series work, from `newnr` at `0a6000f`, on GitHub since 11-01 (archive,
    10-31 evening). Level with `newnr` until 11-02, ahead of it from 11-03 (`911e9ba`, the
    players' data, and the archive's entries of 11-04), level with `master` and `newnr` again on
    11-04, when all three went to its head at the owner's word (archive, 11-04 later), ahead of
    them that evening (`20a0253`, `6ef670e`, `c744581` and the Linux check), level again that
    night, when the owner had both fast-forwarded to it (archive, 11-04 night, later), and again
    since 0.0.4 went out (below, 11-05 later); on 11-06 all three went to the frame counter's
    head, `fps-counter` fast-forwarded in (below, 11-06), then to 0.0.5's, `game-tuning`
    fast-forwarded in (below, 11-06, morning), and again to `game-tuning` with the daemon's fix
    (below, 11-06, later);
  - `cleanup`: from `newnr` at `0456e99`, the shorter brief, `make test` in parallel and the
    leftovers removed; PR #39, run on a GPU on both systems and merged into `newnr` on 11-11
    (`3d933b0`; below, 11-11, latest);
  - ~~`claude/weights-only-quantization-w0o2wh`: one HANDOFF commit (`f06277d`) that reached GitHub
    by accident on 11-01~~: gone from GitHub by 11-04, 08:07 UTC. Its text is carried here (archive,
    11-01, latest), and the branch is saved in a bundle in `D:\NRonWindows\backups` on Windows
    (archive, 11-04).

  The other seven were deleted at the owner's word: `dx12`, `windows`, `improve`, `improve-e4m3`,
  `improve-fusions`, `improve-release` and `experiment-bgra8-pipeline`. All were already in
  `master` or `newnr`, except one merge commit and two of Codex's BGRA8 experiments. A bundle of
  all seven is in `D:\NRonWindows\backups` on Windows. `dx12` went into `newnr` on 10-30, a
  fast-forward.

---

## 0. The one thing to know

**The weight container does not hold plain dense FP16, and almost every dead end in
this project traces to assuming it does.**

`iamwavecut/MLX-DLSS` ships working extraction code for the same DLL. Run on ours it
turns the 153 packed tensors into **649 named, shaped, logical tensors** — none
unsupported, none opaque. The payload is packed into `mma` fragment order and partly
E4M3; our reader was interpreting those bytes as dense FP16.

Proof it matters: `block0.layer0.input_adapter_weight` is `(16, 32)`, exactly the
512-element region we had located at the front of block0. Its logical values have
**correlation −0.02** with our raw FP16 read of the same bytes, and sd **0.2489**
against our 0.0306 — the `1/sqrt(16) = 0.25` that `notes/phase5-stem.md` predicted a
16-input projection should have. Right location, right shape, wrong decode.

So: **start from `work/mlxw/dlssnr-logical.safetensors`, not from
`src/tools/hnet_weights.py`.** Everything we derived from the *kernels* survives;
everything we derived from container *bytes* has to be re-checked against the logical
tensors.

```
git clone --depth 1 https://github.com/iamwavecut/MLX-DLSS work/mlx-dlss
T=work/mlx-dlss/python/mlxdlss/tools
PYTHONPATH=work/shim python3 $T/extract_dlssnr_weights.py work/dl/nvngx_dlssnr.dll work/mlxw/dlssnr-packed.safetensors
PYTHONPATH=work/shim python3 $T/unpack_dlssnr_weights.py work/mlxw/dlssnr-packed.safetensors work/mlxw/dlssnr-logical.safetensors
```

`work/shim/safetensors/` is a 60-line format-compatible shim — this machine has no pip
and the real package needs sudo. Round-trip verified. Both files already exist.

---

## 2. What is solid — measured from the kernels, unaffected by the packing

| Fact | Where |
|---|---|
| Attention **is** softmax: logits hard-clamped (Swin ±6, ViT ±3), `exp` hand-rolled in f16x2 bit arithmetic, **no max subtraction**. Constants exact, verified bit-exact | `notes/phase5-softmax-found.md` |
| The gate multiplies the **skip**, fused into the `mma` C operand: `D = A·B + gate⊙x`; the branch is added unscaled | `notes/phase5-gate-on-skip.md` |
| `attn_scale` is **FP32 per head** (`ld.global.b32` + `cvt.rn.f16.f32`, stride 4, indexed `head = 4·ctaid.z + tid.y`) | `notes/phase5-attn-scale-fp32.md` |
| **head_dim = 32 at every width**, from the QK-norm reduction (4 lanes × 4 squares × 2). Head count = C/32 | `notes/phase5-narrow-blocks.md` |
| Kernel parameter block: `+0` input, `+8` output, `+16` weight arena, `+24/+32` dims. I/O kernels take descriptor tables | `notes/phase5-io-structs.md` |
| **Input is five optional 2D textures**, `tex.2d.v4.f32`; only colour is required | `notes/phase5-input-contract.md` |
| **16-channel packing order**: ch4-6 colour, ch7-9 reprojected history (same affine `(x−a)·b`), ch12-14 sign-encoded validity. MLX-DLSS agrees independently | `notes/phase5-channel-order.md` |
| Output head is **32 → 4**; three channels become display RGB | `notes/phase5-output-head.md` |
| Under **Mesa's default** float controls the XMX multiply flushes subnormal FP16 operands to zero; a per-tensor 2^k rescale guards against it. It is the driver's mode, not the units': Intel's Windows driver keeps them, and since 2026-10-01 libxmx declares `DenormPreserve 16`, so Mesa does too | `notes/phase4-subnormal-flush.md`, `phase71` |
| **Each precision regime has a sharp threshold**: below it a perturbation is annihilated exactly, above it the head jumps to 9-12 % of its sd. float32 ~1e-07, half ~1e-04. The half path is the **more stable** of the two | `notes/phase9-numerics.md` |
| The whole CPU/XMX gap is the FP16 rounding of GEMM *activations*, and **97.3 % of them are already half-valued** — only 186 of 6987 calls are touched. Weights change nothing: 579 of 649 tensors are stored F16, the other 70 are `attn_scale`, not a GEMM operand | `notes/phase9-numerics.md` |
| Batched attention and the folded branched FFN are **bit-identical** to the plain GEMM hook; the two 720p renders are pixel-identical | `notes/phase9-numerics.md` |
| ~~CPU and XMX agree to 9.7e-07 over 4.2M elements~~ — measured on a pass-through with no E4M3 publishes in it; does not transfer | `notes/phase4-end-to-end.md` |

---

## 3. Withdrawn — do not resurrect

Today alone: **eight**. This is the normal rate here; assume the next confident claim is
also wrong until it survives an adversarial test.

- **"The weights are plain FP16, used exactly as stored."** Section 0.
- **"GQA 4:1, Q=C², K=V=C²/4."** `qkv_weight` is `(C, 3C)` — **full MHA, no GQA**. The
  narrow-block Q/K/V split that resisted three methods does not exist.
- **"DLSS-NR does not use softmax."** The `ex2` census was right, the inference wrong;
  `ptx_trace.py` was dropping every `{ … }` scope, which is where the f16x2 exp lives.
- **"3C² means K and V are pre-replicated."** The factor 2 is uniform across all
  matrices — it is the packing, not GQA.
- **"The 128C region is an attention mask."** `max = 0.00` was a rounded print; the
  fraction of exact zeros is 0.0000.
- **"`attn_scale` is stored as a log."** Right symptom, wrong mechanism — it is FP32.
- **"The attention branch helps, 1.39x swing."** Reverses on a structurally different
  image and on two real Cyberpunk frames, where the branch **costs 1.37x**. The swing
  was the branch repairing damage the bilinear scaffolding does to `test_pattern`'s
  fixed-period lines. `notes/phase5-input-dependence.md`
- **"1.02x better than the input."** True only at sigma 0.06 on one synthetic pattern.
  Swept: the network adds a fixed distortion of 0.0163 and removes a constant 6.5 % of
  noise — constancy being the signature of a linear filter, not a denoiser.

Older, still withdrawn: the "missing 2^8.5 factor", the "~450x branch/skip factor" (it
followed from the wrong gate form), the leading-region projection, and "1.01x parity"
(a pass-through).

---

## 5. Layout, commands, repo

```
ref/        the DLL (0444) + sha256      NEVER modify, NEVER commit
work/       weights, PTX modules, mlx-dlss clone, mlxw/*.safetensors, shim, builds
notes/      the findings; INDEX.md maps them, archive/ holds what moved out
src/tools/  PE/resource readers, the container's record reader (hnet_weights),
            and the PTX analysis tools: ptx_trace (dataflow), ptx_addrform
            (address → linear form), ptx_chains (accumulator chains)
src/ref/    nr_model, nr_frame, nr_temporal, nr_display, nr_accel, image_io
src/gpu/    xmxres, nr_resident, nr_frame_resident   <- the device path
src/layer/  nr_layer.c, nr_daemon.py, nr-photo       <- into a game
src/gpu/    gemm_coopmat*.comp, libxmx.c, xmx.py, tests
```

Git repo initialised 2026-09-08, three commits, **code only** — `.gitignore` keeps the
DLL, the weights, the driver payload and the game screenshots out. `/ultrareview` is
ready to run (user-triggered; the model cannot launch it).

Regression, all should exit 0:

```
python3 src/ref/test_nr_model.py                  # primitives, codec, temporal, graph
work/venv/bin/python src/ref/test_against_torch.py # bit-identical to the original
python3 src/gpu/test_resident.py                  # the device path, ~2 min
python3 src/ref/nr_frame.py IN.png OUT.png --gpu  # the visual check
python3 src/ref/nr_frame.py IN.png OUT.png --profile neutral   # the control: ~37x smaller
```

`nr_frame.py` flags: `--gpu --size HxW --profile {standard,neutral,natural,cinematic}
--intensity F --detail-strength F --colour-strength F --frame-index N -v`.

**Controls** (`notes/phase10-controls.md`). Free, post-network — one pass covers the
whole range: `--intensity` (exactly linear, 0 an exact no-op, clamped to [0,1]),
`--detail-strength` / `--colour-strength` (the change is 0.0049 high frequency against
0.0249 low, so `--colour-strength 0` is detail with no tonal shift; past
`--detail-strength 2` it over-sharpens), `--intensity-ladder` renders once and writes
one file per value. Costing a pass each: `--style-index` (a different character, corr
0.50 with style 0 at index 1 — but only 0-8 are sane, 64 gives a magenta cast),
`--local-tone` / `--local-structure` (smooth monotone gains, colour-neutral and safe to
over-drive: tone reaches 1.24x at 2.0, structure peaks near 1.5), `--skin-structure`,
`--auto-mask`, `--control-mask`. Model A/B/C is **not** reproducible — the shipped
weights prove only slot 0.

`nr_xmx.install(fuse_branched=True, exact=False)`. `exact=True` carries activations
half cannot hold as a sum of two halves — more accurate than the reference's own
float32 GEMM — for 16.8 s -> 21.2 s. It moves the head gap from 0.0174 to 0.0124 and
no further; see `notes/phase9-numerics.md` for why zero is not reachable.

The old harness on the wrong weight decode (`src/ref/hnet_*.py`, `forward.py`, `run_frame.py`,
`src/tools/model_spec.py`) was removed on 2026-11-04; the notes that cite it read it at `6d7ad8c`.

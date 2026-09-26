# A reference that claims the vendor's arithmetic: OpenDLSS-NR (2026-09-27)

`maanHimself/OpenDLSS-NR` (MIT, published 2026-09-21) implements the same 71-block network on
NVIDIA Ada in Vulkan with FP8 on the tensor cores, and claims it **bit-exact against captures of the
original**: every one of 75 block boundaries, byte for byte, at 512x512 and up to 3840x2160, and the
head and composed image at eleven sizes. Its `ports/browser-webgpu/` is a second implementation of
the same bytes in WGSL, with no FP8 and no tensor cores — "the exactness is in the specification, not
in the hardware". Neither the captures nor the weights are published, so the claim is theirs; what
can be checked here is that their scalar arithmetic self-test passes on this GPU, and it does.

Where it came from, as far as the record shows: one `init` commit with the whole project, a second
titled "Fix review findings", four commits in all; the author's other repositories are web 3D
(three.js, webgi), and a fork of the Codex CLI from June. It cites neither MLX-DLSS nor this tree
and shares no code with either. "Publish" for an E4M3 rounding point is common vocabulary, from
MLX-DLSS.

## Their specification, against ours

`docs/numerics.md` and `docs/network.md` there. What this tree does differently, each checked in our
code:

- **the padded field.** Theirs aligns each axis to the number of halvings the graph makes, with an
  extra `+ alignWidth` when both axes are multiples of four alignments (a rule they reproduce without
  a reason). Ours rounds to 64 (`nr_frame.network_geometry`). They agree at the live sizes (320x180 ->
  320x320, 448x252 -> 448x320, 576x324 -> 576x384, 1056x594 -> 1088x640) and not at 1280x720
  (1280x768 here, 1344x768 there), 1920x1080 (1920x1088, 1920x1152), 512x512 or 768x768 — where the
  window grid, and so the picture, differ;
- **the GEMMs**: FP8 products summed in groups of 16 as fixed point with 13 fractional bits,
  truncated, onto an f16 accumulator the residual seeds; the ViT's GEMMs split into partitions summed
  in f16. Ours: fp16 x fp16 -> fp32, the residual added after;
- **the softmax denominator**: a fixed tree of half adds, against our sequential float32 sum (numpy's,
  which MLX-DLSS's reference inherits);
- **the ViT**: its own exponential (`0.0895 s + 1.709`, clamp `[1.4395, 1.9775]`, 4-bit shift), queries
  times `sqrt(32)`, unnormalised weights with the reciprocal on the value accumulator, and a padding
  term taken off the denominator. Ours runs the window exponential with the logits clamped to ±3;
- **E4M3 from half**, never from float32 directly; ours rounds float32 straight to E4M3;
- and, not checked here: the history stored truncated to half, and noise from Box-Muller on a hash of
  the padded coordinate.

## Measured: our head against theirs on the same features

`src/tools/opendlss_model.py` writes their model directory from the carved container (the same 153
records, copied as they are); `src/bench/opendlss_reference.py` feeds both networks the daemon's
features for one frame and compares. Their port runs here in headless Chromium on the Arc 140V — 0.55 s
at 320x320, 3 s at 1088x640, against our 23 ms and ~100 — and SwiftShader instead unless Chromium is
told `--ignore-gpu-blocklist --use-angle=vulkan --use-vulkan=native`.

| frame, field | head RGB corr | gate logit corr | composed apart, mean / 99th pct | the pass itself |
| --- | --- | --- | --- | --- |
| Cyberpunk, 320x180 on 320x320 | 0.971-0.976 | 0.86 | 1.13 / 7.2 levels | 4.4 / 4.2 levels |
| DoA5, 320x180 on 320x320 | 0.971-0.976 | 0.65 | 2.74 / 9.6 | 11.2 / 12.5 |
| DoA5, 1056x594 on 1088x640 | 0.988-0.992 | 0.81 | 1.37 / 7.4 | 11.1 / 10.9 |
| Cyberpunk, 1056x594 on 1088x640 | 0.988-0.990 | 0.81 | 1.45 / 8.7 | 8.5 / 7.9 |

For scale, our own graph against itself — the GPU path against the numpy reference, same features,
Cyberpunk at 320x320: RGB corr 0.985-0.994, gate 0.94. So the distance to theirs is about twice what
our two arithmetics make of the same graph, and the gate moves most: its spread differs (sd 0.85 ours,
1.13 theirs, 0.98 our numpy). The pictures look the same side by side; amplified eight times, the
difference is fine texture and a faint tone on the faces, no window seams, no structure.

Not yet measured: where along the graph the distance comes from — their port can capture every block
boundary, which would let our blocks be run on their inputs one at a time — and the two field sizes
that differ, which need both networks run at their own geometry on the same valid frame.

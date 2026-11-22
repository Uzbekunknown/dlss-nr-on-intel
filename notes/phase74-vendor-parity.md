# Phase 74 — NVIDIA's own NR on an RTX 5060 Ti: our A and B against the vendor's (2026-11-21)

*Its activations pass by pass, and where the residual comes from, are in
`phase75-inside-the-network.md`.*

Since 09-07 the brief has said that ground truth here is our CPU reference and that there is no
NVIDIA parity gate: the graph is MLX-DLSS's recovery, checked against its specification and
against behaviour, not against the DLL (`phase7`, HANDOFF section 6). On 2026-11-21 the owner
opened the RTX 5060 Ti machine (Blackwell, sm_120, the DLL's own target) over SSH, and NVIDIA's
DLL now runs there on frames of our choosing. This note is how, what the DLL says about itself,
and the first parity figures. Everything below was measured, unless it says otherwise.

## How NVIDIA's network runs without a game or the NGX core

The machine runs Arch Linux with nvidia-open 610.57.04 (620.73.04 since the evening of 11-21,
below) and has no game with DLSS 5; none is needed. The route is `fernandaspets/dlss5-nr-recipe` (MIT), proved there on an RTX 5090 and an
RTX PRO 6000, which credits `bmitch87/DLSS5VKLayer` (MIT). It was read and reimplemented in C as
`nr_vendor.c` (`work/vendor/`, not in the repository): the DLL is loaded in the NGX core's place
and its own Vulkan exports are called, under Wine (wine-staging 11.15).

- The Linux driver's NGX core for Wine (`_nvngx.dll`, r610) names only `dlss`, `dlssd` and
  `dlssg`, so it would not load `nvngx_dlssnr.dll`. The DLL exports
  `NVSDK_NGX_VULKAN_Init_Ext`, `GetFeatureRequirements`, `CreateFeature1` and
  `EvaluateFeature` itself (feature 18).
- It checks its caller's file name: an import-table hook on `GetModuleFileNameW` answers
  `nvngx.dll`, and one on `GetProcAddress` hides `wine_get_version`, which it asks for three
  times.
- Its kernels are cubins it loads with `vkCreateCuModuleNVX` and launches with
  `vkCmdCuLaunchKernelNVX`, so the VkDevice has `VK_NVX_binary_import` and
  `VK_NVX_image_view_handle`. Wine's winevulkan passes both.
- The parameter block is ours. **The DLL calls it in MSVC's layout of `NVSDK_NGX_Parameter`,
  each overload group in reverse order of declaration:** it read every resource through slot 8
  (`Get(void**)`), the integers through 11 (`int*`) and 12 (`unsigned*`), and every float
  through 14 (`float*`). The harness logs each call with its slot, so every run checks this.
- It finds its GPU only through NvAPI. Without `nvapi64.dll`, creation fails with
  "Unsupported GPU architecture 0x0, minimum required 0x1b0" after `SetGPUArch: Gpu count =
  0`. DXVK 3.1.1's DXGI and dxvk-nvapi 0.9.3 (release tarballs, GitHub's digests checked), with
  `DXVK_ENABLE_NVAPI=1`, answer its `NvAPI_EnumPhysicalGPUs`, `GPU_GetArchInfo` and DRS reads.
- Wine needs a display for its desktop process although nothing is drawn. With none, or with
  Wayland's, `explorer.exe /desktop` never comes up and the first `vkCreateInstance` waits for
  it forever (traced: `get_desktop_window started explorer`). X11 through Xwayland works, and so
  does Xvfb, which `run.sh` starts as a user unit (`:99`) since the evening of 11-21, so that no
  run depends on the desktop session.
- The DLL's own log: `__NGX_LOG_LEVEL=256`, `__NGX_ENABLE_OVERRIDE_LOG_PATH=1` and
  `__NGX_LOG_PATH_OVERRIDE`. The DRS settings the recipe served for it were not needed here.

## What the DLL says about itself (its log)

- **Presets are numbered from 1.** 1 is A,
  `CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_2026_07_04_22_30` (backbone
  crazy-cuckoo); 2 is B, `hnet-crazy-cuckoo-small-fp8`. 0 is "not available in this DLL build"
  and falls back to 1. 310.8 lists one configuration, 310.9.16 two.
- **The network runs at the frame's own size** with `DLSSNR.ScalingRatio` 1.0: 1280x720,
  1600x900 and 1920x1080 as given.
- **Its weight heap is 147 719 680 bytes**, the size of the arena `phase3` decoded and matched
  to skchen17's capture.
- **The knobs' defaults are our `standard` profile.** A frame with intensity, tone, structure,
  style and the automatic mask unset comes out byte for byte as with 1, 1, 1, 0 and 0 set.
- **310.8's A and 310.9.16's A give the same bytes**, as `phase72` found their weights are.
- One run each, from the evaluation's call to the readback: A 5.4-6.0 ms at 1280x720, 7.9 at
  1600x900 and 9.8 at 1920x1080; B 3.3-4.0, 5.0-5.2 and 6.1. Not a timing study. A whole
  process, Wine's start, the DLL's load and the feature's creation with it, takes about 0.5 s.

## Parity

Ours: `src/ref/nr_frame.py --profile standard --intensity 1.0 --resident` on the Arc 140V (Mesa
26.2.7), on the same RGBA8 frame. The vendor's: 310.9.16, `DLSSNR.Hint.Render.Preset` 1 or 2,
history reset, no motion and no depth. Levels of 255. "Change" is the mean |output − input|. The
control is the same comparison against the other network's output from the vendor.

| frames | net | vendor's change | ours | correlation of the changes | mean \|vendor − ours\| (share of its change) | p99 | PSNR vendor–ours | control's correlation |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Dead or Alive 5 Last Round, five 1280x720 frames of its character select (`work/live`) | A | 7.37-7.83 | 7.45-7.94 | 0.989-0.991 | 0.91-1.03 (12-13 %) | 5 | 43.6-45.1 dB | 0.86-0.87 |
| the same | B | 6.61-6.72 | 6.65-6.81 | 0.992-0.993 | 0.74-0.78 (11-12 %) | 4 | 46.5-46.9 dB | 0.86-0.88 |
| Mortal Kombat 1, three 1600x900 frames (`work/mk1cap` 011, 023, 035: a select screen, a close-up, a fight) | A | 7.77-14.53 | 7.74-14.68 | 0.9945-0.9965 | 0.53-1.00 (7-10 %) | 4-5 | 44.4-48.4 dB | 0.92-0.95 |
| the same | B | 7.36-12.64 | 7.31-12.81 | 0.9947-0.9972 | 0.45-0.79 (6-9 %) | 3-4 | 46.9-49.6 dB | 0.92-0.95 |
| Dead or Alive 5 Last Round at 1920x1080, one frame (`work/doa5cap/001`) | A | 10.05 | 10.27 | 0.9946 | 0.93 (9 %) | 5 | 44.9 dB | 0.91 |
| the same | B | 8.47 | 8.58 | 0.9955 | 0.70 (8 %) | 4 | 47.3 dB | 0.91 |
| Hogwarts Legacy, six 1280x720 frames without the interface (the Windows drive's `nr-game/video-1106/shots/clean-off-*`) | A | 6.27-7.07 | 6.31-7.13 | 0.9913-0.9935 | 0.68-0.84 (11-13 %) | 4 | 45.9-47.1 dB | 0.86-0.88 |
| the same | B | 4.38-4.75 | 4.45-4.82 | 0.9907-0.9921 | 0.52-0.58 (11-13 %) | 3 | 48.6-49.3 dB | 0.86-0.88 |
| the same game, three 1280x720 frames with its HUD (`room-off-*`) | A | 8.07-8.24 | 8.12-8.20 | 0.9929-0.9945 | 0.73-0.86 (9-10 %) | 4 | 45.8-47.1 dB | 0.91 |
| the same | B | 6.57-6.67 | 6.55-6.66 | 0.9944-0.9948 | 0.57-0.61 (9 %) | 3 | 48.5-49.0 dB | 0.91 |
| a 1280x720 desktop capture, not a game (`work/gamecap`) | A | 4.96 | 5.49 | 0.9855 | 0.81 (16 %) | 4 | 46.4 dB | 0.74 |
| the same | B | 3.54 | 3.47 | 0.9849 | 0.45 (13 %) | 3 | 50.1 dB | 0.76 |

The rows from DoA5 at 1920x1080 on were run on the evening of 11-21, after the machine had slept:
Hogwarts' 18 runs each a process of its own, all created and evaluated with Success.

**The driver does not change the vendor's output.** After the owner's update to nvidia-open
620.73.04, with wine-staging 11.22 and Xvfb in place of the session's Xwayland, all 19 frames
above were run again with A and B: the 38 outputs are byte for byte those of 610.57.04. The
evaluation's times were as before (one B at 1280x720 took 9.5 ms, the others 3.3-4.0).

On the first DoA5 frame each output is 27.1 dB (the vendor's A) and 26.8 dB (ours) from the
input, and 43.6 dB from the other.

## Reading

- **The recovered graph, its weights and the composition reproduce NVIDIA's picture**, on 18
  frames of three games, Hogwarts Legacy, the game the DLL ships with, among them: 0.989-0.997.
  The residual is a tenth to an eighth of what NR changes, a few levels at the 99th percentile,
  and the two outputs are 43-50 dB apart while each is about 27 dB from its input. The brief's
  warning stands: the graph is chaotic, and per-element agreement is impossible by
  construction. This is agreement of the composed image, which is what the brief says to judge.
- **The gate tells the two networks apart.** Ours against the other network's output from the
  vendor falls to 0.86-0.95 on the games' frames and 0.74-0.76 on the desktop, and on DoA5 the
  residual rises to 51-60 % of the change.
- **The interface costs no agreement.** Hogwarts' three frames with its HUD agree as well as its
  six without (9-10 % of the change against 11-13 %); neither side was given a UI mask.
- **Where NR changes little, the share rises.** On the desktop capture the change is 3.5-5
  levels and the residual 0.45-0.81, as on the games, so 13-16 % of it.
- Ours changes a game's frame by about as much as the vendor's, -3 to +4 %. On the desktop
  capture A changes it 11 % more (5.49 levels against 4.96) and B 2 % less: one frame, not a
  finding. On DoA5's first frame ours takes red down a little more, −3.07 levels on average
  against −2.88.

## The knobs away from their defaults (11-21, evening)

Each setting alone on top of the defaults, presets 1 and 2, on three frames: DoA5 (`live001`),
MK1's close-up (`mk023`, 1600x900) and Hogwarts (`hog_cleanoff1`). Ours ran with
`nr_frame.py`'s matching flags (`work/vendor/knobs.txt`, `knobs.sh`, `compare_knobs.py`). A
knob's effect is a side's output at the setting less its output at the defaults; its correlation
is also given blurred (sigma 12), which takes out the pixel-level residual and leaves where the
knob acts.

| setting (the DLL's; ours) | correlation of the changes | the knob's effect: correlation (blurred) |
| --- | --- | --- |
| `Intensity` 0.5; `--intensity 0.5` | 0.988-0.996 | 0.980-0.994 |
| `LocalToneStrength` 0; `--local-tone 0` | 0.986-0.994 | 0.930-0.985 |
| `LocalToneStrength` 2; `--local-tone 2` | 0.992-0.998 | 0.77-0.975 (0.85-0.99) |
| `LocalStructureStrength` 0; `--local-structure 0` | 0.987-0.998 | 0.977-0.989 |
| `LocalStructureStrength` 2; `--local-structure 2` | 0.974-0.996 | 0.71-0.993 (0.83-0.998) |
| `Style` 1; `--style-index 1` | 0.995-0.998 | 0.988-0.996 |
| `Style` 2; `--style-index 2` | 0.992-0.997 | 0.987-0.991 |
| `UseAutoMask` 1; `--skin-structure 1` | 0.991-0.997 | 0.30-0.61 (0.21-0.71) |
| `UseAutoMask` 1, `SkinStructureStrength` 0; `--skin-structure 0` | 0.990-0.997 | 0.72-0.93 (0.79-0.98) |
| `UseAutoMask` 1, `SkinStructureStrength` 2; `--skin-structure 2` | 0.990-0.997 | 0.38-0.75 (0.46-0.89) |
| Hogwarts Legacy's own (tone 0.5, structure 0.84, the mask, skin 0.27); `--profile hogwarts` | 0.988-0.997 | 0.90-0.98 (0.955-0.994) |

- **The DLL clamps `Intensity` at 1.** It read 1.5 and gave its default's output, byte for byte,
  on all six frames and networks. On the 512x320 crop (`phase75`), 1.001 and 1.5 give 1's bytes
  and 0.99 does not, and 1.5 gives 1's bytes with style 1's grade and with the automatic mask
  too; a control mask is untested. Ours at 1.5 extrapolates past the model's picture, as its
  help says, and its change is 1.5 times the vendor's: that is ours alone, and since 11-21 the
  panel, the setup window, the INI and `docs/SETTINGS.md` say NVIDIA stops at 1.
- **`SkinStructureStrength` acts only with `UseAutoMask`.** Without it, at 2, the DLL's output is
  its default's byte for byte. Our `--skin-structure` turns the mask on by itself. With the mask
  on and the skin strength unset, the DLL hands the pre-block the local structure in its place
  (2 at a structure of 2), as our -1 does.
- **The automatic mask is the same scalars as ours.** The pre-block's arguments carry them, and
  no mask image is bound (`phase75`). Its effect is about one level. The residual between the two
  implementations at a setting is not the residual at the defaults: their correlation is -0.09 to
  +0.15 per pixel and -0.21 to +0.30 blurred, so any change of input reshuffles it. An effect that
  small cannot correlate better, and the large ones do (structure 0, the styles).

## History (11-21, evening)

**Eight consecutive frames, no motion.** DoA5's character select, `work/live` 001-008, 1280x720.
The vendor ran them in one process, the history reset on the first frame only, with zero motion
vectors; ours ran `nr_temporal.py --frames-from`, zero motion (`compare_seq.py`). The history's
effect is a side's sequence output less its single-frame output of the same frame.

| net | correlation of the changes, frame 1 to 8 | residual's share | PSNR | the history's effect, vendor / ours (levels) | its correlation (blurred) |
| --- | --- | --- | --- | --- | --- |
| A | 0.9894 to 0.9953 | 13 to 9 % | 43.6 to 46.9 dB | 1.27 / 1.37 to 2.44 / 2.47 | 0.48 to 0.88 (0.46 to 0.94) |
| B | 0.9925 to 0.9959 | 11 to 8 % | 46.9 to 49.9 dB | 1.34 / 1.39 to 2.26 / 2.35 | 0.72 to 0.90 (0.67 to 0.93) |

- A sequence's first frame is its single frame's output byte for byte, on both sides.
- The two sequences grow closer as the history builds, the residual of each frame being its own
  and the blend averaging them.
- Our blend weighs the history at 0.47-0.55 on average for A and 0.59-0.68 for B, under its
  0.74 ceiling.

**A pan with motion vectors.** A 640x384 window over Hogwarts' `hog_cleanoff1`, moving 6 pixels
a frame for 8 frames. Ours ran `nr_temporal.py --pan 6,0`, whose motion is +6/640 of the width;
the vendor got one vector everywhere (`NRV_MVEC`), with `DLSSNR.MVecScaleX` and `Y` at 1.

| the vendor's vector, against ours | correlation of the changes, frames 1-7 |
| --- | --- |
| (+6, 0), against ours with the motion | A 0.983-0.986, B 0.981-0.986 |
| (-6, 0), the other sign | A 0.91-0.95, B 0.86-0.94 |
| (0, 0), against ours with none | A 0.980-0.989, B 0.972-0.980 |
| (+6, 0), against ours with none | A 0.91-0.97, B 0.88-0.95 |

- **The DLL reads `MVec` as ours reads motion:** in pixels, from a pixel to where it was in the
  previous frame, at a scale of 1.
- The motion's own effect, with it less without, is the same order on both sides: A 1.4-2.0
  levels from the vendor against our 1.1-1.7 (correlation 0.52-0.85), B 0.7-1.3 against 0.8-1.4
  (0.64-0.86). A's is somewhat larger on the vendor's side, and that is not explained.
- Our history weighed 0.69 on average at the last frame with the motion for A and 0.22 for B,
  and 0.01-0.02 with none.

## Not established

- Which arithmetic inside the fused blocks differs. `phase75` found where the residual comes
  from: not the feature assembly and not the composition; each block on the vendor's own input
  lands within 1-3.4 % of it, compounded through the graph.
- History with depth, with a game's own motion (the Hogwarts recordings), and across a scene
  cut; A's motion effect, larger on the vendor's side.
- The control mask and its blend modes, the UI inputs, subrects. A scaling ratio below 1 is not
  reachable in 310.9.16: its evaluation fixes the ratio at 1.0 (`phase76`). The per-pass gate
  this line once proposed is `phase75`.

## Where things are

On the machine, `~/nr-vendor`: `dll/310.8.0.0/` and `dll/310.9.16.0/` (the owner's DLLs, mode
0444, copied there privately; they go nowhere else), `prefix/` (the Wine prefix with the
driver's NGX DLLs, DXVK and dxvk-nvapi), `thirdparty/`, `harness/` (`nr_vendor.c`, its build,
the Vulkan headers), `run.sh`, `run_set.sh` (a list of frames, presets 1 and 2, one process a
frame so that each starts from a reset history, and the parameters to add), `knobs.sh`,
`frames/`, `logs/`. A long set runs as a `systemd-run --user` unit, so that it outlives the SSH
session. Here, `work/vendor/`: `nr_vendor.c`, `run.sh`, `run_set.sh`, `knobs.sh`, `knobs.txt`,
`compare.py`, `compare_knobs.py`, `compare_seq.py` and the frames.

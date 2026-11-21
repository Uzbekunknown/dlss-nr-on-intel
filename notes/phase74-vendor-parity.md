# Phase 74 — NVIDIA's own NR on an RTX 5060 Ti: our A and B against the vendor's (2026-11-21)

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

## Not established

- Where the residual comes from. Candidates, untested: the deterministic noise of the feature
  assembly (MLX-DLSS's), the E4M3 publishes after our float32 accumulation against the
  vendor's float16 (`phase4`, `phase9`), the composition.
- History. Every frame ran alone with the history reset; the harness carries it across frames
  given in sequence.
- The knobs away from their defaults, the masks, UI, subrects, a scaling ratio below 1.
- The vendor's intermediate activations. The harness owns the VkDevice, so
  `vkCmdCuLaunchKernelNVX` could be wrapped and the buffers read between launches: a per-pass
  gate.

## Where things are

On the machine, `~/nr-vendor`: `dll/310.8.0.0/` and `dll/310.9.16.0/` (the owner's DLLs, mode
0444, copied there privately; they go nowhere else), `prefix/` (the Wine prefix with the
driver's NGX DLLs, DXVK and dxvk-nvapi), `thirdparty/`, `harness/` (`nr_vendor.c`, its build,
the Vulkan headers), `run.sh`, `run_set.sh` (a list of frames, presets 1 and 2, one process a
frame so that each starts from a reset history), `frames/`, `logs/`. A long set runs as a
`systemd-run --user` unit, so that it outlives the SSH session. Here, `work/vendor/`:
`nr_vendor.c`, `run.sh`, `run_set.sh`, `compare.py` and the frames.

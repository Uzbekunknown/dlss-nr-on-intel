# Phase 76 — Render scale against NVIDIA's full picture (2026-11-22)

The owner, 11-22, on what the RTX 5060 Ti machine can do for speed: weigh the render scales
players use against NVIDIA's own full picture, and NVIDIA's own lower-resolution mode,
`DLSSNR.ScalingRatio` below 1. NVIDIA's runs were on that machine (driver 620.73.04, 310.9.16's
DLL); ours on this laptop (Mesa 26.2.7).

## NVIDIA has no lower-resolution mode that runs

- **The DLL reads no scaling ratio.** `live001` through network A with `DLSSNR.ScalingRatio` 0.5,
  set before creation and kept through the evaluation, comes out byte for byte as with 1.0, the
  evaluation 6.7 ms either way. The harness logs every parameter the DLL asks for (all eight
  getters), and it asks for none of that name.
- **Its code** (`objdump` of `.text`; RVAs):
  - `DLSSNR.ScalingRatio` is written, never read: by the function at `0x12740`, behind the
    `DLSSNRComputeScalingRatioCallback` the DLL publishes for an application to call. From
    `PerfQualityValue` it gives 1.0 for 2 and 5 and 0.85 for 4, fails on any other value
    (`0xbad00010`, "unsupported PerfQualityValue"), and holds the result to 0.5-1.0. In NGX's
    enumeration 2 is MaxQuality, 4 UltraQuality and 5 DLAA (the SDK's names, not checked here).
  - `CreateFeature` (`0x17b20`) makes the network the requested `DLSSNR.Width` x `Height`, its
    scale starting at 1.0.
  - The evaluation's parameter parser (`0x1a020`) sets the ratio to 1.0 and reads none. It then
    holds it to 0.5-1.0 and flags a smaller network when it is below 0.9999. The evaluation
    (`0x18250`) would then run the network at the output's size times the ratio, and warn that
    the feature should be recreated "at the new scaling ratio for optimal VRAM use". With the
    ratio fixed at 1.0, that path never runs.
- So in 310.9.16 **the network always runs at the output's size**, and what the DLL would offer
  an application is 1.0 or 0.85, never below 0.5. On the RTX 5060 Ti the full size is cheap: A
  takes 5.4-6.7 ms at 1280x720 and 4.1-4.8 at 640x360, B 3.3-3.9 and 2.2 (the evaluation's
  times, `run_set.sh`).

## Our render scale against NVIDIA's full size

`work/vendor/scale_set.py` runs a frame through the daemon's own path, as a game's frame goes
(`process_connection` over a fake socket, B8G8R8A8, profile `standard`, intensity 1, temporal
off), at a list of scales. At 1.0 its outputs are byte for byte `nr_frame.py`'s. The 19 frames of
`set_parity.txt` (DoA5, Mortal Kombat 1, Hogwarts Legacy, a desktop; 1280x720 to 1920x1080),
against the vendor's full-size output (`compare_scales.py`). The change is also split in two bands
by a 3 px Gaussian blur: coarse, and the fine that the blur takes away. Medians over the 19 frames,
the range in brackets:

| | scale | correlation of the change | residual, of the vendor's change | coarse | fine |
| --- | --- | --- | --- | --- | --- |
| A against NVIDIA's A | 1.0 | 0.993 [0.986..0.997] | 10.9 % | 0.996 | 0.960 |
| | 0.75 | 0.931 [0.713..0.967] | 33.5 % | 0.946 | 0.770 |
| | 0.5 | 0.827 [0.710..0.939] | 54.9 % | 0.846 | 0.644 |
| | 0.4 | 0.749 [0.552..0.906] | 68.2 % | 0.781 | 0.466 |
| B against NVIDIA's B | 1.0 | 0.992 [0.985..0.997] | 11.3 % | 0.997 | 0.963 |
| | 0.75 | 0.927 [0.854..0.969] | 41.4 % | 0.947 | 0.755 |
| | 0.5 | 0.789 [0.718..0.951] | 60.4 % | 0.823 | 0.578 |
| | 0.4 | 0.751 [0.670..0.887] | 68.0 % | 0.778 | 0.503 |
| B against NVIDIA's A | 1.0 | 0.875 [0.743..0.952] | 50.9 % | 0.885 | 0.799 |
| | 0.75 | 0.834 [0.592..0.944] | 61.0 % | 0.839 | 0.666 |
| | 0.5 | 0.701 [0.617..0.924] | 71.5 % | 0.730 | 0.523 |
| | 0.4 | 0.673 [0.630..0.861] | 76.1 % | 0.709 | 0.457 |

What each costs on this laptop, for a 1280x720 frame: `frame_profile.py --warm 25 --runs 5` on
the network's field at each scale, two rounds in opposite orders, device ms:

| scale (field) | A | B |
| --- | --- | --- |
| 1.0 (1344x768) | 140.1-140.2 | 73.7-74.3 |
| 0.75 (960x576) | 79.8-81.7 | 41.8-43.3 |
| 0.5 (640x384) | 42.0-44.0 | 21.7-22.4 |
| 0.4 (512x320) | 29.8-31.0 | 15.4-15.6 |

## It is the model, not the upscale

- **The upscale costs about 0.01.** Our own full-size change, taken down to each scale's frame and
  back up with the daemon's own filters (`nr_daemon.resample`: the area mean down, bilinear up),
  correlates with NVIDIA's change at 0.990, 0.986 and 0.982 for A at 0.75, 0.5 and 0.4 (coarse
  0.996 throughout), and at 0.985, 0.978 and 0.972 for B. What a smaller run loses is what the
  network draws: on a smaller frame it draws a different change, coarse band and all.
- **NVIDIA's own network does the same.** Six 1280x720 frames (`live001`, `live005`, `live019`,
  `gc720`, `hog_cleanoff1`, `hog_roomoff1`) were halved with the daemon's area mean to 640x360 and
  run through the DLL. Medians:

  | | A | B |
  | --- | --- | --- |
  | NVIDIA's half-size change, enlarged, against its full-size change | 0.737 (0.698-0.896) | 0.777 (0.767-0.796) |
  | ours at 0.5 against NVIDIA's full size | 0.760 | 0.771 |
  | ours at 0.5 against NVIDIA's half size, enlarged | 0.978 | 0.984 |
  | ours against NVIDIA's at 640x360 (parity at that size) | 0.982 (0.978-0.990) | 0.984 (0.971-0.991) |

- So render scale 0.5 is NVIDIA's network on a half-size frame, faithfully, and that is not
  NVIDIA's full picture. No upscaler can bring a smaller run closer to it, and none is worth
  trying. `phase37`'s "62 % of the high band" was this, seen from our side alone.

## What it means for play

- At the scales players use, 0.4-0.5, the picture differs from NVIDIA's full one by 55-68 % of
  its change, against 11 % at full size; 0.75 already by 33-41 %.
- At a given cost, network B at a higher scale is closer to NVIDIA than A at a lower one. At
  about 42 ms on this laptop, B at 0.75 agrees with NVIDIA's B at 0.927 and with NVIDIA's A at
  0.834; A at 0.5, the same cost, with NVIDIA's A at 0.827. B is NVIDIA's own default in
  310.9.16 (`phase72`).
- `docs/SETTINGS.md` (from `nr_knobs.py`) says a lower scale "draws coarser detail, at about the
  same strength". It draws a different picture. Changing that text is the owner's call.

## Not established

- History at a reduced scale: every frame here ran alone, temporal off.
- The DirectX 12 edit cache ("Faster, experimental": the network at full size on every other frame,
  its answer carried between) against NVIDIA. It needs a sequence.
- What NVIDIA's 0.85 would look like: no build seen runs a smaller network.

## Where things are

`work/vendor/`: `scale_set.py`, `compare_scales.py`, `scale_timing.txt`; `frames/*_ds100_*`,
`_ds075_`, `_ds050_` and `_ds040_` (ours), `frames/*h.rgba`, `*h_v916_{A,B}.rgba` and
`*h_ds100_*` (the half-size runs). On the machine: the half-size frames and outputs, and
`logs/live001_sr10_A.out` and `live001_sr05_A.out` (the ratio's two runs, with the vector trace).
`src/bench/scale_spectrum.py` read the daemon's reply from its end and lost two pixels to the
network-time trailer the daemon writes after its pixels; it reads them from the front now.

# HANDOFF — the state of the work

The working state of DLSS-NR on Intel: what runs, the rules, what is open, the traps, and the
latest changes, newest first. It is for anyone who changes this repository, and for their agents.
`README.md` is for players; `docs/` is for builders and readers (`docs/DEVS.md` the build and the
tests, `docs/ARCHITECTURE.md` the network); `notes/CLAUDE.md` is the brief, the facts that hold;
`notes/INDEX.md` says which note settles what. Where this file and the brief disagree, this file
wins.

Older entries are in `notes/archive/`, by date: `HANDOFF-until-2026-10-07.md` (the graph's
recovery, the kernel work, the experiments that did not pay), `HANDOFF-2026-10-08-to-2026-10-15.md`
(the first Windows releases and the testers' reports), `HANDOFF-2026-10-16-to-2026-10-28.md`
(DirectX 12 through XeSS, preset B, PR #20's review), `HANDOFF-2026-10-30-to-2026-10-31.md`
(DirectX 12 in the release, 0.0.2, the GPU report for Arc A-series),
`HANDOFF-2026-11-01-to-2026-11-04.md` (0.0.3 and 0.0.3.1, the players' data, the staged GEMM's
walk order), `HANDOFF-2026-11-05-to-2026-11-06.md` (0.0.4 and 0.0.5, OptiScaler, the Hogwarts
profile), `HANDOFF-2026-11-08-to-2026-11-19.md` (the E4M3 and by-column weights, PRs #39 to #45,
0.0.6 with FSR 3.1, the Arc A-series switch), `HANDOFF-2026-11-16-to-2026-11-21.md` (the fused
kernels' layout per driver, 0.0.7, NVIDIA's own DLL as a reference), and
`HANDOFF-branches-and-reference-until-2026-11-11.md`. They are history: open one when a line here
points there, or before an optimisation, to see whether it was tried.

Keep this file short: agents load it whole with every request, and `publish_check` refuses it over
200 KB. When the dated entries grow long, the oldest move to a new archive file; a closed open item
is deleted here, and its entry keeps the story.

## Who writes here

- **The maintainers' agents** keep this file. Two of them work on one laptop, one on Linux and one
  on Windows, and hand work to each other through it: an entry per piece of work, newest first,
  ending with **Next on Linux** and **Next on Windows**.
- **A contributor's agent does not edit this file.** Read it for the state, the rules and the
  traps, and keep your own journal in `notes/contrib/<your-github-login>.md`, committed with your
  pull request (`notes/contrib/README.md` has the form). When a pull request is merged, the
  maintainers carry what matters from its journal into this file.

## Rules for every change

- NVIDIA's `nvngx_dlssnr.dll`, and anything taken from it (weights, dumps, captured activations),
  never enters the repository or a release: players supply the DLL. Nothing in `ref/` changes.
- Before a change is done:
  - Linux: `make test`, and again with `XMX_STAGING=1` (the second memory mode); CMake with no
    warning, and CTest.
  - Windows: `tools\build_win.bat` and `src\xess\build_xess.bat` with no warning (MSVC); MinGW
    CMake and CTest; `src/tools/run_tests.py --only cpu,gpu`, whose four Unix-only cases fail
    there.
  - A change on a frame's path keeps the same bytes, or says by how much and why not
    (`src/bench/gpu_frame_stages.py` compares the paths).
  - Say what ran on which GPU, driver and system, and what did not run.
- Timings: a quiet machine, the GPU's clock watched, several processes, the order alternated, and a
  band rather than one figure (section 4).
- Distinguish reported from verified.
- Commit messages and notes say what changed and how it was proven: no personal data, nothing about
  anyone's own machines beyond the GPU, driver and system a result came from, and no session links.
- A release updates the README's GPU and tested-games tables in the same commit.
- Replies in issues and pull requests, PR descriptions and release notes carry no mention of AI.

---

## Where things stand (2026-11-27)

- **It runs on Linux with Mesa and on Windows with Intel's driver**: here on an Arc 140V (Lunar
  Lake), and on players' Arc B- and A-series cards. A Vulkan layer hands each presented frame to a
  daemon that runs NVIDIA's network on the GPU and writes the answer back into the swapchain.
  DirectX 8-11 games go through DXVK. DirectX 12 runs through VKD3D-Proton on Linux, and on Windows
  through a proxy in front of the game's own upscaler, XeSS (`src/xess`, since 0.0.2) or FSR 3.1
  (since 0.0.6), which composes NR's change on the upscaler's output before the interface
  (`mode=after`).
- **Releases are for Windows**: v0.0.1 (`7f9c364`), v0.0.1.1 (`f9ec5f1`), v0.0.1.2 (`2f90064`),
  v0.0.1.3 (`d89563f`), v0.0.2 (`81b0a59`), v0.0.3 (`86a5799`), v0.0.3.1 (`c0b7f9d`), v0.0.4
  (`8232603`), v0.0.5 (`bb9317b`), v0.0.6 (`c473048`) and **v0.0.7** (`921d975`, 11-21, the
  latest): a ZIP with the setup window, DXVK 3.1.1, both layers, a `vulkan-1.dll` proxy, NR's
  `libxess.dll` as `nr_xess.dll`, NR's `amd_fidelityfx_dx12.dll` as `nr_ffx.dll`, and its own
  Python. What each brought is on its release page. Linux has run `make test` green at the code of
  every release, and builds from source there (`docs/DEVS.md`).
- **Branches.** `master` (the default) and `alchemist` hold 0.0.7 and the notes to 11-27
  (`f87f199`). `newnr` is ahead of them, and work lands there first:
  - NVIDIA's own DLL as a reference: parity, the activations between its passes, its knobs and
    history, render scale against its full picture (`phase74`, `phase75`, `phase76`);
  - the intensity's texts (NVIDIA stops at 1), and E4M3 rounding by a magic constant on both
    drivers (`publish.glsl` constant 5, 1-7 % of a network);
  - the GPU image path on by default on Linux, exact on Mesa;
  - the daemon's per-frame cuts, and the layer presenting NR's answer on a semaphore;
  - the GPU path's frame in two submissions, run from C (`xmx_frame_run`);
  - NR in front of OptiScaler v10's custom upscalers, with setup's place for it (#53).

  `media` holds the README's pictures; `improve-int8` keeps the INT8 experiments.
- **Two networks.** A is NVIDIA's 310.8 model. B (310.9.16's `hnet-crazy-cuckoo-small-fp8`) is A's
  graph with every stage at depth 2, 31 blocks against 71, at 0.54-0.58 of A's time (`phase72`).
  The setup window takes B out of a DLL of 310.9.16 or later, and the panel switches the two in
  play.
- **DirectX 12 on Windows** runs in Hogwarts Legacy through its XeSS and its FSR 3.1, and through
  OptiScaler's XeSS (0.9.4). The proxy follows the game's history resets and takes 10-bit and 8-bit
  outputs, and the in-game panel draws there too. No other game has run it yet (Open items).
- **Speed on this laptop.** Network B at 1280x720 and render scale 0.4 takes about 12 ms, the GPU
  path's whole frame 13.4-13.9 (`src/bench/gpu_frame_stages.py`, Windows), and Mortal Kombat 11
  runs at 48-51 frames a second there. Network A at 1920x1080 and 1.0 takes 273 ms on Linux. The
  README's table is `nr_knobs.RATES`, measured with `src/bench/live_rates.py --warmup 3`.
- **On players' GPUs** (`notes/players-20261103.md`): a B580 runs network A at 1280x720 and 0.4 in
  8-9 ms, ~3.9x an A770, which pays ~55 us a pass whatever the field. A-series cards compute NR's
  8x16x16 shape exactly on Windows though they do not offer it; under Mesa an A750 computes it
  wrong (#48). The A-series tiled switch (`XMX_PREFER_TILED`) is slower on all four cards
  reported (two A770s, two A750s), so it stays off.
- **NVIDIA's own output as ground truth** (`notes/phase74-vendor-parity.md`, `phase75`, `phase76`,
  on `newnr`): NVIDIA's DLL runs on an RTX 5060 Ti without a game, on frames we choose; the runner
  (`work/vendor/`) is not in the repository. Our A and B agree with it at 0.989-0.997 correlation
  of the change on 18 frames of three games, and each of our encoder blocks, on the vendor's own
  input, gives its output within 1-3.4 %. The DLL clamps `Intensity` at 1 and always runs its
  network at the output's size: our render scale 0.5 leaves 55-60 % of NVIDIA's full-size change
  unreproduced, against 11 % at 1.0.

## Open items (2026-11-27)

Each line says where its evidence is: "below" is a dated entry in this file, "archive" the archive
under that date. A closed item is deleted; its entry keeps the story.

**Not decided yet**
- A start-up check of NR's matrix shape. Under Mesa an A750 computes the float16 8x16x16 shape
  wrong (#48), and libxmx tries a shape a device does not offer (`shape_unoffered`) without
  checking it, so a game there would get a wrong picture without a word. The proposal: check it at
  start-up, as `xmxres` checks the E4M3 decode, and refuse where it fails (archive, 11-21).
- The render scale's texts for players say a lower scale draws coarser detail; it draws a different
  picture (`phase76`; below, 11-22, later). Whether to say so, and how.
- The speed test runs the CPU path (no `NR_` setting reaches its daemon), while the Windows release
  keeps a game's frames on the GPU, so at 1920x1080 its round trips read 10-20 ms worse than a game
  waits for (archive, 11-03 and 11-08). Changing it moves the window's numbers.
- From `cleanup` (PR #39, merged 11-11): INT8 out of the main line (libxmx, the shader lists, the
  release's file lists), one build system instead of three, and the Configure/Restore Steam
  buttons as a line under Windows (archive, 11-05). With it, the test rules: whether the second
  memory mode runs as `XMX_STAGING=1 make test-gpu`, and CTest on Linux only when CMake changes.
- The steady frame's pipe path in C, so that no Python is left on a frame's path, and a pipe
  connection kept across frames (0.15 ms a frame), which needs a daemon serving several
  connections at once (below, 11-26).
- Judgements in a game, pending since September: whether render scale 0.9 still looks better than
  1.0 for screenshots (archive, 09-27), and whether Mortal Kombat 1 fits at render scale 1.0 now
  that the daemon needs 0.7-1.2 GiB (it was OOM-killed there on 09-16; archive, 09-26).

**Linux**
- `make test` in both memory modes and CTest at `newnr`'s head: the daemon's cuts, the two
  submissions and the C frame changed `nr_daemon.py`, `gpu_image_stats.py`, `gpu_image_pipeline.py`,
  `xmxres.py` and `libxmx.c` (below, 11-26).
- Network B in a game: `NR_PRESET=B` beside A, the look and the frame rate (archive, 10-17).
- B's four full-resolution passes, proposed for the network's speed after the staged GEMM's walk
  (`6ef670e`): not started (archive, 11-04).
- The clock hint (`XMX_LOW_LATENCY`, on by default since 10-18): what it costs in power and heat,
  on battery above all, is unmeasured (archive, 10-18).
- `deploy.sh --release` builds no 32-bit layer, so a 32-bit game under Proton (DoA5) could not use
  a Linux release. Left until a Linux release is planned (archive, 10-07).
- A daemon started by a short-lived process (a launcher that makes a Vulkan instance before the
  game) ends with it, and the game's instance gets none: the layer starts a daemon only in
  `vkCreateInstance`. The same on Windows. Not seen in a game (archive, 10-18).
- Optional: `build_check.py` could hold `build_win.bat` to the same floor (`/DNR_F16C`,
  `/fp:precise`, `/openmp`) (archive, 10-07).

**Windows**
- A DirectX 12 game through the GPU path's two submissions and C frame: only
  `test_gpu_d3d12.py --network` has run it (below, 11-26, night).
- DirectX 12 in other games: only Hogwarts Legacy has run (XeSS 2.0.2.53 and FSR 3.1). The proxy
  takes any XeSS whose exports it carries and reads an older XeSS's execute parameters as 2.0's
  (archive, 10-30). #46: Cyberpunk 2077's XeSS 1.3.0.28 on an A750 never reached the proxy's HELLO,
  cause unknown until its reporter sends `nr-xess.log`. #52: Spider-Man 2's 8-bit XeSS output on an
  A750 is not the finished picture, so NR's change turns its shadows green; the fix (composing at
  the present, or reading that output as linear) is not chosen.
- How the proxy reads a 10-bit output (R10G10B10A2 as linear light, `60a7312`) and an 8-bit one
  (display-encoded, even where FSR 3.1 was told its colour is linear): no game has confirmed
  either (archive, 11-20).
- OptiScaler (#38): NR runs in Hogwarts with OptiScaler 0.9.4, from the game's XeSS and from its
  FSR 3, and setup takes OptiScaler's XeSS (`d936d63`, in 0.0.5). Untested: DLSS input through its
  spoofing, its DirectX 11 path, a game with no XeSS of its own, the cut beside its own D3D12 hooks,
  #38's Stellar Blade. OptiScaler v10's custom upscaler folders (`custom/<name>/ffx`, #53): NR's DLL
  for them and setup's place for it (below, 11-27 and 11-28) ran in Hogwarts
  behind AMD's FSR upscaler standing in. Untested: XMX-SR itself, FSR 4 there, and what
  OptiScaler's FSR 4 preset and model hooks miss now that they scan NR's DLL.
- FSR 3.1 without OptiScaler: a game with FSR 3.1 and no XeSS, FidelityFX SDK 2.0's loader, FSR's
  own frame generation, and whether setup should offer the choice of upscaler where a game has
  both (archive, 11-12).
- #49's DLL, 310.8.2.0 (a B580 in Tomb Raider 2013), is not in `TESTED_DLLS`, and its network is
  not compared with 310.8.0.0's. Tomb Raider (2013) goes into the README's tested games at the
  next release; its row is in the archive (11-21).
- The bottleneck's weights as E4M3 bytes and the weights by column, on by default on Intel's
  Windows driver: no desktop Arc card or B580 has run them (archive, 11-12).
- The GPU's clock: Intel's Windows driver clocks the bench's GPU-only modes down in some periods
  and holds it up under a game's load (below, 11-22, afternoon). No per-process hint exists, and a
  second process keeping the GPU busy does not help (archive, 11-08).
- The staged GEMM's loop on Intel's compiler takes 1.2-1.3x Mesa's time where the graph is large;
  left: the residual at K = 64, the QKV gather and the bottleneck's contract (archive, 11-04).
- MSVC does not vectorise the composition (0.5-1.8 ms behind gcc): it vectorises innermost loops
  only, and each pixel loop holds a three-channel one (archive, 10-02 and 10-07).
- A Vulkan or DirectX 8-11 game that runs elevated, or any with UAC off, never gets the layer; the
  HKLM manifest that would reach it is kept for later (archive, 10-08 and 10-09).
- `windows_launch.py`'s Configure Steam and Restore Steam can go, with their tests, once no release
  in players' hands has those buttons; the GitHub releases have none (archive, 10-09).
- The Vulkan panel under validation: one warning left, `menu.comp`'s `rgba8` against a B8G8R8A8
  swapchain (archive, 11-06).
- The panel open over a game that reads keys by `GetAsyncKeyState`, DirectInput or a gamepad: the
  game still gets them (archive, 10-31).

**NVIDIA's DLL as a reference** (`phase74`, `phase75`, on `newnr`)
- Left: history with depth and a game's own motion (the Hogwarts recordings), network B's taps,
  and the blocks after the encoder one by one.
- `docs/ARCHITECTURE.md`'s controls section says the vendor extrapolates above an intensity of 1
  when a grade or a mask runs its pass (`phase70`). Measured, it does not with style 1's grade nor
  with the automatic mask (`phase74`); a control mask is untested. The same file says preset B was
  not compared with NVIDIA's. Both are to be corrected, later.
- Whether the runner and its tools (`work/vendor/`: `nr_vendor.c`, `run.sh`, `run_set.sh`,
  `compare.py`) go into the repository: later.

**Planned, not started** (do not start unasked)
- A FAQ in the README.
- INT8 on the bottleneck: the exact kernel exists (`gemm_staged_int8.comp`) and stays out of the
  graph; `improve-int8` keeps the branch (archive, 09-26 and 11-11). #54 measured an int8 kernel at
  the A-series' own 8x8x32 tile: 2.7x on the bottleneck's 64x1024x4096, the kernel alone
  (reported).
- The asynchronous live mode (`NR_LAYER_ASYNC=1`) stays off: a frame of latency, and 6-9 frames a
  second behind in Mortal Kombat 11 (archive, 09-25 and 11-11).
- The DLSS-SR (upscaler) reading, dropped on 09-22.
- llama.cpp issue #13530 keeps cooperative matrices off for every Intel GPU, and this project has
  an Xe2 iGPU's measurements to post there. Put off.
- The Mesa issue on shared-memory sizing: Mesa rounds a workgroup's declaration up, so some smaller
  declarations run on fewer threads (still so in Mesa 26.2.4 and in `main` on 10-14). It costs
  this frame nothing. The draft and its patch exist; not filed (`notes/improve-shared-memory.md`).
- An Arc A-series path with 8x8 matrix tiles, from OUCO86's A770 port: paused (archive, 11-01).

**Leads left untried** (each small; the estimates are in the archive)
- Reading the colour and the previous frame as the bytes they arrive as: ~35 MB less memory
  traffic a 1080p frame (archive, 09-26).
- The narrow blocks' fused feed-forward reading its input once: ~2 % at 720p (archive, 09-26).
- A small-M kernel without the shared-memory round trip, for the bottleneck at small fields
  (archive, 09-25); Xe2's 256-register mode per dispatch, ~6 % at best (archive, 09-27);
  `cl_intel_subgroup_2d_block_io` (archive, "1. Do this first").
- Taking each load out in turn, as window attention's cost was found, for every other kernel that
  runs one subgroup to a workgroup (archive, 09-25).
- A narrower subgroup on A-series cards: the raw 8x16x16 shape is 1.4-1.7x faster at 8 or 16 lanes
  on an A770, but #54's 16-lane staged GEMM on an A750 was no faster (B at 720p 15.2 against 15.0
  ms, reported): the staged kernel is bound elsewhere.

---

## NR in front of an OptiScaler v10 custom upscaler in a game: Hogwarts Legacy (2026-11-28, branch `newnr`)

The DLL (`d80bb9b`, 11-27 below) and setup's place for it (`881e63b`, 11-28 below) in one release
built from them, run in Hogwarts Legacy on the Arc 140V (Intel 101.9150). XMX-SR is not
among our files, so AMD's own upscaler stands in for it: the `amd_fidelityfx_upscaler_dx12.dll`
4.1.2.4555 that OptiScaler's build bundles, copied in as a custom upscaler named `standin`.

- **Set up as a player would:** OptiScaler master (`91d2de3`, built locally, 10.0.0.1) as
  `dxgi.dll` with its folder, `Dx12Upscaler=standin`, GPU spoofing off; the stand-in in
  `OptiScaler\custom\standin\ffx`. Then setup's Check and Install NR through the release's bridge
  (API d3d12, upscaler auto). Check said "OptiScaler's custom upscaler standin in <folder>: NR runs
  after it"; Install put `nr_ffx.dll` there under the stand-in's name, the stand-in beside it as
  `amd_fidelityfx_upscaler_dx12_nr_orig.dll`, and `nr-xess.txt`. The game upscales with its own
  FSR 3 Quality (1281x721 -> 1920x1080), whose calls OptiScaler takes.
- **In the game:** OptiScaler loaded NR's DLL from the custom folder when FSR 3 came on ("Loaded
  custom standin"). NR's log: "NR in front of standin, OptiScaler's custom upscaler", the upscale
  context (render up to 1920x1080, flags HDR and vectors at the render's resolution), the dispatch,
  its daemon started, the cut hooked. `OptiPluginInit` was handed on; the stand-in has none, and
  OptiScaler logged the plugin API as declined. Then **996 frames in 45 s, each answered on its own
  frame, 0 declined, 0 errors**: about 22 frames a second, 44 ms from the copy to the answer, on the
  GPU path (`gpu-shared-d3d12`, preset B, network 768x448 at scale 0.4). The picture looked like the
  game's, with no artefact seen (a screenshot; not compared with NR off).
- **OptiScaler's one error:** "Couldn't find FSR 4 preset selection in
  amd_fidelityfx_upscaler_dx12.dll". Its FSR 4 preset hook now scans NR's DLL (the known limit,
  11-27), so with FSR 4 or XMX-SR behind NR, OptiScaler's FSR 4 preset and model choices would not
  reach it.
- **Remove NR** gave the stand-in its name back. The OptiScaler tree was then as before Install NR,
  paths and SHA-256; with OptiScaler taken out, the game's root, executable, XeSS and DLSS folders
  were as before the test.
- **The game's upscaler:** Hogwarts' default upscale type is AMD FSR 1.0 (`UpscaleMode=AMD_FSR_Quality`
  with `bIsUpscaleModeModified=False`), which calls neither FSR 3.1 nor XeSS: a proxy sees no frame
  there. FSR 3 written into `GameUserSettings.ini` before the start did not hold, because the game
  wrote its default back as it started. Chosen in its display options, FSR 3 Quality held.
- **Not run:** XMX-SR, FSR 4 behind NR, OptiScaler's DLSS or XeSS inputs into the custom upscaler,
  another game, the setup window itself (the bridge ran its steps).

**Next:** the #53 follow-up with this run, then a release.

## Setup puts NR in front of an OptiScaler v10 custom upscaler (2026-11-28, branch `newnr`)

The setup half of #53's XMX-SR; NR's DLL for it is the 11-27 entry below. Until now setup refused
any `Dx12Upscaler` but `auto` and `xess`. Committed as `881e63b`. Read in OptiScaler `91d2de3`
(GPL-3.0, nothing copied):
`CustomProviders.h`, `Config.cpp`, `dllmain.cpp`.

- **Where.** OptiScaler's own folder as `optiscaler()` had it for XeSS (OptiScaler's
  `MainDllPath`: `OptiDllPath`, else `<exe>\OptiScaler`, else the executable's), then
  `custom\<name>\ffx` when `Dx12Upscaler=<name>` is none of OptiScaler's own upscaler codes
  (`OPTISCALER_UPSCALERS`; its ini reader takes those first). Folder names in any case, found by
  listing, as OptiScaler and Windows find them and as the suite needs on Linux. The DLL is the one
  OptiScaler takes: `amd_fidelityfx_upscaler_dx12.dll`, else the first `.dll` the folder lists.
- **What.** `_d3d12_target` gives an FSR-kind target there, with `"custom": <name>`:
  - NR's `nr_ffx.dll` under the DLL's name, and the upscaler's DLL beside it under
    `ffx_original`'s, which `nr_ffx.c` loads;
  - `nr-xess.txt` with `env=` beside them;
  - the upscaler's other files left as they are.

  "auto" takes it before OptiScaler's XeSS, "fsr" takes it, and "xess" still asks for
  `Dx12Upscaler=xess`. The FidelityFX checks apply, worded for it: every export of the upscaler's
  DLL in NR's, `ffxDispatch` present, NR's own with no record refused. 0.0.7's `nr_ffx.dll` has no
  `OptiPluginInit`, so Check refuses a custom DLL that exports it; the DLL of 11-27 carries it.
- **Remove NR** knows the custom DLL by its `_nr_orig` copy in the record (`_upscaler_names`; its
  name may be any). It puts the DLL back and takes `nr-xess.log`, and it leaves a new copy of the
  upscaler written over NR's, as for a game update.
- **Check** says "OptiScaler's custom upscaler <name> in <folder>: NR runs after it", with the
  DLL's two names. The window says the same after Check and Install, in four languages
  (`Warn.optiscaler_custom`).
- **Check refuses:**
  - a custom name with no DLL where OptiScaler looks, naming that folder;
  - OptiScaler set to another upscaler after Install NR (custom -> XeSS, XeSS -> custom, or the
    game's own FSR 3.1 -> custom), where NR would get no frame: set it back, or Remove NR, then
    Install NR;
  - a built-in non-XeSS upscaler, as before; its message names the custom folder too.
- `docs/RELEASE-QUICKSTART.md`: a paragraph on custom upscalers.
- **Checked** (Windows 11, native Python 3.12):
  - `test_windows_wizard` 112, 4 of them new, 1 skipped as before. Against `newnr`'s core, given
    only the new constant, the four fail on behaviour.
  - `test_windows_setup` 14, `test_windows_wizard_bridge` 7, `test_release` 22 (5 skipped),
    `test_windows_controls` 14, `test_windows_launch` 43, `test_setup_proxy` 11.
  - The window's self-test: 105 controls, 218 keys. With the Chinese string taken out it fails.
- **Not run** then: a game, OptiScaler, a release built with this (since run: the entry above);
  XMX-SR (never downloaded); the suite on Linux.
- **Known limit:** on a FAT or exFAT drive a folder lists its files in no fixed order. There a
  custom DLL with a name other than the standard one could be listed after its `_nr_orig` copy,
  and OptiScaler would take the copy, past NR.

**Next on Linux:** `make test` runs the new cases (`test_windows_wizard`).

## NR in front of an OptiScaler v10 custom FidelityFX upscaler, for #53's XMX-SR (2026-11-27, branch `newnr`)

Our #53 reply of 11-27 (issuecomment-7210289838) promised to try NR behind XMX-SR. XMX-SR installs
as an OptiScaler v10 custom upscaler (reported): `OptiScaler\custom\xmxsr\ffx\amd_fidelityfx_upscaler_dx12.dll`,
with its own `fsr-original.dll` and `nvngx_dlss.dll` beside it, and `Dx12Upscaler = xmxsr`.

- **What OptiScaler v10 does with `custom\<name>\ffx`** (master `a1e0e19` read on GitHub, GPL-3.0,
  nothing copied; a local build of `91d2de3` has the same loader, hooks and FidelityFX feature):
  - the root is `custom` beside OptiScaler's own files (`<exe>\OptiScaler` in v10), scanned once;
    any DLL name, and with several DLLs in the folder `amd_fidelityfx_upscaler_dx12.dll` wins, else
    the first listed;
  - loaded when that upscaler is first used, after OptiScaler's own FidelityFX DLLs, by `LdrLoadDll`
    on its full path with no flags, past OptiScaler's own hooks;
  - `ffxCreateContext` and `ffxQuery` are required, the other three taken. `OptiPluginInit`
    (`plugin_sdk`, MIT, API version 3) is called once on the DLL it loaded, its settings in
    `[Plugin.<folder>]`;
  - no SDK version check of the DLL: `ffxQuery(NULL, GET_VERSIONS)` has to list one. The context
    is made from the chain UPSCALE, BACKEND_DX12, OVERRIDE_VERSION (the version that
    FfxUpscalerIndex picks for the name). The dispatch gets the game's command list, inputs in
    COMPUTE_READ, the output in UNORDERED_ACCESS, the upscale size set, and NON_LINEAR_COLOR_SRGB
    or PQ in its flags when FsrNonLinearSRGB or FsrNonLinearPQ is set;
  - after the load, its FSR 4 preset and model hooks scan that DLL for AMD's FSR 4 provider
    classes.
- **The trap: OptiScaler's LoadLibrary and LdrLoadDll hooks take any DLL whose file name ends in a
  FidelityFX SDK name**, the upscaler's among them, from any folder. They hand back OptiScaler's own
  copy, or load the file and detour its five exports as the game's. So a DLL in a folder of its own
  cannot chain to `custom\xmxsr\ffx\amd_fidelityfx_upscaler_dx12.dll` by that name: it would get
  AMD's upscaler, or XMX-SR hooked with its calls coming back into OptiScaler. XMX-SR itself, as
  DDXDB describes it, takes AMD's DLL as `fsr-original.dll` (reported, its source not public).
  `dlssg-to-xefg-xmx` (GPL), a custom FG DLL, loads `libxess_fg.dll` plainly but calls only XeFG's
  unexported functions.
- **So NR takes the standard name in the custom upscaler's folder**, and the upscaler moves aside in
  that folder: `custom\xmxsr\ffx\amd_fidelityfx_upscaler_dx12.dll` is `nr_ffx.dll`, XMX-SR's is
  `amd_fidelityfx_upscaler_dx12_nr_orig.dll`, and its own files stay where they were. `nr_ffx.c`
  already loads `<self>_nr_orig.dll` by its full path, a name the hooks pass by. No setting for a
  DLL in another folder.
- **Built** (`d80bb9b`):
  - `OptiPluginInit` exported and handed on to the upscaler's own, so its tab and `[Plugin.xmxsr]`
    stay;
  - the dispatch's sRGB flag counts as the context's non-linear flag (an 8-bit output is still a
    display picture, anything wider is refused), and PQ is refused;
  - the log names the custom upscaler ("NR in front of xmxsr, OptiScaler's custom upscaler (...)"),
    and, when the upscaler's own DLL is missing, says what goes beside NR's.
- **Checked:**
  - `build_xess.bat` with no warning;
  - `test_nr_xess` all passed, with new cases:
    - an 8-bit output with sRGB at the dispatch;
    - PQ, and sRGB above 8 bits, handed on whole;
    - `custom_folder` on seven paths;
    - the built DLL loaded as OptiScaler loads one, in front of the new stand-in
      `test_ffx_original.dll`. Each call reaches the stand-in with OptiScaler's own descriptions,
      the versions come back, `OptiPluginInit` is handed on, and the stand-in finds its own file
      beside it. Alone in a folder, NR's DLL refuses to load (1114) and logs why.
  - Against the old code:
    - `newnr`'s DLL fails the load case, having no `OptiPluginInit`;
    - with the flags' checks taken out, PQ and sRGB above 8 bits get a cut.
  - `test_xess_panel` passes.
- **Known limits:**
  - NR in two custom folders in one process: the second stays passive;
  - NR's own `libxess.dll` in OptiScaler's folder: if OptiScaler loads it first, the custom
    upscaler's NR stays passive;
  - OptiScaler's FSR 4 preset and model hooks now scan NR's DLL, so if XMX-SR carries AMD's FSR 4
    provider classes, those choices no longer reach it. Unknown without its source.
- Setup's place for it is the 11-28 entry above; the game run, the entry above that.

**Next on Linux:** nothing: `src/xess` builds only on Windows.

## The GPU's idle inside NR's frame; the frame in two submissions, then in C (2026-11-26, night, branch `newnr`)

Why: in a GPU-bound frame only the GPU's idle time can go, and Python on a frame's path behaves
differently from one machine to another, so the frame moves to C (C and not Rust: all native code
here is C, and Rust would not orchestrate faster). Arc 140V, Intel 101.9150, network B.
`9d0fc73`, on `newnr` since 11-27 with the daemon's cuts (below).

- **Where the GPU stood idle.** Measured with a GPU timestamp at the start and the end of every
  libxmx submission, and the host's clock at each submit and wake-up (measurement only, not
  committed).
  - Light client, 896x512 field: 1.33-1.37 ms of NR's frame sat between its six submissions.
    0.50-0.53 of that was the daemon's Python; 0.82-0.84 was the wake-up after each fence and the
    next submission's start, ~165 us a round trip.
  - Mortal Kombat 11, 1280x720 at 0.4: a 20.7 ms frame = the game's GPU work ~5.9 + NR's 13.0
    (network 12.0, decode 0.12, prep 0.34, compose 0.49) + idle 1.9. 1.36 of the idle was inside
    NR's frame (Python 0.55, latency 0.75). The render engine was 90.9 % busy.
  - So in a GPU-bound frame what can go is the round trips. The CPU's time before the acquire
    already hides behind the game's GPU work (daemon-cuts, below).
- **Two submissions a frame.** New in libxmx: `xmx_batch_cut`/`_graph`/`_submit`,
  `xmx_rec_external`, `xmx_rec_timestamp`. Also `GPUImagePipeline.process_device(own=)` and
  `nr_gpu_transport._Ownership`, for Vulkan and D3D12. `NR_ONE_SUBMIT=0` keeps six.
  - The first submission, behind the client's semaphore: the acquire, the letterbox check, the
    decode and the cut test.
  - The second, which signals the client: the history, prep, the network's captured graph, the
    composition and the release.
  - Idle inside NR's frame drops to 0.32-0.56 ms, all in the one gap left: Python 160-185 us plus
    the latency.
- **The frame in C**: `xmx_frame_run`, `xmx_frame_second`. `NR_FRAME_C=0` keeps the Python-joined
  frame. The log line ends `joined`, or `joined c`.
  - The image passes are captured as graphs, by parity and by history kept or dropped
    (`_FrameGraphs`). libxmx runs the frame from the acquire to the release: both submissions,
    the bars' check, the cut test's decision and the head's check.
  - Python captures the graphs when the settings change and keeps the history's state.
  - It gets the frame back when the bars moved (status 1), or when only the cut's float64 sum can
    decide (status 3).
  - Idle inside NR's frame: 0.16-0.17 ms. The one gap is 5 us of host work and ~160 us of the
    OS's and driver's wake-up and submission. Only one submission a frame could take that, with
    the cut's decision made on the GPU.
- **The same bytes**:
  - `gpu_frame_stages.py`'s five paths (CPU, host, six, two, C) are exact at 1280x720@0.4 and
    1762x992@0.5, also with the change metric on and `cinematic`'s grade.
  - Six, two and C give the same bytes frame by frame (a check script, not in the tree) with:
    - the temporal off;
    - a cut limit that drops the history on every frame;
    - one that drops it every fourth frame, where C decided as Python did on each.
    - black bars that change height (another such script). C handed each such frame back to Python
      (status 1), which ran it as before; the crop's change drops the history in all three.
  - The change and cut values C reads are Python's to the last bit.
  - Frames through the layer under sync validation: 0 errors on the game's side
    (`--sync-validation`, the semaphore and fence protocols) and on the daemon's
    (`NR_TEST_DAEMON_VALIDATION=1` in a helper not in the tree).
  - The daemon's validation found one bug in the C frame, now fixed. A session's shared buffers,
    re-imported after a resize, took the freed buffers' slot ids, so graphs keyed by ids ran
    against a destroyed VkBuffer. They are keyed by the buffer objects themselves now
    (`_FrameGraphs.holds`).
- **Timed** (light client: quiet before and after; MK11: alternating runs):

  | | six | two, Python | C |
  | --- | --- | --- | --- |
  | bench 1280x720@0.4, device ms, 3 processes | 14.32-14.52 | 13.65-13.92 | 13.43-13.92 |
  | bench 1762x992@0.5 | 32.61-32.72 | 32.21-32.27 | 31.85-32.09 |
  | light client, request, 3 rounds | 32.52 | 32.01 | 31.82 |
  | MK11 fps | 49.13, 49.72 / 49.62, 49.93, 47.98, 48.49, 49.96 | 50.73, 50.24 | 51.23, 50.35, 51.43, 49.87, 51.68 |

  - MK11 with two: 2 of 2 pairs above six (+2.1 %).
  - With C: 5 of 5 pairs above six, three of them run C first; the median 51.23 against 49.62,
    +3.2 %. C's own 0.2 ms over two is near a game's noise from run to run.
  - Each C run sent ~90 frames in a row back to Python's own path, prep ~4 ms there: the bars
    likely moving through a round's intro. Those frames ran six submissions; a fallback that kept
    them joined is not built.
- **Checked:** MSVC `build_win.bat` with no warning; MinGW CMake with no warning in `libxmx.c`
  and CTest 72/72; `run_tests.py --only cpu,gpu` 67/71 (the four Linux-only failing);
  `test_gpu_transport` 58, 7 of them new for the hand-over; `test_gpu_d3d12.py --network`, whose
  frames run in C. Again on 11-27 at the commit: a fresh MinGW build (its 198 warnings as at 0.0.7,
  none in `libxmx.c`) and CTest 72/72, which is also the CTest the daemon's cuts owed (below); the
  five edge cases the same bytes, 15 of 16 frames in C with the temporal off.
- **Not run:** a DirectX 12 game; a B580; Linux.
- Level Zero's busy share for the ALL engine group is this driver's average over seven engines, a
  seventh of the real share: read RENDER_ALL.

**Next on Windows:** a DirectX 12 game through this path; the steady frame's pipe path in C
(proposed). **Next on Linux:** `make test` in both memory modes and CTest. Changed: `libxmx.c`, `xmxres.py`, `gpu_image_pipeline.py`,
`nr_frame_resident.py`, `nr_gpu_transport.py`, `nr_daemon.py`, `gpu_frame_stages.py`. The
transport is Windows-only, but `gpu_frame_stages.py` runs the joined and C paths on Linux too.

## What a desktop card's frame holds besides the network; 0.9 ms of CPU and waits off it, which a GPU-bound game does not see (2026-11-26, branch `newnr`)

#51 (a B580 in the Bloodborne PC recompile, native Vulkan: network B at 896x512 in 9 ms, 21 ms a
frame in the daemon) raised the question whether speed-ups dropped for a small gain on this iGPU
matter on desktop cards. Arc 140V, Intel 101.9150. `c07391d` and `dc793be`, on `newnr` since
11-27.

- **The daemon's frame time holds the game's own GPU frame.** `total_seconds` starts before
  `xmx_external_acquire`, which waits on the CPU for the game's `ready` semaphore (with
  sync=semaphore, and in D3D12): #51's 9.3 ms beside the network, prep and compose are the game's
  rendering, which runs in series with NR. Hogwarts' D3D12 frames show 20-33 ms of it here.
- **Per step on the light client** (test_gpu_present with `--no-validation`; B with a 896x512 field
  at 1280x720, 1762x992 and 2560x1440, 4 processes each; the marks were in a scratch worktree, not
  committed):
  - the network 29.7-31.0 ms;
  - decode and cut 0.54-0.79, and prep 1.05-1.60, both flat with the output's size;
  - compose 0.91-1.04 / 1.24-1.58 / 2.12-2.45, about 0.5 ms fixed and 0.5 a megapixel;
  - the daemon's CPU 1.1-1.6 ms: the settings file read every frame 0.22, the letterbox check's
    own submit 0.25, recording the image passes 0.35-0.45;
  - the layer and the pipe between frames 1.2-1.75, the copy back's CPU wait 0.26-0.68 of it.

  On a B580, prep and compose (1.6 and 1.2 ms in #51) are hardly faster than here: fixed costs. So
  besides the network, the rest is about 17 % of a frame here and about 40 % of NR's own time on a
  B580.
- **Cut on this branch**, each in its own commit: the settings file read only when its stamp could
  hide a write (`c07391d`); the letterbox check inside the decode's submit (`c07391d`); the copy
  back presented on a semaphore (`dc793be`). Together, timed with the GPU's clock steady, the whole
  request went from 35.94 to 35.01 ms at 1762x992 (6 of 6 rounds). That is CPU time and waits, and
  it shows where the GPU has room, as on the light client.
- **In a game it does not show** (Mortal Kombat 11, release roots built the same way from `newnr`
  and from this branch, B, GPU path, 1280x720 at 0.4, 40 s a run, alternating):
  - frames a second 48.25 and 47.67 with the branch against 50.14 and 48.13 with `newnr`, which
    moves 4 % between its own two runs;
  - prep 1.15-1.16 against 1.21-1.25 ms;
  - the rest of the daemon's frame, its wait for the game's own GPU work, 4.33-4.85 against
    3.32-4.13.

  So on this iGPU the game's GPU work and NR's fill the frame, and the CPU time freed only moves
  into waiting. A frame that is GPU-bound gains only from less GPU work or fewer idle gaps on the
  GPU; #51's B580 frame (the game's ~9 ms beside NR's ~12) may well be one. All frames
  `gpu-shared` with `sync=semaphore`, no refusal, the game folder as before after each run.
- **Measured, not built:** a pipe connection kept open across frames, 338-388 -> 210-232 us a
  request (`nr_pipe`, 30 ms apart), about 0.15 ms a frame. The daemon serves one connection at a
  time, so a held one would lock out a second client. Not decided.
- **Left for desktop cards:** the image passes, ~2.9 ms of a B580's frame, mostly fixed costs.

**Not run:** a B580, Linux (CTest on Windows ran on 11-27, above). **Next on Linux:** `make test`
in both memory modes and CTest: `nr_daemon.py`, `gpu_image_stats.py` and `gpu_image_pipeline.py` changed (the layer's
change is Windows-only). **Next on Windows:** nothing for this.

## The GPU path's slow state at 1280x720 on Windows is the driver's own clock choice (2026-11-22, afternoon, branch `newnr`)

`gpu_frame_stages.py`, B at 1280x720 and 0.4, Intel 101.9150, quiet, the GPU checked free per
process (`Get-Counter '\GPU Engine(*)\Utilization Percentage'`). In some periods the GPU image path
and the device path take 15-35 ms a frame and the network alone 15-31 ms, against 14-15 and 12.3;
the host path in the same process always runs the network in 12.3-12.8.
- **The cause so far: the GPU's clock.** Level Zero's sysman reads
  700-1400 MHz in those phases, the request equal to the actual and no throttle reason; 1950 in the
  host path and in fast periods. The driver's governor chooses it; no power or thermal limit.
- **Ruled out:** constant 5 (seen with 0 and 1); the clock poll itself (with and without, alternating);
  the process's power class (default, high QoS, EcoQoS through `SetProcessInformation`); an idle CPU
  (a busy core in another process beside it changes nothing); the wait (every wait is a blocking
  `vkWaitForFences`). Another GPU user doubles every mode, the host path too: a replay running beside it did, and those runs are left out.
- **Periods, not processes:** none in 20 processes at 12:44-12:47 and 15:32-15:35; nearly all of 35 at
  15:35-15:45.
- **A game does not see it**: Hogwarts Legacy from the 0.0.7
  release through the FSR 3.1 proxy (the game is set to FSR 3 Quality with XeFG x4; its settings
  left as they were), B at 768x448 for 1920x1080 at 0.4. Over 9 minutes, 8736 frames from the
  daemon's log: the network 25.0 ms median, 27 at the 90th percentile, 30 at most, never doubled;
  the GPU at 1900-1950 MHz with the request at 4250, as in the host path. The bench right after it
  was low again in its GPU-only modes (1360-1830 MHz, request 1400-2700; the host path 1940, 4232).
  So the governor holds the clock up under a game's load, and the slow state belongs to the bench's
  GPU-only modes: time those with the clock beside them, within one state. Nothing to change in
  NR for it. What sets the request (4250 against ~1400) is not known; forcing the clock is not done
  (11-15). The daemon logs to the release root's
  `work\nr_daemon.log`, not beside the game.

**Next on Windows:** nothing. **Next on Linux:** nothing.

## `XMX_E4M3_MAGIC=1` on Intel's Windows driver: the same bits and 1-5 % faster; `789ed4c` built and checked (2026-11-22, midday, branch `newnr`)

Arc 140V, Intel 101.9150 before and after. Asked by the morning's entry below.
- **Built and checked at `789ed4c`:** MinGW CMake/Ninja builds; MSVC `build_win.bat` and
  `build_xess.bat` with no warning; CTest 72 of 72 on MSVC's libraries.
- **The same bits with constant 5 on this driver:** `frame_replay.py`'s heads with 0 and 1 are
  Linux's (B 576x1024 `edcbf478`/`acbd05d9`, 320x512 `df409ac4`/`b6d9870b`, A 320x512
  `3e37b87c`/`29d613d2`); `e4m3_probe.py` 0 mismatches over 1 114 875 inputs; `test_ffn_fused`,
  `test_window_block`, `test_glue` and `test_global_attention` pass with `XMX_E4M3_MAGIC=1`.
- **Timed:** `frame_profile.py --warm 25 --runs 5`, device ms a frame, every run its own process,
  a warm-up process thrown away, rounds alternating the order. Two sets: 3 rounds (quiet before;
  after it Windows Update's servicing had started, TiWorker and 33 % CPU) and 5 rounds (quiet
  before and after).

  | | default | `XMX_E4M3_MAGIC=1` | medians |
  | --- | --- | --- | --- |
  | B 576x1024 | 41.22/38.74/41.08; 41.13/39.78/39.40/39.04/42.05 | 39.67/39.39/39.81; 39.82/39.05/39.13/39.35/41.02 | -3.4 %, -1.1 % |
  | B 320x512 | 12.18/12.76/12.08; 12.37/12.03/12.26/12.42/12.55 | 12.17/11.69/11.83; 12.18/12.76/12.15/12.07/12.85 | -2.9 %, -1.5 % |
  | A 576x1024 | 80.97/77.56/76.89; 80.52/81.44/80.88/74.33/82.16 | 80.96/75.74/76.09; 80.74/81.21/79.10/73.74/77.03 | -1.9 %, -2.2 % |
  | A 320x512 | 24.94/23.42/24.02; 25.47/24.76/25.79/26.35/25.69 | 24.81/22.03/22.77; 25.24/24.85/24.40/22.62/25.09 | -5.2 %, -3.3 % |

  Faster by the median in all 8 comparisons and in 26 of the 32 pairs; a process's own state moves
  a frame 2-8 %, as much as the constant, so no single pair decides. The window blocks 2-7 % faster
  in 29 of 32 pairs, the fused feed-forwards in 25. One loss: global attention at 320x512, 0.09 ->
  0.13-0.14 ms in all 16 pairs there (unchanged at 576x1024). October's 10-22 % loss (10-13) is
  gone; the gain is about half of Mesa's.
- **The daemon's own paths** (`gpu_frame_stages.py`, its three modes, 5 rounds alternating, B at 1280x720 8 more): the outputs the same bytes with 0 and 1 in
  every mode and round. Where a process holds its speed: the host path's network 1.5 % faster, A
  and B at 1280x720 and 0.4; B at 1920x1080 and 0.4 1.8 % faster on the GPU image path (32.18 ->
  31.59 ms, 5 of 5 pairs), its network 1.9 % on the frames that never leave the GPU (24.32 ->
  23.85, 5 of 5). At 1280x720 the two GPU paths change speed state from process to process (B's
  frames 14-35 ms, the network alone 12.4-29.5, while the host path in the same process holds
  12.5-12.9): the constant cannot be seen there; in the fast state both read 14.3-14.9 ms.
- **The checks owed from 11-21:** `8c82133` again, by column against `XMX_FUSED_BY_COLUMN=0`, 5 rounds alternating: B 576x1024 39.97 against 41.65 ms, A 78.66
  against 81.48, 5 of 5 each, the bands apart; the window blocks 8 % and the feed-forwards 15-17 %
  slower by row: the 4 % holds. The setup window's self-test after `0169ad9`: 105 controls, 217
  keys; and its pacing block (`Merge-PacingFromProfile`, `6c61d486`, 10-23) was a bare `{ }`,
  which PowerShell prints instead of running, so its four checks never ran. Now `& {`
  (`dist-tools/windows-wizard.ps1`): it passes, and fails as it should with the function broken.

**Verdict:** stably faster by the median, never slower, the same bits: the constant goes
on for Intel's Windows driver too. **Done** (11-22, afternoon):
`libxmx.c` turns it on for `VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS` as for Mesa, and the comments
in `publish.glsl`, `e4m3_probe.py` and notes/CLAUDE.md say so. Checked: CMake and MSVC with no
warning, CTest 72 of 72; the default reads on here and `XMX_E4M3_MAGIC=0` turns it off; the heads
with the default are Linux's. **Next on Linux:** fetch and fast-forward; nothing changes on Mesa.
**Next on Windows:** nothing for this; the next release carries it.

## The GPU image path on by default on Linux, and exact on Mesa (2026-11-22, morning, branch `newnr`)

- **What ran on the CPU on Linux**: the daemon's passes around the network, the frame's resize and
  features, the head's upscale, the composition and the encode. The network itself has always run
  on the GPU. The GPU image path existed (`NR_GPU_IMAGE=1`, which the Windows release sets) and was
  off by default everywhere, because on Mesa it was not the host path's bytes (below, the night).
- **Exact on Mesa now.** The night's half rounding (`fff7ea5`), and the division:
  `image_compose.comp`'s and `image_history.comp`'s `divide` and `image_preprocess.comp`'s
  `area_divide` have `_f64` builds (`NR_DIVIDE_F64`) that divide through double. libxmx takes them
  on Mesa's ANV where the device has `shaderFloat64`, and turns the feature on at device creation
  (`xmx_image_divide_f64`; `XMX_IMAGE_DIVIDE_F64=0|1`). `xmxres.image_shader` picks the build, and
  `image_path_exact` is true there as well, so the image parity tests run on Linux now and pass
  (`test_image_compose`, `_history`, `_preprocess`, `_pipeline`). The default builds are byte for
  byte as before, and Intel's Windows driver keeps them.
- **On by default on Linux** (`nr_daemon.gpu_image_wanted`): `NR_GPU_IMAGE=1` or `0` decides; unset,
  on where `image_path_exact` holds and the system is not Windows, whose release sets the variable
  itself and whose speed test leaves it unset on purpose. A pipeline that cannot be built, when
  nobody asked for it, leaves the host path with one line instead of failing every frame. A daemon
  with no settings here says `GPU image path ready: transport=gpu-image-host`.
  `test_image_pipeline.py` holds the rule. `docs/DEVS.md` has the variable's row.
- **Measured after** (`gpu_frame_stages.py`, B, median ms a frame; `exact outputs=True` in all
  three; the double division costs nothing seen, the passes 0.86 + 0.67 ms at 1280x720):

  | | host path | GPU image path | frames never leaving the GPU (Windows only) |
  | --- | --- | --- | --- |
  | 1280x720 at 0.4 | 20.4 | 16.6 | 15.7 |
  | 1920x1080 at 0.4 | 40.8 | 30.7 | 28.9 |
  | 1280x720 at 0.5 | 28.6 | 23.9 | 22.7 |

- Checked on Linux: `make test` 74 of 74 in both memory modes, CMake with no warning, CTest 73 of
  73, `build_check.py` (both build systems build the `_f64` shaders, from the same sources with
  the same defines).

**Not run:** a game on Linux. **Next on Windows:** with the night's commits, the MSVC build and
CTest; nothing in the Windows path changes (the `_f64` builds are never chosen on Intel's Windows
driver, and the release's shader list is as it was).

## The network's speed, the night of 11-22: E4M3 by a magic constant on Mesa, and what did not pay (2026-11-22, night, branch `newnr`)

The task: speed the network up at full size after NVIDIA's own structure (activations in E4M3
and the rest). Mesa 26.2.7, `frame_profile.py --warm 25 --runs 5`, every pairing
in two rounds with the order swapped (`work/e4m3act/pair.sh`, `pair_env.sh`). The baselines: B at
1344x768 (1280x720's field) 74 ms on the device, at 512x320 (1280x720 at 0.4) 15.6; A 139 and
30.

**Kept, local, tested: every E4M3 publish by a magic constant on Mesa** (`publish.glsl` constant 5,
`libxmx` `e4m3_magic`, `XMX_E4M3_MAGIC=0|1`). The rounding `NR_E4M3_MAGIC` had since October,
the magnitude plus a power of two that leaves three mantissa bits and the same power taken
away again, is now a specialisation constant: on for Mesa's ANV, off everywhere else, so Intel's
Windows driver, 10-22 % slower with it in October, runs as before.
- The same bits: `src/bench/e4m3_probe.py` 0 mismatches over 1 114 875 inputs; the heads of
  `frame_replay.py` the same with the constant 0 and 1, and those of 11-21 (B 576x1024
  `edcbf478`/`acbd05d9`, 320x512 `df409ac4`/`b6d9870b`, A 320x512 `3e37b87c`/`29d613d2`).
- The device's time, constant 1 against 0: B 1344x768 71.2/70.4 against 73.9/74.4 ms, A
  133.8/133.6 against 140.1/140.3, B 512x320 15.2/14.7 against 15.1/15.3, A 512x320 29.2/26.2
  against 30.9/26.7. The window blocks 8-10 % faster, the fused feed-forwards 4-12 %. On the
  daemon's own path (`gpu_frame_stages.py`, with `NR_GPU_IMAGE=1`, rounds alternating): B at
  1280x720 and 0.4 16.5/16.1 against 17.7/17.6 ms a frame, the network 13.7/13.6 against
  14.6/14.5; A there 30.1/30.0 against 32.2/33.0, the network 26.9/27.0 against 28.9/28.9; B at
  1920x1080 and 0.4 30.7/31.1 against 32.3/34.2, the network 26.0/26.3 against 27.5/27.8.
- `make test` 74 of 74 in both memory modes, `XMX_E4M3_MAGIC=0 make test-gpu` 44 of 44, CMake
  with no warning, CTest 73 of 73. Not built with MSVC.

**Measured and not kept:**
- **Activations as E4M3 bytes**, NVIDIA's layout between its passes (`phase75`). A knockout, the
  staged GEMM reading its A operand as eight bytes where it reads sixteen and decoding them
  (`work/e4m3act/a-bytes-knockout.patch`): the GEMMs with N = 32 gain 5-45 %, every other one
  loses 10-20 % to the decode, and the frame gains nothing (B 1344x768 74.3/77.1 -> 76.5/77.0,
  A 138.8/137.8 -> 143.3/141.3, B 512x320 15.6/15.7 -> 15.9/16.2). Routed to the narrow GEMMs
  alone it could save 1-2 % of a frame before the producers are made to write bytes. The frame
  is not memory-bound: the window blocks and the feed-forwards at full size read and write
  20-40 GB/s against ~80.
- **The 32-channel blocks' skip as half**, as the reference's `numerics.md` has it (half carries
  the accumulators and those skips; ours float32; `work/e4m3act/half-skip.patch`, `NR_HALF_SKIP`):
  no faster (B 74.4/74.1 -> 74.7/75.1 ms), and no closer to NVIDIA (closer on 10 of the 19
  frames with A and with B, the correlation of the change -0.0005 and +0.0001 on average).
- **Half arithmetic for the publishes.** Knockouts, each primitive replaced by the identity
  (`work/ko_*`): `e4m3` is 8 % of B's frame at 1344x768 even by the magic constant (the window
  blocks 15 % of theirs), `half_round` 6.7 %, the feed-forward's gate 3.5 %: the fused kernels
  are bound by their arithmetic as much as by anything. But a half FMA on Mesa does not round
  once (470 of the 65 536 half inputs of the gate differ by an ulp), the gate exact with float32
  FMAs and a half multiply is 6 % faster alone, and the E4M3 publish in half arithmetic, exact
  over every half, is slower than float32's (3.14 against 2.60 ms in `work/probe/gate_probe.py`).
  Nor is a table: the gate and its publish are a function of 65 536 half inputs, and looking
  them up (`work/probe/lut_probe.py`) took 6.58 ms where computing them took 6.20.
- **Spills**: of the pipelines a frame runs (`XMX_PIPELINE_STATS`) only block 0's feed-forward
  spills much (25, 95 fills); the merge's 4 and 54, the window blocks 3-6. Not followed.
- **The window block's weights**, 6 + 2 + 1 KB copied into shared memory for every window: a
  knockout writing a constant there instead takes nothing off (18.86/18.85 against 18.98/19.15
  ms). Its time is ~30 % arithmetic and the rest its structure, as Windows found on 11-15.
- **The 32-row staged build for every GEMM with fewer than 256 64-row blocks** (twice the
  blocks; the same bits): slower everywhere, A at 512x320 29.0/28.9 -> 33.4/31.6 ms, B
  14.9/14.0 -> 16.1/15.9, 768x448 3-8 %. Each block reads B's tile again.
- **A pass's own cost** is ~2.7 us with its barrier (a thousand tiny passes in one submit): B's
  179 at 512x320 are ~0.5 ms of its 15. Fewer passes is not the lever.

**The image path on Linux:** a game on Linux has the daemon's host passes on
the CPU unless `NR_GPU_IMAGE=1`, which the Windows release sets; the GPU path is off on Mesa
because it is not exact there. Measured (`src/bench/gpu_frame_stages.py`, median ms, B; A
the same shape):

| | CPU path | `NR_GPU_IMAGE=1` |
| --- | --- | --- |
| B 1280x720 at 0.4 | 22.2 (prep 3.0, compose 3.2, metadata 0.8) | 17.1 (0.9, 0.7, 0) |
| B 1920x1080 at 0.4 | 39.3 (4.2, 6.5, 1.2) | 30.8 (1.5, 1.4, 0) |
| A 1920x1080 at 0.4 | 62.4 | 54.4 |

And how far from exact: over the 19 frames at 0.4 (`work/vendor/scale_set.py` with
`NR_GPU_IMAGE=1`), A and B, the two paths differed by one level at most, on 0.05-0.22 % of the
values, and agreed with NVIDIA's output to the fourth place. **Two causes, found:**
- `image_compose.comp` rounded the head to half with its own `float(float16_t(v))`, the cast
  Mesa folds away (`publish.glsl`'s constant 1 exists for exactly this): on Mesa the head went
  unrounded. Fixed locally, the same constant 1 (`HALF_BY_CAST`), so Intel's Windows driver keeps
  the cast it had. With it the composed bytes of all 38 (19 frames, A and B, at 0.4) are the CPU
  path's byte for byte, and `gpu_frame_stages.py` reads `exact outputs=True`. `make test` 74 of 74.
- The division the image shaders correct to the CPU's correctly rounded quotient
  (`divide`, `area_divide`: the native quotient, then `q + fma(-q, b, a) / b`) is not corrected
  on Mesa: there the residual is exact in only 5-45 % of cases (`work/probe/fma_probe.py`, the
  quotient laundered through its bits or not), though a lone `fma` is fused, and the result is
  one ulp off in 7-17 %. Divided through double instead (`shaderFloat64`, which this ANV has), the
  four image parity tests pass on Mesa with `image_path_exact` forced (`test_image_compose`,
  `_history`, `_preprocess`, `_pipeline`). Not kept: Intel's Windows path is exact as it is, and a
  module that declares Float64 needs a driver that has it, so it would be a second SPIR-V per
  image shader chosen per driver (`work/probe/image_*-div-f64.patch`). It matters for an exact
  claim on Mesa in the area mean (a scale with an odd factor), the grade and history, not for the
  common path above.

`src/bench/gpu_frame_stages.py`, like `scale_spectrum.py`, had kept the network-time trailer
as the reply since it came in, and refused every frame: it keeps the pixels now. Local.

The frames that never leave the GPU (`gpu-device` above, 15.7 ms against 17.1 at 1280x720 and
0.4) need the shared transport, which the layer has on Windows alone (`#ifdef _WIN32` around
`NR_GPU_TRANSPORT`): on Linux a frame crosses the pipe both ways. A Linux transport (an exported
memory fd) would be a project of its own.

**On `newnr`** (11-22, morning): constant 5
(`75b4626`: `publish.glsl`, `libxmx.c`, `xmxres.py`, `nr_daemon.py`, and `e4m3_probe.py`
comparing against the constant off, since on Mesa `publish.glsl`'s own `e4m3` is the magic one
now; 0 mismatches again over the same inputs), the composition's half rounding (`fff7ea5`), the
two benchmarks' trailer (`f63c41b`) and these notes. Checked at that state: `make test` 74 of 74 in both memory modes, CMake with no warning, CTest
73 of 73; the heads with the constant 0 and 1 the same at 512x320, 576x1024, 1920x1088 and
2560x1472.

The GPU image path went on for Linux with the division that makes it exact on Mesa (above, 11-22,
morning). **Next on Windows:** once this is on GitHub, the
MSVC build and CTest, and `XMX_E4M3_MAGIC=1` timed against the default on Intel's driver
(`frame_profile.py --size 576 1024` and `--size 320 512`, B and A, rounds alternating): October's
10-22 % loss (10-13) came before November's changes on that driver, the staged walk, the E4M3 and
by-column weights and the fused kernels by column; if it gains now, the constant goes on for that
driver too.

## Render scale against NVIDIA's full picture: NVIDIA has no smaller network, and a smaller frame is a different picture (2026-11-22, later, branch `newnr`)

How the render scales players use compare with NVIDIA's full picture, and what NVIDIA's own
`DLSSNR.ScalingRatio` below 1 does. The note is
`notes/phase76-render-scale-against-nvidia.md`.

- **NVIDIA's 310.9.16 always runs the network at the output's size.** With `ScalingRatio` 0.5 its
  output is byte for byte that of 1.0, and it asks for no such parameter. In its code the ratio is
  written for an application by `DLSSNRComputeScalingRatioCallback`: 1.0, or 0.85 for
  `PerfQualityValue` 4, never below 0.5. The evaluation's parser fixes the ratio at 1.0, so the
  smaller network the evaluation can build never runs.
- **Our render scale against NVIDIA's full size**, 19 frames, the daemon's own path:
  - A: correlation of the change 0.993 at 1.0, 0.931 at 0.75, 0.827 at 0.5 and 0.749 at 0.4. The
    residual is 11, 34, 55 and 68 % of NVIDIA's change.
  - B against NVIDIA's B: 0.992, 0.927, 0.789 and 0.751.
  - B against NVIDIA's A: 0.875 at 1.0.
  - The coarse band (a 3 px blur) falls as well: A at 0.5 is 0.846.
- **It is the model.** Our full-size change, taken down to the scale's frame and back up, would
  still be 0.986 at 0.5, so the upscale costs 0.01. NVIDIA's own network on six frames halved to
  640x360 agrees with its full-size picture at 0.737 (A) and 0.777 (B). Ours at 0.5 agrees with
  that half-size picture at 0.978 and 0.984. No upscaler can win this back.
- **Cost on this laptop**, 1280x720's fields, device ms: A 140 / 80-82 / 42-44 / 30-31, B 74 /
  42-43 / 22 / 15.5 at 1.0 / 0.75 / 0.5 / 0.4. At about 42 ms, B at 0.75 is 0.927 of NVIDIA's B
  and 0.834 of its A, where A at 0.5 is 0.827 of its A.
- `src/bench/scale_spectrum.py` had lost two pixels to the daemon's network-time trailer since it
  came in: it reads the reply's pixels from the front now. Local, with the notes; not committed.

**Not decided:** the render scale's texts say a lower scale draws "coarser detail, at
about the same strength" (`nr_knobs.py`, and from it `docs/SETTINGS.md`) or is "cheaper, with
coarser detail" (`Hint.render_scale` in four languages, `windows-wizard.ps1`, and the panel's
tables from it). It draws a different picture: whether to say so, and how.

**Next on Linux:** nothing owed. **Next on Windows:** nothing.

## The half accumulator timed again on Mesa: about 1 % of a frame, and no closer to NVIDIA (2026-11-22, branch `newnr`)

The FP16 accumulator, NVIDIA's own arithmetic, was last timed in a frame at phase 31, behind the ANV
store bug that phase 38 found was not there.

- **The build.** `gemm_staged.comp` takes `-DACC16` as `gemm_resident.comp` does: a `float16_t`
  accumulator, widened to float32 at each store, direct or through the stage (a matrix is stored
  only into an array of its own component type, phase 38). Off by default, and the default
  SPIR-V of the three staged builds was byte for byte as before. Dropped (11-22): the change is `work/acc16/gemm_staged-acc16.patch`, the builds beside it.
- **Speed**, `frame_profile.py --warm 25 --runs 5 --calls 0`, every GEMM kernel built with it
  (resident, tiled, staged, staged32 and its deep build, through `XMX_*_SPV`) against the
  default, five rounds with the order swapped each round, Mesa 26.2.7. Median ms of the device
  (of the staged GEMM):

  | | FP32 | ACC16 |
  | --- | --- | --- |
  | A 1024x576 | 84.12 (49.46) | 82.73 (48.26) |
  | A 512x320 | 30.44 (19.53) | 30.24 (19.19) |
  | B 1024x576 | 43.47 (19.87) | 43.19 (19.47) |
  | B 512x320 | 15.54 (8.33) | 15.26 (8.23) |

  1.2-2.5 % of the staged GEMM, 0.8-1.8 % of the device; the paired differences straddle zero in
  three of the four.
- **Why so little.** The pipelines a frame runs are the specialised ones (`XMX_SPECIALIZE` is 7
  by default), and none of the staged kernel's twelve spills in float32 (Mesa's `INTEL_DEBUG=cs`):
  a subgroup holds four 8x16 accumulators, 16 of its 128 registers. Phase 31's 1.36x came from
  a block that spilled 15 in float32 and none in float16. The fused kernels, which have no
  `ACC16`, spill little: `window_block`'s variants 4-7 each, `ffn_fused`'s none or 41-43, so a
  half accumulator there has a couple of per cent of a frame to find at most.
- **Against NVIDIA** (`work/vendor/compare.py`, the 19 frames of `set_parity.txt`, ours as
  `nr_frame.py --profile standard --intensity 1.0 --resident` with the five kernels): ACC16 is
  closer to the vendor's output on 8 of the 19 frames with A and on 8 with B. The correlation of
  the change moves by −0.0001 on average with A (−0.0019 to +0.0010) and 0.0000 with B (−0.0007
  to +0.0008), the residual's share by +0.3 and 0.0 points. The first frame tried, the 512x320
  crop of MK1, read 0.9921 -> 0.9927 and alone would have said closer.

So on Mesa the half accumulator buys about 1 % and brings the picture no closer to NVIDIA's; it
stays off. Intel's Windows driver is untimed: another compiler, where the staged GEMM's loop
takes 1.2-1.3x Mesa's time.

**Next on Linux:** the speed settings players use against NVIDIA's full picture, and NVIDIA's
own `DLSSNR.ScalingRatio` below 1: done (above, 11-22, later). **Next on
Windows:** nothing.

---

The reference sections. 1 ("Do this first") and 6 ("Honest standing") are in the archive, and since
11-11 so are the long forms of 0, 2, 3 and 5, in `HANDOFF-branches-and-reference-until-2026-11-11.md`;
all keep their numbers, because other files cite them. 4, the traps, and 7 are kept whole.

## 0. The one thing to know

**The weight container does not hold plain dense FP16, and almost every dead end in
this project traces to assuming it does.** Start from `work/mlxw/dlssnr-logical.safetensors`, not
from the container's raw bytes; how to make it is in `docs/DEVS.md`, and the brief's "The weight
format" has the facts. The proof and the MLX-DLSS recipe are in the archive.

---

## 2. What is solid — measured from the kernels, unaffected by the packing

A table of the network's facts, each with the note that measured it: softmax with hard-clamped
logits, the gate on the skip, FP32 `attn_scale`, head_dim 32, the input contract, the 16-channel
packing, the 32-to-4 head, the subnormal flush, the numerics. In the archive.
`docs/ARCHITECTURE.md` is the specification; the brief's "Settled" says what not to re-derive.

---

## 3. Withdrawn — do not resurrect

Eight claims were withdrawn in one day, and four older ones stay withdrawn: plain FP16 weights;
grouped-query attention (`qkv_weight` is `(C, 3C)`: full MHA); "DLSS-NR does not use softmax"; K
and V pre-replicated; an attention mask in the 128C region; `attn_scale` stored as a log (it is
FP32); the attention branch's 1.39x swing; "1.02x better than the input"; the missing 2^8.5 factor,
the ~450x branch/skip factor, the leading-region projection and "1.01x parity". Why each fell is
in the archive. `src/tools/claims_check.py` fails the suite if a withdrawn claim is published
outside `notes/`. Assume the next confident claim is also wrong until it survives an adversarial
test.

---

## 4. Traps this project keeps falling into

- **Tools that skip input silently.** `ptx_trace.py` dropped brace scopes and hid 120k
  instructions. Always check coverage: parsed statements vs `;` count, and whether the
  dataflow graph is connected.
- **PTX is not SSA, and it has loops.** "Last definition before the use" is only valid
  in straight-line code. The pre-block's store tail sits *textually before* the texture
  reads it consumes, because it is a loop body. A def/use map over all definitions
  invents edges; program order alone is not enough either.
- **The metric has had five holes.** Correlation rewards inaction; dividing by a fitted
  gain is 0/0 on collapse; a pass-through scores 1.00x; the score measured the
  scaffolding not the model for a whole phase; and results depended on the test image
  and the noise level. `run_frame.py` now reports COLLAPSED, PASS-THROUGH and
  NETWORK-INERT — **and `--no-attend` is the control to run whenever a score moves.**
- **Statistical segmentation cannot find tensor boundaries.** Three methods failed
  calibration at C=512 where the answer was known. The notes said so; I re-ran one of
  them anyway. Read section 3 before trying a fourth.
- **A lookup table written from memory mislabels everything downstream.**
  `frame_profile.py` shipped a hand-written map of unary kind numbers to names. It was
  wrong, and under the wrong names one pass appeared to run at a fifth of memory speed,
  which produced a confident and entirely false diagnosis — with a proposed fix — before
  anyone noticed the table. The 50 ms belonged to the one pass that needed nothing.
  **Generate name tables from the source they name.** The file now regexes them out of
  `resident.comp` and `attention.comp` at import. `notes/phase45`.
- **A textbook mechanism is a hypothesis, not a finding.** `attention.comp` gives each of
  a subgroup's 32 lanes one row of a stride-64 shared tile: bank `i mod 32` for all 32
  lanes, a perfect 32-way conflict, arithmetic beyond dispute. Measured on Xe2 with a
  30-line probe it costs **1.11x**, not 32x, and the `gather`/`scatter` surgery it would
  have justified was not worth doing. `src/bench/bank_probe.*`, `notes/phase45`.
- **On this model, looking at the picture disagrees with measuring it.** The pass moves
  the *level* — it pulls back blown-out skin by 30-40 % — and that shift dominates every
  raw statistic and every impression. Twice in one session a confident visual reading was
  wrong: "more pores" where fine texture had *fallen* 16 % (it rose 31 % once normalised
  for level), and "the irises turned brown" where the hue moved 18° to 21° and had been
  brown all along. **Normalise for level, and sample the exact pixels you are claiming
  about.** `notes/phase42`, `phase44`.
- **One frame is not a recommendation.** `cinematic` was declared the defensible default
  for faces on the strength of a single cutscene where it landed slightly positive. Three
  frames later it was removing detail — on a bright scene it *smooths*. Any statement of
  the form "profile X is right for Y" needs several scenes. `notes/phase44`.
- **`pgrep -f` and `pkill -f` match the shell that runs them.** Four times now. The
  `[n]ame` bracket trick fixes the pattern but not the case where the string also appears
  elsewhere in your own command line — a heredoc, a comment, an echo. `pkill -f
  nr_daemon.py` killed the very shell launching the daemon. **List with
  `ps -eo pid,args | awk '/pattern/ && !/awk/'` and kill by explicit PID.**
- **The test suite and a loaded daemon do not fit together.** 15 GiB shared with the iGPU;
  a resident daemon holds the model, `make test` allocates its own device buffers, and the
  run gets OOM-killed. Stop the daemon before the suite, not after.
- **Steam compiles shaders behind your back, and it looks like a slow machine.** After a
  Vulkan driver update it replays each game's pipeline cache — Counter-Strike 2's was
  6.5 GB — in `fossilize_replay` workers that hold whole cores for many minutes. A
  benchmark run alongside is measuring a machine with cores missing. `phase60` was.
  **`ps -eo pcpu,comm --sort=-pcpu | head` before any timing run.**
- **Numbers in a published page rot silently, and the page keeps being read.** The
  README's frame-time table was measured on 2026-09-10 and survived the host passes moving
  to C two days later: for a week it understated this machine by a third, and at 1080p by
  half, on the same page that quoted Tekken's 10.5 fps and contradicted it. It is now
  generated from `nr_knobs.RATES` by `src/tools/knob_doc.py`, with `src/bench/live_rates.py`
  behind the numbers, and `make test` fails if the page and the table disagree. Sibling
  check: `src/tools/claims_check.py` fails the suite if a withdrawn claim appears outside
  the notes that withdrew it — it found eight on the day it was written. `notes/phase64`.
- **A pinned dependency nobody re-clones is a dead pin.** The README's Vulkan-Headers
  commit did not exist in KhronosGroup/Vulkan-Headers at all, so the build's second line
  failed for every reader from a clean checkout; the local copy has no `.git` and could
  never have shown it. Pin by tag (`--branch v1.4.321`), which fails loudly at clone time.
- **External write-ups are summaries, not sources.** A WebFetch of `weight_spec.json`
  returned plausible-looking shapes with a confabulated label (`block31` as "final
  output stage"). Clone the repo and read the file.
- **Nothing pinned the subgroup width.** SPIR-V 1.6 lets the driver pick it, and ANV picks SIMD16
  where SIMD32 spills; code written for 32 lanes then races, only in the build that spills. Every
  pipeline now requires 32 (`build_pipeline_spec`, 2026-09-27). A kernel that is right only
  sometimes, and only unspecialised, was this.
- **A reset to the remote drops whatever was never pushed.** On 2026-09-23 `master` was
  reset to `origin/master`, and three commits kept on purpose the day before went with it;
  `test_present.c` cited two notes that no longer existed until they were restored on
  09-24. Before resetting a branch to its remote, `git log origin/<branch>..<branch>` — and
  leave a backup ref.

- **Measure with empty swap, a quiet CPU, and the clock warmed.** zram under load spread 1080p
  live runs over 322-463 ms (archive, 09-23). The GPU clock climbs through the first frames
  (`frame_profile.py --warm`), and the graph's time moves up to 44 % between daemon processes while
  staying steady within one (archive, 10-02). Pair and alternate, and quote a band. On Linux the
  governor also clocks down through the gaps between frames, and where it settled carries into
  the next process (B's 320x320 without the hint: 11 ms right after runs with it, 15-18 after A's
  without). libxmx holds the clock since `fe00b06`, so a run with `XMX_LOW_LATENCY=0` measures
  the governor as much as the code (10-18).
- **Shared memory comes in allocation sizes.** A 64-byte array beside a 2 KB stage took every
  tiled GEMM to 4 KB and cost 23 ms of a 720p frame. Mesa rounds each declaration up, and a core
  holds 128 KB between its workgroups: declare exactly an allocation size. Archive, 09-23 and
  09-24; `notes/improve-shared-memory.md`.
- **A fusion moves a write into an earlier dispatch**, so it has to re-check the scratch arena.
  The role table assumes a pass finishes before the next starts (`k16` shares a role with the QKV
  projection's input). Archive, 09-23.
- **The compiler's contraction is not the arithmetic.** `branch + skip * cos` was one fused
  multiply-add inside one kernel and a multiply and an add written inline in another (archive,
  09-26), and Intel's compiler contracted the scalar form and not a four-channel one (archive,
  10-02). Write `fma()` under `precise` where the bits matter.
- **A comparison that holds the change constant proves nothing about the change.** The
  `min_extent` bottleneck bug was "checked" with the pad at 32 on both sides (archive, 09-26).
  When a new test passes, run it once against the old code (archive, 09-19).
- **Instruction counts mislead; knockouts measure.** Replacing each load in turn with a constant
  found window attention's cost in two runs where counts pointed the wrong way twice, and counts
  say nothing across the two compilers. Archive, 09-25 and 10-02.
- **Do not take a game's API from its executable.** `/proc/<pid>/maps` on the running process
  says which renderer it loaded, and whether the layer attached. Archive, 09-16.
- **Never install a desktop shortcut from a tool.** In Plasma 6.7 the registry lives inside KWin,
  and editing it behind KWin's back crashed KWin at the first key. Bind it in System Settings.
  Archive, 09-11.

---

## 5. Layout, commands, repo

The directory map as of September, the regression commands and `nr_frame.py`'s flags and controls
are in the archive. The brief's "Repo layout" is the map, `docs/DEVS.md` the build and the tests
(`make test`, CTest), and `notes/phase10-controls.md` the controls.

---

## 7. Hardware (probed, trust it)

Intel Arc 140V-class Xe2, Mesa ANV, Vulkan 1.4.354, subgroup 32. Six cooperative-matrix
configs, all scope=subgroup, all M=8 N=16; the path is **fp16 × fp16 → fp32** (config 1).
`cooperativeMatrixRobustBufferAccess = false`, so edge tiles need explicit padding.
Shared-memory APU: correctness and capacity are unaffected (141 MiB of weights against
an 11.46 GiB heap); it caps **bandwidth**, already the measured ceiling at 0.7–1.9
TFLOP/s with a naive kernel. `libxmx.c` still memcpys into a mapped HOST_VISIBLE buffer
on every dispatch — on an integrated GPU those copies are free to remove.

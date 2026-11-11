# HANDOFF — read this first

State of the DLSS-NR on Intel Xe2 project as of **2026-11-05**. notes/CLAUDE.md holds the
brief, shortened on 2026-11-04 (the original is `notes/archive/CLAUDE-until-2026-11-04.md`);
**this file overrides it wherever they disagree**.

**How to read this file.** "Where things stand" and "Open items" are the current state. The dated
entries after them are the latest, newest first. Everything older is in `notes/archive/`, word
for word: the graph's recovery, the kernel work, the measurements and the experiments that did not
pay in `HANDOFF-until-2026-10-07.md`, the first Windows releases and the testers' reports in
`HANDOFF-2026-10-08-to-2026-10-15.md`, DirectX 12 through XeSS, preset B and PR #20's review in
`HANDOFF-2026-10-16-to-2026-10-28.md`, DirectX 12 in the release, 0.0.2, the panel inside
DirectX 12 and the GPU report for Arc A-series in `HANDOFF-2026-10-30-to-2026-10-31.md`, and
0.0.3 and 0.0.3.1, the Arc A-series reports, the players' data and the staged GEMM's walk order in
`HANDOFF-2026-11-01-to-2026-11-04.md`, with the Open items closed by 11-11 after them, 0.0.4 and
0.0.5, OptiScaler, the frame counter and the Hogwarts profile in
`HANDOFF-2026-11-05-to-2026-11-06.md`, and the releases' and branches' day-by-day history with the
reference sections 0, 2, 3 and 5 in `HANDOFF-branches-and-reference-until-2026-11-11.md`. It is
history: do not read it to start a task. Open it only when a line here points there, or before an
optimisation, to see whether it was tried and not kept. `notes/INDEX.md` says which note settles
what.

Keep this file short. Every session loads it whole and sends it again with every request it
makes, so its size is paid for on every turn, and `publish_check` refuses a file over 200 KB. When
the dated entries grow long, move the older ones the same way, into a new archive file: on 11-01
the entries before 10-30 went, and the file fell from 156 KB to 69 KB; on 11-04 those before
11-01, from 100 KB to 77 KB; on 11-11 those before 11-05 and every struck open item, from 138 KB
to 91 KB, and later that day the releases' and branches' history and the reference sections the
brief and `docs/` repeat, from 96 KB to 87 KB, and then the entries of 11-05 and 11-06, from 87 KB
to 44 KB. A closed open item is struck through where it stands, and goes to the archive at the next
such move; what it still leaves open becomes a line of its own.

---

## Where things stand (2026-11-05)

- **It works on both systems of this laptop** (Arc 140V): Linux with Mesa, Windows with Intel's
  driver. A Vulkan layer hands each presented frame to a daemon that runs NVIDIA's network on the
  GPU and writes the answer back into the swapchain. DirectX 8-11 games go through DXVK. DirectX 12
  runs on Linux through VKD3D-Proton, and on Windows through the game's own XeSS (since 0.0.2).
- **The releases are Windows-only so far**: v0.0.1 (`7f9c364`), v0.0.1.1 (`f9ec5f1`), v0.0.1.2
  (`2f90064`, 10-15), v0.0.1.3 (`d89563f`, 10-18), v0.0.2 (`81b0a59`, 10-31), v0.0.3
  (`86a5799`, 11-01), v0.0.3.1 (`c0b7f9d`, 11-01), v0.0.4 (`8232603`, 11-05) and **v0.0.5**
  (`bb9317b`, 11-06, the latest), a ZIP with the setup window, DXVK 3.1.1, both layers, a
  `vulkan-1.dll` proxy, NR's `libxess.dll` as `nr_xess.dll` and, since 0.0.4, its own Python.
  0.0.4 brings that Python and the staged GEMM's walk, which takes 9-19 % off the network on
  Intel's Windows driver (archive, 11-05 and 11-04 night). 0.0.5 brings the `hogwarts`
  profile, Hogwarts Legacy's own tuning, the panel's frame counter and OptiScaler's XeSS in setup
  (archive, 11-06, morning). What 0.0.2 to 0.0.3.1 brought is in the archive (10-31, 11-01) and on
  the release pages. The last five went out on the owner's word before Linux's `make test` at
  their heads, as 0.0.1.3 did, and Linux has run them green since: 0.0.2's at `eb9b281`, 0.0.3's
  and 0.0.3.1's at `d28e841` (archive, 11-04 later), 0.0.4's and 0.0.5's at `127f6dc` (below,
  11-11). Linux users build from source (README).
- **Branches**, on GitHub since 10-30: `master` (the default), `newnr` and `alchemist` are level
  since 11-11 and move together, each push a fast-forward of all three; `media` holds the README's
  pictures; `improve-int8` is kept for INT8 experiments; `cleanup` (PR #39, merged 11-11) stays.
  Seven branches were deleted at the owner's word: `dx12` (it went into `newnr` on 10-30, a
  fast-forward), `windows`, `improve`, `improve-e4m3`, `improve-fusions`, `improve-release` and
  `experiment-bgra8-pipeline`. All were already in `master` or `newnr`, except one merge commit and
  two of Codex's BGRA8 experiments. A bundle of the seven is in `D:\NRonWindows\backups` on
  Windows, and so is one of the stray `claude/weights-only-quantization-w0o2wh` (a HANDOFF commit
  that reached GitHub by accident on 11-01 and was gone by 11-04; its text is in the archive, 11-01
  latest). How `alchemist` ran ahead of the others and level with them, day by day, is in
  `HANDOFF-branches-and-reference-until-2026-11-11.md`.
- **DirectX 12 on Windows, through the game's XeSS** (`src/xess`, from 10-16, on `newnr` since
  10-30): a `libxess.dll` in front of Intel's hands the daemon the frame XeSS is about to upscale,
  in the game's own D3D12, and
  adds NR's change to it. It runs in Hogwarts Legacy. The trail behind moving things was found
  in a recording and fixed (`e803624`), **pushed on the owner's word before anyone saw it in the
  game**: what is untested is listed in the archive's entry of 10-16. Carrying a change onto later
  frames is what makes the trails and patches (10-18), so the proxy now cuts the game's command
  list at XeSS and runs NR on the frame itself, all on the GPU (`mode=cut`): the daemon
  takes D3D12 frames (step 1, archive, 10-26), the cut runs both Hogwarts recordings with no
  trail (step 2, archive, 10-27), and the daemon's history follows the game's motion (step 3,
  archive, 10-27). In the game, NR before XeSS gave halos and trails, so NR now runs on XeSS's
  output, before the interface (`mode=after`, archive, 10-28). In Hogwarts that way the owner
  found it excellent with preset B: at full scale, and at 720p and 0.5, about 21 frames a second,
  under the game's frame generation too (archive, 10-28 later). A daemon restarted under the game,
  for another preset, is taken back by the proxy 5 s later, on a new fence (archive, 10-28 evening);
  in Hogwarts A to B left 5.5 s without NR, most of it the new daemon's start, and the owner saw
  NR come back. Since 10-30 a preset changes in the running daemon instead, as the in-game panel
  changes it, and in Hogwarts NR stayed on through A to B and back (archive, 10-30). Since that
  evening the setup window installs it, **DirectX 12 (games with XeSS)**, and the proxy starts the
  daemon itself (`6c503ae`, archive, 10-30 evening). Since 10-31 the in-game panel draws there too, and in both
  hosts the game no longer gets the panel's keys and clicks; in no release yet (archive, 10-31 later).
- **NVIDIA's faster model is out** (`nvngx_dlssnr.dll` 310.9.16.0, 10-16): preset A is our model
  byte for byte. **Preset B (`hnet-crazy-cuckoo-small-fp8`) is A's graph with every stage at depth
  2**, 31 blocks against 71, with its own weights. On 10-17 it was read out of `.text` with REA,
  unpacked, and run here, on CPU and on the device, at 0.54-0.58 of A's time. Through the daemon
  its round trip is 0.5-0.8 of A's once the GPU's clock is held up (archive, 10-18). That is on
  branch `newnr`, on GitHub since 10-18; `NR_PRESET=B` picks it (archive, 10-17; phase72). On
  Windows the setup window takes B out of a 310.9.16 DLL and offers it (`a75e5da`, in 0.0.1.3).
  Intel's Windows driver clocks a gap-separated frame down too, B's to 865-956 MHz, and Windows
  has no hint for it yet (archive, 10-18, Windows).
- **Speed on this laptop**, `src/bench/speed_test.py`, network and round trip: 640x360 at 0.4 is
  23 ms and 26-27 ms on both systems; 1920x1080 at 1.0 is 273 and 299 ms on Linux, 384 and 409 on
  Windows, where Intel's compiler loses time in the staged GEMM's loop: per pass at the vendor's
  1080p and 1440p fields, 1.6-2.1x Mesa's time and all of Windows' loss, every other pass level
  or faster (archive, 11-04 evening). Most of it was the driver's walk order: since `6ef670e` the
  network takes 9-19 % less there, and B at 2560x1472 is within 2 % of Linux (archive, 11-04
  night), and 7-9 % less in Mortal Kombat 11 at 1280x720 and render scale 1.0 (archive, 11-04
  night, latest); the speed test's Windows figures predate it. The README's table
  (`nr_knobs.RATES`) was measured with `live_rates.py --warmup 3`, again on 10-18 with the clock
  hint, and the default warm-up of 25 now reads the same. On Linux libxmx asks the driver for its
  low-latency clock strategy since 10-18 (`XMX_LOW_LATENCY`): without it the GPU's governor
  clocked a frame with gaps around it down to 700-1200 MHz of 1950 (archive, 10-18).
- **On the players' GPUs** (archive, 11-03; `notes/players-20261103.md`): a B580 runs network A at
  1280x720 and 0.4 in 9 ms, 3.9x an A770. An A770 plays level with this laptop at that size,
  because its network pays ~55 us a pass whatever the field. Eleven A-series cards compute NR's
  shape exactly, and a Pro B70 and two B580s offer it natively.
- **Working rules.** Nothing goes to GitHub without the owner's word, and nothing untested at all:
  `make test` in both memory modes (`XMX_STAGING=1` is the second), CTest, and on Windows the MSVC
  build. Commits use the noreply address. NVIDIA's DLL, and anything derived from it, never enters
  the repository or a release. A Linux agent and a Windows agent work on this one laptop; the owner
  relays between them, and this file is how they hand work over. A release updates the README's
  GPU and tested-games tables in the same commit (the owner, 11-02). What people read, replies in
  issues and pull requests, PR descriptions, release notes, carries no "Generated by Claude Code"
  line or other mention of AI: people are uncomfortable reading it (the owner, 11-05, who had it
  taken out of the six replies that carried it, #19, #23, #25, #27, #29 and #38).

## Open items (collected 2026-10-14, from this file and its archive)

Each line says where its evidence is: "below" is a dated entry in this file, "archive" the archive
under that date. When an item is done, strike it through here and name the commit. The items
closed by 11-11 are in `archive/HANDOFF-2026-11-01-to-2026-11-04.md`.

**Waiting for the owner**
- Answers to the A-series reports, as GitHub stood on 11-04: we have commented on #18, #19, #23,
  #25, #27, #29 and #34 (#34: two comments from the owner's account, 11-03 and 11-04), and not
  yet on #22, #24, #26, #28, #30-#33 and #36. Two questions the data raised: #25, which network
  each API ran (its reporter wrote on 11-01 that A and B made no difference in either API, and
  that Vulkan gave 23 fps against DirectX 11's 40 at one scale and model; the 11-03 reading in the archive
  puts that down to A against B, so the two are not reconciled); #23, whether NR was on in
  Hogwarts (asked on 11-01, no reply). Archive, 11-03 and 11-04.
- Judgements in a game, pending since September:
  - Whether render scale 0.9 still looks better than 1.0 for screenshots, now that 1280x720 runs
    on the vendor's field. Archive, 09-27.
  - Whether Mortal Kombat 1 fits at render scale 1.0 now that the daemon needs 0.7-1.2 GiB. It
    was OOM-killed there on 09-16. Archive, 09-26.
- GitHub on 10-14: discussion #14 (a B570 in Mafia 3, and a Reddit post) has no answer. Issue
  #17 (DOOM at 3 fps with the GPU at 10 %) is answered and its GPU-path bug fixed, archive, 10-16.
  For #17 the owner's reading: when the whole frame waits on the network, the game has nothing to
  render past the network's own frame rate, so its load stays low. #12 and #15 work after a reinstall, and #12 waits for the next release. #16 is
  Paimonshen's in-game menu.
- Preset B against the vendor's own B: a capture on the RTX 5060 Ti with
  `DLSSNR.Hint.Render.Preset` set to B, when the owner opens that machine. Archive, 10-17 later.
- The two files that came with the 310.9.16 DLL (`nvngx_dlssnr_compat-fp16-310.9.16-cuda13.4.3.rar`
  and `renodx-dlss.addon64`, in the Windows Downloads). The owner says, as their source claims,
  that the first is the DLL patched for RTX 30 and 40 cards, with FP16 kernels, and the second
  lets RenoDX pick preset A or B. Untrusted and unopened; they wait until one is needed.
- `newnr` holds `perf-1108` and `e4m3-weights` since 11-11, locally, green on Windows and run in
  Mortal Kombat 11 (below, 11-11, morning): the owner's look, then the word on pushing it, with
  `master` and `alchemist` after it.
- From the `cleanup` branch (PR #39, merged 11-11), proposed and not done: INT8 out of the main
  line (it is in libxmx, the shader lists and the release's file lists) and one build system
  instead of three; the Configure/Restore Steam buttons are a line under Windows. Archive, 11-05.
- The working rules after #39: whether the second memory mode runs as `XMX_STAGING=1 make
  test-gpu`, and CTest on Linux only when CMake changes. Archive, 11-05.

- ~~**PR #39, branch `cleanup`**: green on Linux at `dff4ad5` and on Windows at `4cd720f`, with this
  file cut and `make test` quiet since, and pushed (below, 11-11, later). Windows builds and runs
  CTest at that head once more; then the owner's word on merging it into `newnr`~~: green on
  Windows at `fe2c33c` too, and merged into `newnr` as `3d933b0` (below, 11-11, latest).

**Linux**
- ~~**PR #39, branch `cleanup`** (on GitHub since 11-08: `newnr` merged in, Windows green):
  `make test` in both memory modes and CTest at its head, the code of `297e13a`: the first run on a
  GPU of its runner, `attention.comp`, libxmx and `nr_resident`, and three tests changed (below,
  11-09 and 11-08; archive, 11-05)~~: green at `dff4ad5`, with `newnr` (`c22f798`) merged in (below, 11-11,
  later).
- `e4m3-weights`, the bottleneck's weights as their E4M3 bytes (below, 11-08, evening): `make test`
  in both memory modes and CTest once `newnr`, which holds it since 11-11, is on GitHub. It is off on Mesa unless
  `XMX_E4M3_WEIGHTS=1`, so `test_gemm_e4m3.py` (new, `make test` and CTest `gpu_gemm_e4m3`) says
  it is off; run it once more with `XMX_E4M3_WEIGHTS=1`, and if Mesa decodes all 254 values, B's
  320x320 and 1344x768 heads with it set and unset, and the bottleneck's four GEMMs both ways.
  `gemm_staged.comp`, `libxmx.c`, `xmxres.py` and `nr_resident.py` changed.
- `perf-1108`, the GPU path's frame around the network (below, 11-08): `make test` in both memory
  modes and CTest once `newnr`, which holds it since 11-11, is on GitHub. `image_stats.comp` changed, and `gpu_image_stats.py`,
  `gpu_image_pipeline.py`, `gpu_image_compose.py` and `nr_gpu_transport.py` with their tests.
- Preset B in a game: `NR_PRESET=B` beside A, the look and the frame rate. Archive, 10-17 later.
- B's four full-resolution passes, the Linux agent's proposal for the network's speed after the
  staged GEMM's walk (`6ef670e`), are not started. Archive, 11-04 evening.
- The clock hint (`XMX_LOW_LATENCY`, on by default on Linux since 10-18, and kept on by the owner):
  what it costs in power and heat, on battery above all, is unmeasured. Archive, 10-18.
- No Linux release has been published, but `deploy.sh --release` would build one without a 32-bit
  layer, so a 32-bit game under Proton (DoA5) could not use it. Archive, 10-07. Left until a
  Linux release is planned (10-18).
- A daemon started by a short-lived process (a launcher that makes a Vulkan instance before the
  game) ends with that process, and a game whose instance found it alive gets none after: the
  layer starts a daemon only in `vkCreateInstance`. The same on Windows. Not seen in a game, and
  left until it is (10-18).
- Optional: `build_check.py` could hold `build_win.bat` to the same floor (`/DNR_F16C`,
  `/fp:precise`, `/openmp`). The owner does not mind either way. Archive, 10-07.
**Windows**
- NR through OptiScaler (#38): runs in Hogwarts with OptiScaler 0.9.4, from the game's XeSS and
  from its FSR 3; setup takes OptiScaler's XeSS (`d936d63`), in 0.0.5 (archive, 11-05 evening).
  Untested: DLSS input through OptiScaler's spoofing, its DirectX 11 path, a game with no XeSS of
  its own, the cut beside OptiScaler's own D3D12 hooks, and #38's Stellar Blade (cause not known).
  An FSR 3.1 proxy without OptiScaler: designed, not built (archive, 11-05 later).
- The two Reddit posts on 0.0.5, the owner's to post: `work\pr-replies\reddit-0.0.5.md` for
  r/IntelArc and `reddit-0.0.5-technical.md`, with the video and screenshots in
  `work\pr-replies\reddit-0.0.5-media` (archive, 11-06, later).
- The game harness leaves a game's files alone (the owner, 11-06: let the game save its data).
  Killing the game and putting its settings, `pc_info.json` and save list back made it prepare its
  shaders at every start and Epic report a cloud sync conflict; it closes the game through its
  window now, removes only NR's files and reports what the game wrote. `hogwarts-xess-test.ps1`
  must not run again: its clean copies back XeSS 2.0.2.53 from 10-16. Archive, 11-06, early
  morning.
- An Arc A-series path with 8x8 matrix tiles, from OUCO86's A770 port: begun on 11-01 and paused
  at the owner's word, on the local branch `arc-a-n8` (`6be5235`, not on GitHub). Archive, 11-01.
- Where an A770's ~55 us a pass goes: a per-pass profile from an A-series tester
  (`frame_profile.py --warm`, timestamps per pass) would say whether passes or the gaps between
  them. It is half of a frame at 1280x720 there. Archive, 11-03.
- The speed test measures the CPU path (no `NR_` setting reaches its daemon), while the Windows
  release keeps a game's frames on the GPU since 0.0.2. At 1920x1080 its round trip reads 10-20 ms
  worse than a game waits for. Archive, 11-03. Measured on 11-08 at `perf-1108`, network B: with
  `NR_GPU_IMAGE=1` in its daemon's environment (one line, `gpu-image-host`: frames still cross the
  pipe, the passes around the network on the GPU) the round trips are 29.0 -> 24.3 ms at 1280x720
  and 0.4, 57.0 -> 43.1 at 1920x1080 and 0.4, 108.9 -> 92.4 and 219.3 -> 196.2 at 1.0, the network
  the same; a whole GPU-path frame there is ~21 and ~38 ms. The owner's call, since the window's
  numbers move; not changed.
- The GPU's clock: whether Intel's Windows driver also clocks a light network down between
  frames, as Linux's governor did (archive, 10-18). The power mode moved the host passes there
  already (10-15). libxmx's hint is Mesa's alone and does nothing on Windows.
- Holding the GPU's clock up on Windows, as libxmx does on Linux: no per-process hint found.
  Archive, 10-18, Windows. On 11-08 (`perf-1108`, B at 0.4, 1280x720, the pipeline on a device
  buffer, 120 frames, two rounds): a 15 ms gap before each frame makes it 22.9-24.2 ms against
  21.5-21.7 back to back, the network 17.2-18.0 against 16.2-17.1; a second process submitting a
  tiny pass every millisecond through the gaps does not take it back (22.9-24.0, the network
  18.1-19.0), so keeping the GPU busy from outside is not the hint.
- The staged GEMM's loop on Intel's compiler takes 1.2-1.5x Mesa's time where the graph is large,
  and took 1.6-2.1x at the vendor's 1080p and 1440p fields (archive, 11-04 evening). Most of that
  was the driver's walk order, read since `6ef670e`: the GEMM is 1.2-1.3x Mesa's time there now,
  and the residual at K = 64, the QKV gather and the bottleneck's contract are what is left (archive,
  11-04 night). The packed copies took 6-9 %. Archive, 10-02 and 10-03.
- DOOM (2016) through 0.0.1.2 is untested: the owner's own installation from another release is in
  its folder, and its removal was refused by this session's safety check. Archive, 10-15.
- `windows_launch.py`'s Configure Steam, Restore Steam and their wrapper can go, with their tests,
  once no release with those buttons is in testers' hands. The GitHub releases have none. Archive,
  10-09.
- A Vulkan or DirectX 8–11 game that runs elevated, or any with UAC off, never gets the layer; the
  HKLM manifest that would reach it is kept for later. Archive, 10-08 and 10-09.
- DirectX 12 in other games: only Hogwarts Legacy (XeSS 2.0.2.53) has run. Check takes any XeSS
  whose exports NR's proxy carries, and an older XeSS's execute parameters are taken to be laid
  out as 2.0's. Archive, 10-30 evening.
- MSVC: the composition is not vectorised (0.5-1.8 ms behind gcc): MSVC vectorises innermost
  loops only, and inside each pixel loop is a three-channel one (`/Qvec-report:2`, reasons 1106
  and 1303). ~~Three C4068 warnings~~: `NR_IVDEP`, `1c49d04` (below, 11-08). Archive, 10-02, 10-07.
- ~~The GPU path's frame around the network, `perf-1108` (below, 11-08): a game, with the owner
  there, then the owner's word on merging it~~: merged into `newnr` at the owner's word and run in
  Mortal Kombat 11, B 5.5-10 % and A 3 % more frames (below, 11-11, morning). Hogwarts Legacy, for
  the D3D12 transport in a game, has not run it.
- The bottleneck's weights as E4M3 bytes (below, 11-08, evening), in `newnr` and on in Mortal
  Kombat 11's runs (below, 11-11, morning): whether it stays on by default for every GPU on
  Intel's Windows driver; desktop Arc cards and the B580 have not run it.
- `NR_CHANGE_METRIC` (below, 11-08, evening, later): on by default; whether it should be off by
  default, or a row in the panel or the setup window, is the owner's call. Off, it is 0.3 ms of
  B's frame at 1280x720 and 1.0-1.5 at 1920x1080 on the merge (below, 11-11, morning).
- #40 and #41, the panel's `render_scale` floor and its shader at SPIR-V 1.0 (merged on 11-11,
  `7d37976` and `d0a4f65`): in no release yet. Below, 11-11.
- The Vulkan panel under validation: one warning is left, `menu.comp`'s `rgba8` against a
  B8G8R8A8 swapchain. Archive, 11-06.
- The panel open over a game that reads keys by `GetAsyncKeyState`, DirectInput or a gamepad: the
  game still gets them. Archive, 10-31 later.
- The `game-tuning` worktree, `D:\NRonWindows\dlss-nr-tuning`, can go once the owner says so: its
  profile is in 0.0.5. Archive, 11-06, morning.
- Housekeeping: each release root in `D:\NRonWindows\rel` holds ~440 MB of extracted weights, and
  `D:` ran out of space mid-test. Archive, 10-12.

**Planned, not started** (parked by the owner: do not start unasked)
- A FAQ in the README. Later.
- INT8 on the bottleneck: the exact kernel exists (`gemm_staged_int8.comp`) and stays out of the
  graph by the owner's decision; `improve-int8` keeps the branch. Archive, 09-26. Measured
  again on 11-11 (the evening's entry): most of its speed is its weights by column, which the half
  kernel can have exactly.
- The asynchronous live mode exists (`NR_LAYER_ASYNC=1`) and stays off: a frame of latency, over
  the owner's ~15 ms. Archive, 09-25. On 11-11 it is also 6-9 frames a second behind in
  step in Mortal Kombat 11, kept off the shared transport (the evening's entry).
- The DLSS-SR (upscaler) reading is on the local branch `upscaler`, dropped on 2026-09-22.
- llama.cpp issue #13530 keeps cooperative matrices off for every Intel GPU, and this project has
  measurements of an Xe2 iGPU to post there. Put off by the owner, 10-14.
- The Mesa issue on shared-memory sizing: Mesa rounds a workgroup's declaration up, so some
  smaller declarations run on fewer threads. Still so in Mesa 26.2.4 (`src/probe/slm_occupancy`:
  64-256 B at 3.7x the 1 KB time) and in Mesa's `main` on 10-14, where the code is as it was in
  2024. It costs this frame nothing. The draft with the patch is `work/mesa-26.2.3/ISSUE.md`; the
  owner's gitlab.freedesktop.org account is too new to file it yet. Archive, 09-24;
  `notes/improve-shared-memory.md`.

**Leads left untried** (each small; the estimates are in the archive)
- Reading the colour and the previous frame as the bytes they arrive as: ~35 MB less memory
  traffic a 1080p frame (archive, 09-26). Codex's `experiment-bgra8*` branches explored it.
- The narrow blocks' fused feed-forward reading its input once: ~2 % at 720p (archive, 09-26).
- A small-M kernel without the shared-memory round trip, for the bottleneck at small fields
  (archive, 09-25); Xe2's 256-register mode per dispatch, ~6 % at best (archive, 09-27);
  `cl_intel_subgroup_2d_block_io`, untried (archive, "1. Do this first").
- Taking each load out in turn, as window attention's cost was found, for every other kernel that
  runs one subgroup to a workgroup (archive, 09-25).

---

## INT8 in the bottleneck measured again; most of its speed is how it reads the weights, and that is exact; the asynchronous mode in a game (2026-11-11, evening, branch `newnr`)

The owner, 11-11: check what is on `newnr`, whether part of the computation can go to INT8, and
then how the asynchronous mode does. Nothing changed in the code: `newnr` is as the entries below
left it, and every figure here is from timing proxies and scripts kept outside the repository
(`D:\NRonWindows\nr-game\perf-1111\int8` on the Windows laptop). Arc 140V, Intel 101.9150, on
AC, quiet before and after; each case a process of its own, two or three rounds, alternating.

**`newnr` (`b354198`) built from scratch in a new worktree:** MinGW (198 warnings, as before) and
CTest 69 of 69; MSVC `build_win.bat` with no warning and CTest 69 of 69; the seven menu fixtures,
`test_windows_setup.py` 14 and `test_windows_wizard.py` 100 (1 skipped); `frame_replay.py` heads
of A at 320x320 and 1280x720 and of A and B at 320x512 and 768x1344, with the E4M3 weights on and
off, all the morning's; `test_gemm_e4m3.py` both ways, the image suites, the transports, the
daemon, `test_gpu_d3d12.py` with and without the network, `build_xess.bat` and `test_nr_xess`.
B at 0.4 and 1280x720 reads 18.9-19.1 ms a frame today.

**INT8, the bottleneck's four weight GEMMs** (`gemm_staged_int8.comp`, bit-exact against numpy on
this driver too, `test_gemm_int8_staged.py`). A timing proxy in the real frame: each bottleneck
block records the integer kernel four times with its shapes on int8 stand-ins, weights of its own
a block so they come cold, where the graph records its four half GEMMs; the epilogues and the
operands' conversion are left out, so this is the most it could give. B at 512x320, a call:

| ms a call | the graph (E4M3 bytes) | E4M3 bytes by column | INT8 |
| --- | --- | --- | --- |
| contract 64x1024x4096 | 0.229-0.245 | 0.119-0.128 | 0.077-0.084 |
| expand 64x4096x1024 | 0.120-0.139 | 0.085-0.098 | 0.063-0.070 |
| QKV 64x3072x1024 | 0.106-0.118 | 0.069-0.080 | 0.049-0.054 |
| projection 64x1024x1024 | 0.061-0.062 | 0.035-0.037 | 0.021 |
| the device's total | 16.1-17.1 | 14.1-14.9 | 13.4-13.6 |
| a whole frame, 1280x720 at 0.4 | 18.9-19.1 | 17.05-17.4 | 15.8-16.8 |

The frames are two series of three rounds (one base run of 20.4 ms left out; the second series
read 0.3-0.8 ms higher for INT8). A conversion pass before each integer GEMM, the dearest way to
put its operand on a grid, made the INT8 frame 16.2-16.6 ms where it read 15.8-16.0 without.

**What INT8 does to the picture** (`int8_bottleneck.py` with the preset's own blocks, B at
512x288, weights and operands, six frames of Dead or Alive 5, Tekken 7 and Mortal Kombat 1): 6.8
to 13.6 % of the network's effect, 0.46-0.73 levels of 255 on average, a p99 of 2.3-3.8 and a worst
pixel of 6-10, the same on a grid of max/127 and on a power-of-two one. On the power-of-two grid
99.85 % of the bottleneck's weights are exact (they are E4M3 values, four bits of significand
within four binades of their column's largest), so the damage is the operands': 41 % of their
elements are rounded. Two int8 planes an operand would hold them, at two GEMMs for one: no faster
than half.

**Most of INT8's speed is not the integers.** Its kernel keeps the weights by column (N x K) and a
thread reads consecutive K bytes of one column; the half kernel reads K x N by row, where each K
row of a block's 32 columns is a cache line of its own and, at N = 4096, a page of its own. The
half kernel given the same reads (a timing proxy, values wrong: the packed loader reads an E4M3
B's bytes by column, keeps the tile by column in shared memory and loads it column-major) is the
middle column above: 1.7-2.0 ms of the frame, and INT8 0.9 ms beyond it. The same reads for every
staged weight GEMM, half ones too (N > 32): the device's total 13.7 ms against 16.1-16.6, the
staged GEMMs 9.6 -> 7.5 ms.

Built for real it changes no byte: the fragments hold the same values and K runs in the same
order. The kernel has the case already as `transposed` (bit 1), on its scalar loader; what is
missing is the packed loader's version of it, libxmx taking an E4M3 B that way, and the weights
stored transposed. `improve-b.md` found a by-column tile spilling on Mesa and its bottleneck not
waiting on weights, so this is likely Intel's Windows driver's alone, as the E4M3 bytes are.

**The asynchronous mode in a game, as it stands.** Mortal Kombat 11 through DXVK, B at 0.4 and
1280x720 (network 512x320), a release built at this branch's code, the screen on; seven runs of
40 s, the first three cases in both orders, all clean: no refusal or fallback, no daemon left,
the game's folder as before (`async-series.ps1`, `release-tests\async1111-summary.txt`).

| the case | frame path | processed a second | median frame, ms | network, ms |
| --- | --- | --- | --- | --- |
| in step, as a player runs it | `gpu-shared` | 35.5, 39.3 | 25, 23 | 17, 16 |
| `NR_LAYER_ASYNC=1` by hand | `gpu-image-host` | 29.84, 29.84 | 30, 28 | 17, 16 |
| in step on that path (`NR_GPU_TRANSPORT=0`) | `gpu-image-host` | 26.2, 29.3 | 31, 21 | 20, 16 |
| asynchronous as the setup window writes it | `host` | 32.0 | 30 | 19 |

Asynchronous is 6-9 frames a second behind in step today; on 10-12 the two were level at 19-20,
and `docs/RELEASE-QUICKSTART.md` and `docs/WINDOWS.md` still quote that. The mode did not get
slower, in step got faster: `process_frame_gpu` takes only a synchronous frame ("requires
synchronous live every frame"), so an asynchronous frame crosses the pipe as pixels, and the
shared transport is where in step gained since. Like for like on the pipe asynchronous is level
with in step or ahead of it (29.8 against 26.2 and 29.3). Its two runs counted the same 1194
frames, which reads as a ceiling somewhere other than NR's speed; not looked into, and one run a
case is too few to say more. An asynchronous frame on the shared transport does not exist: it
would be new work in the layer and the daemon (a frame in flight on the shared images, its answer
taken a present later), and it would still show a frame late, which is why the mode stays off.
The two pages' figures are not changed here: they are 0.0.5's, and these are a branch's.

**Next on Linux:** nothing from this entry. **Next on Windows:** the owner's word on building the
by-column weights (exact; INT8 stays parked, as on 09-26, unless the owner wants its further 5 %
for a changed picture), and on the asynchronous mode above.

## The network on Intel's Windows driver: three leads measured, none kept (2026-11-11, afternoon, branch `newnr`)

The owner, 11-11: on to the network's speed. Nothing changed in the code; `newnr` is as the entry
below left it. Arc 140V, Intel 101.9150, on AC, quiet before and after; B at 512x320,
`frame_profile.py`, each case a process of its own, two or three rounds.

- **The half accumulator, again** (`gemm_staged.comp` with a float16 accumulator, converted on
  its way to the stage; a timing proxy): whole frames, B at 0.4 and 1280x720, 19.27-19.68 ms
  against 18.81-19.21, inside the day's spread. `phase31`'s verdict holds on this driver.
- **Where the bottleneck's GEMMs stand**, a call: the contract 64x1024x4096 0.235 ms
  (2.2 TFLOP/s), the projection 64x1024x1024 0.063, the expand 64x4096x1024 0.127 (4.4), QKV
  0.108. The feed-forward GEMMs of the C = 128 and 256 levels reach 4.4-4.7 TFLOP/s, so the
  machine's peak here is above the 3.8 the notes carry.
- **The two N = 1024 GEMMs wait on their weights coming cold.** With B read from a footprint
  that stays in the cache (a proxy, wrong values) the contract is 0.153 ms and the projection
  0.045; dispatched twice, the second runs at that speed, so the 4 MB stay cached. That is 0.8 ms
  of a frame, and no exact way took it:
  - the next step's E4M3 bytes loaded into registers before this step's multiply-adds: nothing;
  - reader workgroups as a second layer of the dispatch, a word of each line: nothing on the
    64-deep build, whose 16 KB of shared memory leave them no place beside the 64 blocks that
    multiply; 0.015-0.02 ms a call on the 32-deep build;
  - a dispatch that only reads B, before the GEMM: 0.05 ms for the 4 MB, the GEMM then 0.165,
    0.015 ms a call gained.
  About 0.15 ms of a frame at best. Not kept.
- **A trap for proxies:** a shader that changes what an E4M3 GEMM computes fails `xmxres`'s
  start-up decode check, which turns the E4M3 weights off for the run without a word; the
  expand at 0.147 ms and QKV at 0.125 are its mark. A proxy keyed on `pc.n >= 1024` leaves the
  check's narrow GEMMs right.

**What is left**, each the owner's call: the fused C >= 64 blocks (below, 11-08), by this profile
0.5-1 ms on the 140V, those passes running near 3.3 TFLOP/s already; a split-K for the two
N = 1024 GEMMs, which changes the order of summation and gained nothing on Linux (archive);
INT8 in the bottleneck (Planned, not started), the one lever that cuts the stream of weights
itself. The scripts, the logs and the experiment's patch: `D:\NRonWindows\nr-game\perf-1111\opt`
on the Windows laptop.

**Next on Linux:** nothing from this entry. **Next on Windows:** as the entry below.

## `perf-1108` and `e4m3-weights` on `newnr`: merged, green on Windows, and what they give today (2026-11-11, morning, branch `newnr`)

The owner, 11-11, before sleeping: move the two branches of 11-08 onto `newnr` and see how they do
there. Merged in a scratch worktree as `e1be49f` (parents `c0660f2` and `9aefe63`). `newnr` is
local and ahead of GitHub; nothing is pushed, and `master` and `alchemist` stay at `c0660f2`.

**The merge.** Two files conflicted. `Makefile`: the branch had added `test_gemm_e4m3.py` to
`make test`'s list, which #39 moved into `src/tools/run_tests.py`; the check is in the runner's
list now, and in CTest as `gpu_gemm_e4m3`. This file: `newnr`'s, with the branch's open items and
its three entries of 11-08 word for word (below). The merged code names nothing #39 removed.

**Checked on Windows** (Arc 140V, Intel 101.9150, on AC):
- MinGW (gcc 16, layer off): the build, with the 198 warnings `newnr` has, and CTest 69 of 69.
- MSVC `build_win.bat`: no warning at all, the three C4068 gone with `NR_IVDEP`. CTest on its
  libraries: 69 of 69. The seven menu fixtures. Under the native Python, `test_windows_setup.py`
  14 of 14 and `test_windows_wizard.py` 100, 1 skipped.
- `frame_replay.py` on the MSVC libxmx: A's heads at 320x320 and 1280x720 are October's, with the
  E4M3 weights on (this driver's default) and with `XMX_E4M3_WEIGHTS=0`; A's and B's at 320x512
  and 768x1344, on and off, are 11-08's. `test_gemm_e4m3.py`: its 82 cases, by default and forced.
- `test_image_*`, `test_image_pipeline.py --full-model` (8 frames, 5 of them along motion, and
  again with the metric off), `test_gpu_transport` (50), `test_daemon`, `test_settings_live` (17),
  `test_gpu_d3d12.py` with and without `--network`.
- `build_xess.bat` and `test_nr_xess`. `deploy.bat --release`, and that release through
  `nr-game\standin-release-check.ps1`: 726 frames, none rejected, the speed test A/71 and B/31,
  Save report 19 checks, none failing, the folder as before.
- vkcube under the validation layer with the panel open and shut: 0 errors, no leak line.
  `test_gpu_present.py --run --sync-validation`, the panel shut, open and in a corner: 0 errors.

**Whole frames on the GPU path** (the daemon's pipeline on a device buffer, render scale 0.4;
`newnr` from `fe2c33c`'s build, the same code; each run a process of its own, three rounds, the
second in reverse order; `quiet-check.ps1` quiet before and after):

| ms a frame | `newnr` before | the merge | with `XMX_E4M3_WEIGHTS=0` | with `NR_CHANGE_METRIC=0` |
| --- | --- | --- | --- | --- |
| B, 1280x720 | 19.78-19.91 | 18.75-19.00 | 18.63-18.80 | 18.41-18.85 |
| B, 1920x1080 | 36.77-37.12 | 33.43-33.83 | 34.59-34.84 | 32.32-32.42 |
| A, 1280x720 | 35.65-37.66 | 34.17-36.59 | 34.54-35.74 | 34.01-34.14 |

By round, 0.8-1.2 ms of B's frame at 1280x720 (5 %) and 2.9-3.6 ms at 1920x1080 (9 %), 4.4-4.7
(12 %) with the metric off. The E4M3 weights are 1.0-1.2 ms of that at 1920x1080 and nothing at
1280x720; the metric off is 1.0-1.5 ms at 1920x1080 and 0.3 at 1280x720, where the bands overlap.
A's bands overlap too: its second round was disturbed for both builds.

**11-08's 2 ms at 1280x720 was partly the day.** The same four builds side by side, B at 1280x720,
three rounds: `5836d29` as built on 11-08 19.85-19.90 ms, `newnr` 19.79-19.97, `9aefe63` as built
on 11-08 18.18-18.94, the merge 18.64-18.86. So #39 changed nothing in this frame, the merge runs
as the branch did, and the base that read 23.17-24.13 ms on 11-08 reads 19.9 today, on the same
driver and a quiet system both days. Why is not established.

**Where the merge's frame goes** (B, 1280x720, 18.8 ms): the network 15.1-15.4; the four submits
around it 0.15 (the letterbox check), 0.53 (the decode and the cut test), 0.53 (the history's
resize and the features) and 1.3 (the finite check, the composition and the metric; 1.0 without
the metric), 2.5 together; Python
between libxmx's calls 0.6-0.8. At 1920x1080 the four are 0.4, 1.25, 1.14 and 3.1 (1.8 without
the metric) around a network of 28.2. The network is 81-84 % of a frame now.

**In Mortal Kombat 11** (Steam, DXVK through the proxy, 1280x720 at render scale 0.4, network
512x320, frames on the GPU path; a release built at `fe2c33c` against one built at the merge;
`nr-game\perf1108-series.ps1`, two rounds, the second in reverse order; the screen on and the
session unlocked, the owner asleep):

| frames processed a second (the daemon's median frame) | `newnr` before | the merge |
| --- | --- | --- |
| B | 37.16 and 38.71 (23 and 22 ms) | 40.95 and 40.84 (21 and 21 ms) |
| A | 23.42 and 24.39 (39 and 38 ms) | 24.17 and 25.12 (38 and 36 ms) |

B gains 5.5 and 10 % by pair, A 3 %. All eight runs kept every frame on `gpu-shared`, refused
none, closed the game through its window, ended their daemon with it and left the game's folder
as before. Part of the first merged B run's window shared the disk with a `du` of this session's.

**Not run:** Hogwarts Legacy, so the D3D12 transport in a game; Linux.

**Next on Linux:** nothing until `newnr` is on GitHub; then Open items, Linux. **Next on
Windows:** the owner's look in a game, then the word on pushing `newnr`, with `master` and
`alchemist` after it.

## Checked on Windows: PR #39 at `fe2c33c`, then merged (2026-11-11, latest, branch `newnr`)

The owner, 11-11, back from the Linux run: #39 can be merged once Windows has run it. The Linux
session had merged #40 and #41 already (`d0a4f65`, `7d37976`), so `newnr` (`c22f798`) was inside
`cleanup` and the PR's tree is the one that merges. Tested in a scratch worktree at `fe2c33c`, on
the Arc 140V on AC power; no timing was taken.
- MinGW (gcc 16, layer off): the build, with the 198 warnings `e41d2b3` has, and CTest 68 of 68, two
  of them skipped until the MSVC libraries exist.
- MSVC `build_win.bat`: the three known C4068 and nothing else. CTest on its libraries: 68 of 68.
- The seven menu fixtures. Under the native Python, `test_windows_setup.py` 14 of 14 and
  `test_windows_wizard.py` 100, 1 skipped.
- `frame_replay.py`, network A, on the MSVC libxmx: the heads and changed heads are October's,
  `e62005b8…`/`e7c72789…` at 320x320 and `c217fd2f…`/`95f46f67…` at 1280x720, as on 11-08. So
  with #40 and #41 in, removing `xmx_rec_qkv`, `gemm_smallm.comp` and the two switches still changes
  no byte the network writes on Intel's driver.
- `build_xess.bat` and `test_nr_xess` pass, with `nr_menu_floor.c` linked; `deploy.bat --release`
  builds a release of `fe2c33c`.
- vkcube under the validation layer with the panel open and shut: 0 errors and no leak line.
  `test_gpu_present --panel open|corner --sync-validation`: 0 errors.

**Merged** from the Windows session at the owner's word, 11-11 03:39 +0900, as a merge commit
`3d933b0` (parents `c22f798` and `fe2c33c`): `newnr`'s tree is the tested one, byte for byte.
`master` and `alchemist` are level with `newnr` from this entry on.

**Next on Linux:** fetch; nothing is owed for #39. **Next on Windows:** nothing. `build_win.bat`
and `build_xess.bat` run from `newnr` before the next game.

## PR #39 on Linux: green on a GPU, this file cut to 91 KB, and `make test` printing 5 KB (2026-11-11, later, branch `cleanup`)

The owner, 11-11: #39 is there so that tests and records nobody needs stop filling a session's
context, and two more things were to be done for that. `newnr` (`c22f798`, with #40 and #41) was
merged into `cleanup` first, `f69ee5d`: only this file conflicted, and both sides' lines are kept.

**What a session pays for, before and after.**
- The brief: 31 KB to 9 KB, in this PR since 11-05.
- This file: 138 KB to 91 KB (`827d7eb`). The dated entries of 11-01 to 11-04 went word for word
  to `archive/HANDOFF-2026-11-01-to-2026-11-04.md`, 37 KB, and with them the 40 open items that
  were struck through, 11 KB. What six of those still left open is a line of its own now, under
  Linux and Windows. The pointers to the moved entries say "archive".
- `make test` in the terminal: 122 KB to 5 KB (`dff4ad5`). `run_tests.py` prints one line a check,
  the last 40 lines of a failing check's output with its log's path, and the summary, which now
  names every skip. `make test TEST_ARGS=--verbose` prints every check whole, as before; `--quiet`
  is gone. Tried on stand-ins: a pass, a skip (exit 77) and a failure of 100 lines.

**Checked on Linux, the branch's first run on a GPU.** No daemon, swap empty, a quiet CPU; Mesa
26.2.6.
- Before the two changes, on `cleanup` with `newnr` merged: `make test` 70 of 70 in 1 min 39 s,
  where the old recipe took 2 min 50 s on `newnr` the same night; `XMX_STAGING=1 make test-gpu`
  40 of 40, the seven suites that print the mode saying `staging=True`; CMake with no warning;
  CTest 69 of 69. So `attention.comp`, libxmx without `xmx_rec_qkv` and `nr_resident` hold on Mesa.
- At `dff4ad5`, the same again: 70 of 70 in 1 min 29 s, 40 of 40, CTest 69 of 69. Between the two
  runs only the runner, its two descriptions and the notes differ.
- The parallel run needed neither `--serial` nor `NR_TEST_JOBS=1` here.

**Not run:** Windows since `4cd720f`. What changed for it since is `newnr`'s #40 and #41, which
Windows built at their heads, and the runner's output.

**Left for a later cut** (made on 11-11, later, from the Windows session): "Where things stand"
carried each branch's history, 9 KB; the reference sections 0-7 are 18 KB, the traps half of
that.

**Pushed at the owner's word**, with the PR's description brought up to date. The owner, 11-11:
the merge waits for Windows.

**Next on Windows:** `git fetch`, then `cleanup` at GitHub's head; `build_win.bat` and CTest once
are enough, since nothing Windows compiles changed after its own check but #40 and #41. Write the
result here on `cleanup`. **Then** the owner's word on merging #39 into `newnr`.
**Next on Linux:** nothing for #39.

## Checked on Linux: PR #40 and #41 together, and `newnr` at `127f6dc` (2026-11-11, branch `alchemist`)

The owner, 11-11: the work is on #40 and #41, Paimonshen's two against `newnr`. #40 (`e41d2b3`)
keeps the panel's `render_scale` from going below the last step on the cheapest network field;
#41 (`ca6b020`) builds the panel's shader as SPIR-V 1.0 and drops its two device requirements.
Windows found both ready on 11-10, with one commit of ours pushed to each branch (the comments in
the PRs; this file had no word of either). The Linux branches were fast-forwarded to GitHub's
`127f6dc`.

**Tested:** `newnr` (`127f6dc`) with both heads merged, a local merge never pushed. They overlap
only in `tools/build_win.bat` and merge with no conflict. No daemon, swap empty, a quiet CPU;
Mesa 26.2.6.
- `make test` passes, in 2 min 50 s. `XMX_STAGING=1 make test` passes, in 2 min 11 s, the seven
  GPU suites that print the mode saying `staging=True`. Skipped as before: the Windows-only cases,
  and the GPU image path's parity tests on Mesa. No compiler warning.
- CMake builds with no warning, and CTest passes 68 of 68.

**That run exercises little of either PR**, so each was checked apart.
- #40: `MENU_TESTS` is empty off Windows, CTest's `layer_menu_rows` is Windows-only, and no Linux
  test calls `nr_frame.lowest_effective_scale`. So `nr_menu_floor.c` was built with gcc (`-Wall
  -Wextra`, no warning) through `test_menu_rows.py`'s own probe and set against the Python rule:
  no difference at the test's 19 sizes and at 400 more, 20 widths from 320 to 7680 by 20 heights
  from 180 to 4320. The floor is 0.25 at 1280x720, 0.15 at 1920x1080 and 0.30 at 1024x768.
- #41: every change to `nr_layer.c` is inside `_WIN32`. The panel's shader was built here as
  `:menu_shader` does (glslang 16.6.0, `--target-env vulkan1.0`): `menu.spv` is SPIR-V 1.0 with
  `Shader` alone, `menu_rgb10a2.spv` adds `StorageImageExtendedFormats`, and both pass `spirv-val
  --target-env vulkan1.0`.

**Not run:** either PR alone on `newnr`.

**The same run is Linux's first of what `newnr` gained since `c744581`**, so four items under
Open items, Linux, are struck: the daemon keeping the panel's unsaved edits, the `hogwarts`
profile, the frame counter, and the release's own Python.

Both results are in the PRs as comments, at the owner's word.

**Then, at the owner's word.** #41 was merged into `newnr` as `d0a4f65` and #40 as `7d37976`, from
the Linux session. The tree of `7d37976` is the tested one, byte for byte. This entry went on top,
and `master` and `alchemist` were fast-forwarded to `newnr`, on GitHub and here.

**Next on Linux:** PR #39 (below, 11-09), whose branch now needs `newnr` merged in again.
**Next on Windows:** `git fetch`, then fast-forward `master`, `newnr` and `alchemist` to GitHub's.
`build_win.bat` and `build_xess.bat` changed, so build again before the next game.

## PR #39 waits for Linux (2026-11-09, branches `master`, `newnr`, `alchemist`)

The owner, 11-09: leave the Linux side a message on #39. Its branch `cleanup` is on GitHub at
`297e13a` since 11-08, pushed at the owner's word: `newnr` (`5836d29`) merged in, three tests fixed,
Windows green. That branch's entry "Checked on Windows: `cleanup` with `newnr` merged in" was
written before the push, so its "nothing is pushed" is out of date. The PR is MERGEABLE, still a
draft.

**Next on Linux:**
1. `git fetch origin`, then `git switch cleanup` (`git switch -c cleanup --track origin/cleanup`
   where there is no local branch) and `git merge --ff-only origin/cleanup`.
2. Stop the daemon. On this branch `make test` runs through `src/tools/run_tests.py`, in parallel;
   `make test TEST_ARGS=--serial` is the old order one check at a time, if memory or timing
   misbehaves. Then `XMX_STAGING=1 make test` (or `make test-gpu`), then CTest (69 entries).
3. What changed for Linux: `attention.comp`, libxmx (no `xmx_rec_qkv`), `nr_resident.py`, the test
   runner, and the three tests of `4cd720f` (`test_ui_resize.py`, `test_windows_setup.py`,
   `test_windows_wizard.py`), which run on Linux too.
4. Write the result on `cleanup` as an entry "Checked on Linux: `cleanup`" and commit. Pushing it,
   and merging #39 into `newnr`, are the owner's word.

**Next on Windows:** nothing for #39.

## The change metric as a switch, `NR_CHANGE_METRIC` (2026-11-08, evening, later, branch `e4m3-weights`)

The owner, on the change figure the entry below found costs a frame time: "сделаем возможность
включения и отключения такого параметра". `change` in the daemon's frame lines (how far the answer
moved the game's frame) is read by nobody but a person reading the log; the tools and tests only
carry example lines.
- `NR_CHANGE_METRIC=0`, in the daemon's environment or the release's `dlss-nr\nr-env.txt` (the
  proxies set its lines, and the panel's and the setup window's writes keep lines that are not
  theirs), leaves the figure out on every path, and the line reads `change off`. On by default.
- The GPU image path (`gpu_image_pipeline.CHANGE_METRIC`, `GPUImagePipeline.change_metric`): the
  composition is asked for no float32 copy (`image_compose.comp` flag `COMPOSED`, 32;
  `GPUImageComposer.record(composed=)`, the buffer made only when first asked for) and the
  reduction is not recorded. The CPU path skips its quarter-row mean (`nr_daemon.CHANGE_METRIC`).
- **The same bytes:** `test_image_pipeline.py --full-model` runs its eight-frame sequence with the
  metric off as well (wire and history exact); `test_image_compose.py` composes every case of its
  table again without the copy (wire bytes and history equal, the copy's buffer untouched); a host
  check holds that neither the copy nor the reduction is recorded.
- **Measured** (B at 0.4, 1280x720, the pipeline on a device buffer, three processes each,
  alternating, with the E4M3 weights): the composition's submit 2.00-2.11 -> 1.38-1.41 ms, the
  frame 21.70-22.06 -> 20.70-21.70.

## The bottleneck's weights as their E4M3 bytes (2026-11-08, evening, branch `e4m3-weights`)

After the entry below, still at the owner's "look for speed": on `e4m3-weights`, a local branch
on `perf-1108`, in the same worktree; nothing pushed.

**Why.** Network B at 512x320, 1280x720 at render scale 0.4: the bottleneck's eight blocks are
4.5-5.0 ms of 15.6-16.6 on the device, four GEMMs at 64 tokens streaming 25 MB of half weights
a block. Reading half of B's bytes (a timing proxy, every B row read twice) took them to 3.9-4.1 ms
on Intel's driver, where `improve-b.md` found 7 % on Mesa: here they wait on those bytes. Every
value of the 84 large matrices of A and B but the four attention-bias tables is an E4M3 value
(the DLL ships them as E4M3), so the bytes are the weights, exactly.

**The change** (all bit-identical):
- `gemm_staged.comp`, bit 0x4000000: B is E4M3 bytes, read by the packed loader (eight bytes a
  thread where it took sixteen) and decoded to halves before the shared-memory store. A byte's
  sign and magnitude go to a half's places, the half reading the value times 2^-8 (an E4M3
  subnormal a half subnormal), and a multiply by 256 gives the value: exact with float16
  subnormals kept. Each four columns' bytes are stored 0 2 1 3, so one shift and two masks
  make a pair of halves from a word and no byte moves alone (`e4m3x4_halves`); the first
  version, which moved each byte, took 0.3 ms off the bottleneck where this takes 0.4-0.75.
- `libxmx.c`: `xmx_buf_mark_e4m3` marks a buffer as E4M3 bytes, `record_gemm` sets the bit for
  such a B, takes its offsets in bytes, sends it to the staged kernel at any M, and refuses it
  where the packed loader cannot read it (transposed, gathered, a batch, unaligned).
  `xmx_e4m3_weights` is on for Intel's Windows driver, where float16 subnormals are kept and the
  packed loader is on; `XMX_E4M3_WEIGHTS=0|1` sets it.
- `xmxres.py`: `Runtime.buffer_e4m3` (the bytes in that order, `e4m3_codes`, or None where a
  value is no E4M3 one), and a check at each runtime's start, 7 ms: an identity times all 254
  values on each staged build the bottleneck uses, bytes against half, or the weights stay half.
- `nr_resident.GlobalBlockWeights` keeps the four GEMMs' weights that way where the runtime may,
  and the daemon's `runtime options` line names `XMX_E4M3_WEIGHTS=0|1`, so a Save report says
  which ran.

**The same bytes:** `test_gemm_e4m3.py` (new; 82 cases: the bottleneck's shapes and a partial and
a 32-row block, plain, published, the gate, the residual with both skips and the ViT's QKV
epilogue, specialised and not, B holding every E4M3 value); the heads of A and B at 320x512 and
768x1344, original and changed input, with `XMX_E4M3_WEIGHTS=0` and `=1`; the 30 GPU suites of
CTest's list, `test_image_pipeline.py --full-model`, `test_gpu_transport`, `test_gpu_d3d12.py
--network`, `test_daemon`. MSVC `build_win.bat` with no warning.

**Measured** (Intel 101.9150, quiet, each in a process of its own, alternating):

| | half | E4M3 |
| --- | --- | --- |
| B 512x320, the bottleneck's four GEMMs (`frame_profile`, 6 processes each) | 4.5-5.0 ms | 4.1-4.4 |
| B 512x320, the device's total | 15.6-16.6 | 15.3-16.1 |
| B 768x448 (1920x1080 at 0.4), the bottleneck / the device | 7.0 / 29.5-30.4 | 5.5-5.7 / 28.2-28.3 |
| A 512x320, the bottleneck / the device | 5.1-5.2 / 30.8-32.0 | 4.3 / 28.8-29.2 |
| B, the pipeline's frame at 1280x720, network (4 processes) | 15.95-16.59 | 15.91-16.13 |
| A, the same (3 processes) | 30.30-30.61 | 29.79-30.10 |

So 0.3-0.5 ms of the network at 1280x720 and 0.4, more at 1920x1080, and 96 MiB less on the
device (294 -> 198 MiB for B at 320x512). The 32-row deep build stays the right one for the
contract with E4M3 too (1.81-1.88 ms against 1.98-2.09 on 64-row blocks).

**Not run:** a game; Linux; Arc A-series and Battlemage cards, which take it by default on the
same driver, and whose start-up check is the only thing between them and a wrong decode.

**Next on Linux:** Open items. **Next on Windows:** a game with the owner, with `perf-1108`.

## The GPU path's frame around the network, 9 % less; what the other ports do (2026-11-08, branch `perf-1108`)

The owner, away for the afternoon: finish what is left, then look for speed, and see what the AMD
ports do. Everything here is local, on `perf-1108` from `newnr` at `5836d29`, in a worktree of the
session's; nothing is pushed.

**What the other ports do.** mochizuki's DLSSNR-AMD left GitHub with its author's account and is
on GitLab (`gitlab.com/mochizuki0323/DLSSNR-AMD`, MIT, 0.0.8.3 of 11-06). Its fast networks are
not the vendor's: `*_OPTIMIZED` and `CC_SMALL_FP8_EXPERIMENTAL` refit the MLPs to ReLU, replace some
decoder attention with fixed tables and put layers in int4, so the picture moves; not for this
project. Its one exact trick, heads whose attention is the same for every input skipping Q and K,
finds nothing here: no head of A or B has a scale of 0 (the smallest are 0.0145 and 0.21), and
after the E4M3 publish no query is all zero. `chuanloop/dlssnr-b390` (MIT, 11-06) is an AMD port
moved to an Arc B390 (Xe3): per-level fused kernels (QKV, norms, attention and projection in one
workgroup at C = 64 and 128, where our `window_block` stops at 32), a fixed ladder of 37
resolutions, and 39 of its shaders derived from NVIDIA's runtime, which it does not ship. It
publishes no timings. `improve-b.md` already tried the nearest thing (a level-1 block whole,
no faster).

**Where a played frame's time goes** (network B at 512x320, 1280x720 at 0.4; `frame_profile.py
--warm 25 --runs 5`, Intel 101.9150, quiet): 15.4-17.3 ms on the device, the staged GEMM 60 %;
the bottleneck's four GEMMs at 64 tokens are 28-31 % of the frame and stream their weights at
26-55 GB/s. The rule `improve-b.md` set on Mesa for those M = 64 GEMMs holds on Intel's driver
too: every other routing, tried through a local switch (all of them on 32-row blocks, the 64-deep
step everywhere or nowhere, none on 32 rows), made a call slower (the contract 0.23-0.25 -> 0.32
ms off the 32-row deep build; the expand 0.14-0.16 -> 0.21-0.26 and the QKV 0.12-0.13 ->
0.16-0.19 on 32-row blocks) and the frame 0.5-1 ms slower; the switch is not kept.
`XMX_STAGED_WALK_Y` makes no difference at this field.

**Around the network the GPU path spent 5.5 ms of a 1280x720 frame, in eight submits** (B at 0.4,
the daemon's pipeline on a device buffer, timed submit by submit). Four exact changes, all bytes
the same:
- `268d6d7`: the cached letterbox check (`image_stats.comp` mode 3) ran eight working threads, each
  reading a line a pixel at a time: 0.87 ms. A workgroup a line now, 0.21 ms; modes 0 and 3 word for
  word the old ones on 18 frames from 1x1 to 2560x1440.
- `71d21ad`: the history's three buffers trade places instead of three full-frame copies (0.74 ms
  at 720p, 1.42 at 1080p). `NR_HISTORY_SWAP=0` is the comparison.
- `61c3de9`: three submits fewer, where the CPU decides nothing between them: the cut test (or the
  follower) with the decode, and the head's finite check, the composition and the change metric in
  one. `GPUImageStats.record_delta`/`record_nonfinite` record into the caller's commands on scratch
  of their own.
- `95bb757`: the composition writes the answer straight into the transport's shared output; the
  transport copied it there in a submit of its own, on both GPU transports.

Results, against `5836d29`, three processes each alternating, quiet:

| | before | after |
| --- | --- | --- |
| pipeline frame, B 0.4, 1280x720 (mean of 150) | 23.17-24.13 ms | 21.14-21.92 |
| pipeline frame, B 0.4, 1920x1080 | 42.15-43.47 | 38.25-38.75 |
| layer present, `test_gpu_present.py --run`, 1280x720 (median) | 41.08-44.08 | 37.95-39.41 |
| the same at 1920x1080 | 70.59-74.04 | 67.15-68.01 |

**The same bytes:** `test_image_pipeline.py --full-model` (8 frames against the CPU path, the history
read after each); ten 1280x720 frames with motion through `1c49d04`'s pipeline and through these
(answers and the three history buffers byte for byte; history, motion, cut and change the same to
the last digit); `test_gpu_d3d12.py --network` (the client's frames changed the same channels by
the same levels). Also `test_image_*`, `test_gpu_transport` (50), `test_daemon`,
`test_settings_live`, `test_gpu_d3d12.py`, and `test_gpu_present.py --run` under synchronization
validation, the panel shut, open and in a corner (0 errors, every frame on the GPU path). The
release built from this branch passes `nr-game\standin-release-check.ps1` (723 frames, none
rejected; the speed test A/71 and B/31; Save report 19 checks, none failing; the folder as before).
`test_image_pipeline.py --full-model` gains the motion case (`efd4a6d`): eight frames, five along
motion, byte for byte against the CPU path. MinGW (gcc 16): the build and CTest 67 of 67; MSVC `build_win.bat` with no
warning at all, and CTest 67 of 67 on its libraries.

**Left, for the owner:** the change metric (`stats.delta(composed, active)`) is a number in the
frame log, and the composer's float `composed` buffer exists only for it. In whole frames the
reduction alone costs 0.2 ms at 720p and 0.3-1.0 at 1080p (three processes each, without it against
with it), and the reduction and the buffer's write together 0.4 ms at 720p (21.57-21.74 ms against
20.99-21.58, four processes each) and ~0.3 at 1080p, where the bands overlap; keeping its last bits needs the reduction as it is, so
computing it elsewhere, less often, or not at all changes the log, not the picture. The
bytes-for-the-previous-frame lead (Leads left untried) would touch three shaders. The acquire and
release waits are the transports' own synchronisation, and the layer's own capture and return are
0.07 and 0.4 ms at 720p (`layer-timing.jsonl`).

**The structural lead, for the owner** (days of work, exact if each fused kernel keeps its passes'
order). `Ashley-QwQ/GlassBox-NR-Anatomy-public` (Apache-2.0, a dissection of the 310.8 DLL)
lists NVIDIA's launches: every Swin block of the C = 32, 64, 128 and 256 tiers is one launch
(`cc_tinlayout_fused_swin_{1h_32,2h_64,4h_128,8h_256}`, a first, chained and last-with-downsample
variant each), the block's tokens kept on chip. Here a C = 32 block is two passes (`window_block`,
`ffn_fused`) and a C >= 64 block about five (the QKV GEMM with the window gather, window attention,
the projection with its residual, the gate and the contract). Of B's 15.2 ms at 512x320 listed by
call site, the bottleneck is 4.7, the C >= 64 levels 4.7, the C = 32 window blocks 2.7 and their
feed-forward 2.3. Fused C = 64-256 blocks, as the Xe3 port has per level, are where 1-2 ms of a
played frame could come from; `improve-b.md`'s level-1 attempt (the C = 32 feed-forward inside the
window block, no faster) is the warning: the feed-forward's weights shared over 256 tokens cost
less than over a window's 64.

**Tried and not kept** (all exact):
- **A pass timed alone misleads here.** The features pass from the inner image the decode already
  made, in two 16-byte stores a pixel, read 0.45 -> 0.23 ms at 720p and 0.87 -> 0.52 at 1080p
  submitted alone, and nothing in whole frames: 1080p 37.4-37.7 ms before against 37.5-38.3 after
  over two sets of three processes. Four pixels an invocation for the RGB32 decodes, three 16-byte
  stores, no better either, and slower where they resample. A lone submit lets the GPU's clock
  fall between dispatches; whole frames in separate processes are what count.
- **The barrier between passes costs 0.4-2.6 us** on Intel's driver (200 dispatches in a submit,
  with and without), and synchronization2's barrier on storage accesses alone is the same; B's frame
  at 512x320 15.4-16.0 ms either way. A pass's fixed cost at small fields is the dispatch's own.
- The M = 64 routings and `XMX_STAGED_WALK_Y` (above).

Also: `1c49d04`, `NR_IVDEP` (Open items).

**Next on Linux:** `make test` in both memory modes and CTest, once `perf-1108` is on GitHub (Open
items). **Next on Windows:** a game with the owner, Mortal Kombat 11 at 0.4 before and after (and
Hogwarts for the D3D12 transport); then the owner's word on merging and pushing.

## Checked on Windows: `cleanup` with `newnr` merged in (2026-11-08, PR #39)

The owner, 11-08: look at the open pull requests and start on them. `cleanup` was cut from `newnr`
at `0456e99`, and `newnr` has 26 commits more, so the merge is what was tested: `newnr` (`5836d29`)
into `cleanup`, conflicting only here, every hunk kept from both sides (`683a944`).

**Three tests changed** (`4cd720f`). MinGW CTest failed 3 of its 68 entries:
- `layer_ui_resize`, revived on this branch, looked for `WaitForFences` right before the UI
  image's resize. Since `476d5f0` that wait is `menu_drain`, which waits on every slot's fence,
  and the check now asks for that.
- `tools_windows_setup`, new in CTest: `setup.bat` refuses a Python without NumPy and safetensors,
  and MSYS2's has no safetensors. Its install test failed, and nine others passed only because the
  refusal comes first (line 219) and looks like the failure each tests, exit 3 with an ERROR
  line. The class skips under such a Python now, as `test_setup_proxy.py`'s batch tests do.
- `tools_windows_wizard`, new in CTest: one fixture compared paths, and MSYS2's Python joins them
  with `/`; it compares names now. It failed by hand under MSYS2's Python before (11-06).

**Then everything passed:**
- MinGW (gcc 16): the build, and CTest 68 of 68. The build's warnings are all in files this branch
  does not touch.
- MSVC `build_win.bat`: the three known C4068 and nothing else. CTest on its libraries: 68 of 68.
- Under a native Python with both packages: `test_windows_setup.py` 14 of 14, and
  `test_windows_wizard.py` 100, 1 skipped.
- `frame_replay.py`, network A, on the MSVC libxmx: the heads and changed heads are October's,
  `e62005b8…`/`e7c72789…` at 320x320 and `c217fd2f…`/`95f46f67…` at 1280x720. So removing
  `NR_JOINT_QKV` and `NR_FUSE_BRANCHED_FFN` changes no byte the network writes on Intel's driver.

The merge and the test change are on the local branch `pr39-merge`, in the Windows checkout's
repository; nothing is pushed.

**Next on Linux:** once `pr39-merge` is on GitHub's `cleanup`, or fetched from
`D:\NRonWindows\dlss-nr`: `make test` in both memory modes and CTest. **Waiting for the owner:**
the push to `cleanup`, then the merge into `newnr` once Linux is green.

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

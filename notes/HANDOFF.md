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
`HANDOFF-2026-11-01-to-2026-11-04.md`, with the Open items closed by 11-11 after them. It is
history: do not read it to start a task. Open it only when a line here points there, or before an
optimisation, to see whether it was tried and not kept. `notes/INDEX.md` says which note settles
what.

Keep this file short. Every session loads it whole and sends it again with every request it
makes, so its size is paid for on every turn, and `publish_check` refuses a file over 200 KB. When
the dated entries grow long, move the older ones the same way, into a new archive file: on 11-01
the entries before 10-30 went, and the file fell from 156 KB to 69 KB; on 11-04 those before
11-01, from 100 KB to 77 KB; on 11-11 those before 11-05 and every struck open item, from 138 KB
to 91 KB. A closed open item is struck through where it stands, and goes to the archive at the next
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
    leftovers removed; PR #39 into `newnr`, run on a GPU on both systems (below, 11-05 and 11-11,
    later);
  - ~~`claude/weights-only-quantization-w0o2wh`: one HANDOFF commit (`f06277d`) that reached GitHub
    by accident on 11-01~~: gone from GitHub by 11-04, 08:07 UTC. Its text is carried here (archive,
    11-01, latest), and the branch is saved in a bundle in `D:\NRonWindows\backups` on Windows
    (archive, 11-04).

  The other seven were deleted at the owner's word: `dx12`, `windows`, `improve`, `improve-e4m3`,
  `improve-fusions`, `improve-release` and `experiment-bgra8-pipeline`. All were already in
  `master` or `newnr`, except one merge commit and two of Codex's BGRA8 experiments. A bundle of
  all seven is in `D:\NRonWindows\backups` on Windows. `dx12` went into `newnr` on 10-30, a
  fast-forward.
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

- **PR #39, branch `cleanup`**: green on Linux at `dff4ad5` and on Windows at `4cd720f`, with this
  file cut and `make test` quiet since, and pushed (below, 11-11, later). Windows builds and runs
  CTest at that head once more; then the owner's word on merging it into `newnr`.

**Linux**
- ~~**PR #39, branch `cleanup`** (on GitHub since 11-08: `newnr` merged in, Windows green):
  `make test` in both memory modes and CTest at its head, the code of `297e13a`: the first run on a
  GPU of its runner, `attention.comp`, libxmx and `nr_resident`, and three tests changed (below,
  11-09 and 11-08; 11-05)~~: green at `dff4ad5`, with `newnr` (`c22f798`) merged in (below, 11-11,
  later).
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
  from its FSR 3; setup takes OptiScaler's XeSS (`d936d63`), in no release yet (below, 11-05
  evening). Untested: DLSS input through OptiScaler's spoofing, its DirectX 11 path, a game with no
  XeSS of its own. An FSR 3.1 proxy without OptiScaler: designed, not built (below, 11-05 later).
- The two Reddit posts on 0.0.5, the owner's to post: `work\pr-replies\reddit-0.0.5.md` for
  r/IntelArc and `reddit-0.0.5-technical.md`, with the video and screenshots in
  `work\pr-replies\reddit-0.0.5-media` (below, 11-06, later).
- An Arc A-series path with 8x8 matrix tiles, from OUCO86's A770 port: begun on 11-01 and paused
  at the owner's word, on the local branch `arc-a-n8` (`6be5235`, not on GitHub). Archive, 11-01.
- Where an A770's ~55 us a pass goes: a per-pass profile from an A-series tester
  (`frame_profile.py --warm`, timestamps per pass) would say whether passes or the gaps between
  them. It is half of a frame at 1280x720 there. Archive, 11-03.
- The speed test measures the CPU path (no `NR_` setting reaches its daemon), while the Windows
  release keeps a game's frames on the GPU since 0.0.2. At 1920x1080 its round trip reads 10-20 ms
  worse than a game waits for. Archive, 11-03.
- The GPU's clock: whether Intel's Windows driver also clocks a light network down between
  frames, as Linux's governor did (archive, 10-18). The power mode moved the host passes there
  already (10-15). libxmx's hint is Mesa's alone and does nothing on Windows.
- Holding the GPU's clock up on Windows, as libxmx does on Linux: no per-process hint found.
  Archive, 10-18, Windows.
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
- MSVC: the composition is not vectorised (0.5-1.8 ms behind gcc), and `_Pragma("GCC ivdep")`
  gives three C4068 warnings that an `NR_IVDEP` macro would silence. Archive, 10-02 and 10-07.
- #40 and #41, the panel's `render_scale` floor and its shader at SPIR-V 1.0 (merged on 11-11,
  `7d37976` and `d0a4f65`): in no release yet. Below, 11-11.
- The Vulkan panel under validation: one warning is left, `menu.comp`'s `rgba8` against a
  B8G8R8A8 swapchain. Below, 11-06.
- The panel open over a game that reads keys by `GetAsyncKeyState`, DirectInput or a gamepad: the
  game still gets them. Archive, 10-31 later.
- The `game-tuning` worktree, `D:\NRonWindows\dlss-nr-tuning`, can go once the owner says so: its
  profile is in 0.0.5. Below, 11-06, morning.
- Housekeeping: each release root in `D:\NRonWindows\rel` holds ~440 MB of extracted weights, and
  `D:` ran out of space mid-test. Archive, 10-12.

**Planned, not started** (parked by the owner: do not start unasked)
- A FAQ in the README. Later.
- INT8 on the bottleneck: the exact kernel exists (`gemm_staged_int8.comp`) and stays out of the
  graph by the owner's decision; `improve-int8` keeps the branch. Archive, 09-26.
- The asynchronous live mode exists (`NR_LAYER_ASYNC=1`) and stays off: a frame of latency, over
  the owner's ~15 ms. Archive, 09-25.
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

**Left for a later cut, not done:** "Where things stand" carries each branch's history, 9 KB; the
reference sections 0-7 are 18 KB, the traps half of that.

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

## The daemon keeps the panel's unsaved edits; the Reddit media; a Ko-fi link (2026-11-06, later, branch `game-tuning`)

The owner asked for a short comparison video and screenshots for the Reddit posts, then, on the bug
the shoot found: "Да, можешь пофиксить. А так же чуть редактируй страницу релиза и readme добавив
ссылку на поддержку проекта https://ko-fi.com/uzbekunknown".

**The media.** 0.0.5's own ZIP in Hogwarts Legacy through `nr-game\hogwarts-update-test.ps1`, with the
owner in play. NR was switched with no keys, through the release's trigger file and the profile in
`nr_settings.json`. Stills were taken while the game was in front; the clip was recorded by ffmpeg's
ddagrab and Quick Sync. The game closed through its window after, NR removed, the folders as before.
- `work\pr-replies\reddit-0.0.5-media`: a 38 s video (a live walk with NR off, `hogwarts` and
  `standard`, two slider comparisons on one frame, a link card) and five screenshots;
- `D:\NRonWindows\nr-game\video-1106`: the scripts and raw frames;
- ffmpeg 9.0.2 in `%USERPROFILE%\tools`, downloaded at the owner's word, its SHA-256 gyan.dev's.

It was all shot at render scale 0.4, not the owner's 0.6, because of the bug below; the owner kept
0.4, which matches the posts' 25-26 NR frames a second.

**The bug.** The owner had moved render scale to 0.6 in the panel without saving. At the shoot's
first write of the saved file, the daemon logged `settings (saved): render_scale=0.4` and ran its
network at 512x320 again, while the panel and its `.live` file still said 0.6. On a frame where
only the saved file changed, `Settings.refresh` read the preview as None, "unchanged", and applied
the saved values with nothing on top. The panel keeps unsaved edits for the session, and the saved
file can change under them: the panel's own language key writes it (`save_language_only`), as do
the setup window and `nr-ctl`, so a player meets it with no tool of ours. The other way round, a
preview removed on its own (the panel's process ends, `nr_menu_bridge_destroy`) changed nothing,
since a missing file read as unchanged too.

**The fix** (`669135b`, `src/layer/nr_daemon.py`):
- the last preview is kept (`preview_values`), as the saved values are, and goes on top of every
  refresh;
- `_read_json` returns `Settings._GONE` for a preview that was there at its last look and is not
  now, which drops it;
- the owner check runs on the kept preview at every refresh, and a refused one is forgotten, so a
  later process with the same PID cannot revive it;
- the log names the file that changed.

The panel replaces both files atomically (`MoveFileExA` with `MOVEFILE_REPLACE_EXISTING`,
`rename`), so neither is ever briefly missing. `test_settings_live.py` gained three cases: a saved
write under a live preview, a preview removed on its own, and a kept preview whose menu has ended.
The first two fail on the old code.

**The support link.** The README's new section "Support the project" (`037970c`), and one line on
the v0.0.5 release page before "Built from", edited on GitHub at the owner's word. The ZIP, the tag
and "Latest" are as they were, and `work\pr-replies\release-notes-0.0.5.md` carries the line too.
The release's own README (`docs/RELEASE-QUICKSTART.md`) does not, unless the owner wants it.

Also on this branch: `3fa69b8`, phase73's finding that NVIDIA's Unreal plug-in clamps the strengths
to 0..1 every frame.

**Checked on Windows:**
- `test_settings_live.py` 17 of 17, on MSYS2's Python 3.14 and on the release's own;
- `test_daemon.py`, and `test_daemon_owner.py` (14, 3 skipped);
- `test_gpu_d3d12.py` plain and with `--network`: a real daemon refreshing every frame, the client
  from `dlss-nr-fps\work`, built at `7d70ff1`;
- `test_image_pipeline.py`;
- `test_windows_wizard.py` 100 (1 skipped), on the release's Python;
- `test_release.py` 22 (5 skipped), and `test_toggle.py`'s page check;
- `claims_check`, `publish_check`, `build_check`, `knob_doc --check`.

Under MSYS2's Python, `test_windows_wizard.py` fails one fixture on a path separator
(`dlss-nr\foreign.txt` against `dlss-nr/foreign.txt`), at `8a2a9cd` as well. Not run: Linux; a game.

**Pushed** at the owner's word ("Yes, push", 11-06): `game-tuning`, with `3fa69b8`, `037970c`,
`669135b` and this entry, fast-forwarded into `alchemist`, and `alchemist`, `master` and `newnr` to
its head on GitHub and here.

**Next on Linux:** `git fetch`, fast-forward the three branches to GitHub's, then `make test` in both
memory modes and CTest (Open items). **Next on Windows:** the Reddit posts, the owner's.

## 0.0.5 released: Hogwarts Legacy's own tuning, the frame counter, OptiScaler (2026-11-06, morning, branches `alchemist`, `master`, `newnr`)

The owner, 11-06: "Соберем релиз быстро без участия линукса. Проверки сделаем там позже. Нам после
релиза надо сделать пост на реддит с такими сравнениями и сколько фпс можно получить и заявить о
поддержке optiscaler", and after reading the notes: "можешь пускать в релиз", with the line on
where Hogwarts Legacy keeps the DLL cut from them.

- `bb9317b`: `VERSION` 0.0.5. The README names Hogwarts Legacy as a source of the DLL and gives
  its rows of 11-06 and, through OptiScaler, of 11-05. What to expect gains frame generation, and
  the install steps the counter and the profile. The release guide says the same.
- **In the game, before the build:** with the owner in play, the panel's counter read **Screen
  51.4-52.1 fps, NR 24.7-26.0, network 17.0-17.5 ms**. That is 1280x720, render scale 0.4,
  network B and XeSS frame generation x2: the generator doubles what the screen shows.
- **Built** by `tools\deploy.bat --release` into `%USERPROFILE%\nr-rel\final-0.0.5` (on C:,
  D: had 5.6 GB free) and zipped by `nr-game\zip-release.py`: **`dlss-nr-windows-0.0.5-x64.zip`,
  46 775 008 bytes, SHA-256 `4b28c8cabc39d3985e9395dd43d8e858a6d97110aec3195d0f8a195ddb8a24f2`**,
  from `%USERPROFILE%\nr-rel\final-0.0.5-bb9317b`. The release commit was rebuilt as `bb9317b` to take
  a home path out of two HANDOFF commits before the push; that build matches the checked one,
  made at the first hash, in every file but `release-metadata.json`.
  `release-metadata.json` names 0.0.5 and `bb9317b`.
- **Checked from the unpacked ZIP** (`final-0.0.5\check`): the window's self-test, 89 controls and
  198 keys in four languages. The stand-in run (`standin-release-check.ps1`):
  - discover chose `python\python.exe`; Check; Install with both networks in 8 s;
  - 572 frames, none rejected;
  - the speed test, A with 71 blocks and B with 31, in 44 s;
  - Save report, 19 checks, none failing;
  - Remove NR, and the folder as before.
- **Published:** the lightweight tag `v0.0.5` at `bb9317b`, as `v0.0.4` is. `alchemist`, `master`
  and `newnr` were fast-forwarded from `e1e37b3` on GitHub and here. The release "DLSS-NR 0.0.5 for
  Windows" carries the ZIP. Its body is `work\pr-replies\release-notes-0.0.5.md` without the draft
  comment.
- Not run: Linux (the owner: later).

**Next on Linux:** `git fetch`, fast-forward the three branches to GitHub's, then `make test` in
both memory modes and CTest (Open items: `3d8292a` and the frame counter's commits). **Next on
Windows:** the Reddit post, the owner's to post (`work\pr-replies\reddit-0.0.5.md`). Removing the
`game-tuning` worktree and the release folders on C:, at the owner's word.

## Hogwarts Legacy's DLSS 5 update, and the game's own NR tuning as a profile (2026-11-06, early morning, branch `game-tuning`)

The update reached the owner's Epic copy around 03:00. The owner: look at what the other session
produced (above Open items: pushed, `e1e37b3`), and then: "нам надо сделать так что бы эти новые
настройки применялись ... для Hogwarts Legacy что бы у нас был выбор сделать настройки как
задумывал Nvidia с разработчиками и стандартная настройка", for a video and a Reddit post.

**The update** (`nr-game\dlss5-update-check.ps1`, `release-tests\hogwarts-1106-after\check.txt`):
`nvngx_dlssnr.dll` 310.9.16.0 in `Engine\Plugins\Runtime\Nvidia\StreamlineCore\Binaries\ThirdParty\Win64`,
**byte for byte ours** (SHA-256 `2bc19bc0…789f`): no new network, and Hogwarts owners now have the
DLL. Also Streamline 2.14.16 with `sl.dlss_nr.dll`, DLSS SR, RR and FG 310.9.16, XeSS 2.0.2.53 to
2.0.2.68, XeFG 1.3.1.78, build 2153883; FSR 3.1 unchanged. Setup takes all of it as tested.
- **0.0.4 runs in it** (`nr-game\hogwarts-update-test.ps1`, the DLL taken from the game, as a
  player would now): XeSS starts at the title screen, ~27 frames a second, each answered, none
  declined, the panel on Insert. The counter's build (`rel\check-476d5f0`, `e1e37b3`'s code) in
  DirectX 12: "Screen 26.9 fps NR 25.9 fps network 17.9 ms" in the panel and the top-right
  corner, on the title screen, where XeFG adds no frames; play not looked at.
- The first start came up with FSR 3 Quality. That was the 11-05 FSR 3 test's options: Epic
  uploaded them when that test closed the game, and brought them back over the copy put back
  after it. Not the update (the owner).
- **The harness leaves the game's files alone** (the owner, 11-06: "Дай игре сохранить данные не
  удаляй все без разбора"). Killing the game and putting its settings, `pc_info.json` and save
  list back made it prepare its shaders at every start, and made Epic report a cloud sync
  conflict. It now closes the game through its window (25-65 s), removes only NR's files, and
  reports what the game wrote. `hogwarts-xess-test.ps1` must not run again: its clean copies
  back XeSS 2.0.2.53 from 10-16.

**What the game sets, read out of its executable** (`notes/phase73-game-tuning.md`): Tone 50,
Detail 84 and Character 27 percent for every upscaler, times 0.01, into NVIDIA's Unreal
plug-in's cvars (each 1.0 by default), and Streamline hands them to NGX unchanged. That gives
tone 0.5, structure 0.84 and skin 0.27, at intensity 1 and style 0. The automatic mask's mode
is in the game's packed config, not its code, and the skin strength needs it, so the profile
turns it on.

**The `hogwarts` profile** (`3d8292a`): `nr_frame.PROFILES["hogwarts"]`, with
`AutomaticMask(0.27, -1)`; `feature_lanes` gives channels 10-14 to the native pass and the GPU,
as MLX-DLSS's builder writes them. The GPU image path's push block grew two floats for channels
13 and 14 (`image_preprocess.comp`, 104 bytes). The panel (its profile row is 0..3), the setup
window ("Hogwarts Legacy" in its four languages), the Linux config and `nr_knobs` list it.
- Offline, DOA5's 1280x720 frame on the GPU, network A, change against the game's frame:
  `standard` 0.0369, `hogwarts` 0.0233, `hogwarts` without the mask 0.0293. The mask's share is on
  skin, 7.5x concentrated: faces, chests, arms, the portraits.
- In Hogwarts from a release built at `3d8292a` (`%USERPROFILE%\nr-rel\tuning-3d8292a`, on
  C: since D: has 5.6 GB free), switched live through `nr_settings.json`: on the title screen,
  change 0.029 with `standard`, 0.022 with `hogwarts`, 0.027 with `standard` again; 2750 frames,
  none declined, no error. With the owner in play: the player character's face is softer under
  `hogwarts`, its skin less worked and its shadows lighter, and `standard` brings the texture
  back.

**Checked on Windows:** the MSVC build (`build_win.bat`, `build_xess.bat`);
- `test_nr_model.py` (the profile lanes, against MLX-DLSS's builder for every profile),
  `test_native_image.py` (the mask, native and NumPy), and `test_image_preprocess.py`: the GPU's
  features with the mask, bit for bit against the CPU;
- the seven menu fixtures (`test_menu_choices`: 4 profile options, all labelled), `test_nr_xess`,
  `test_xess_panel`;
- `test_windows_wizard.py` 100, `test_release.py` 22, the bridge, controls, setup and launch
  suites;
- `test_image_pipeline`, `test_image_compose`, `test_settings_live`, `test_daemon`,
  `test_gpu_transport` 49, `test_gpu_d3d12.py` plain and with `--network`;
- `knob_doc --check`, `claims_check`, `publish_check`, `build_check`.

Not run: Linux.

**Next on Linux:** `make test` in both memory modes and CTest at `game-tuning`'s head once it is on
GitHub: `image_preprocess.comp`'s push block, `nr_frame`, the panel's profile row, `nr_config`,
the menu strings. On Mesa the GPU image path's parity tests skip, as before. **Next on Windows:**
the owner's word on merging `game-tuning` into `alchemist` and pushing. The README's DLL line could
name Hogwarts Legacy, since 11-05.

## The Vulkan panel presents on a semaphore (2026-11-06, past midnight, branch `fps-counter`)

The owner, asked whether to take the GPU wait out of the corner first or try MK11 first: do what is
right, then test. `476d5f0` on `fps-counter`, local; nothing pushed.

**What changed.** The panel's submit took the game's present semaphores, so the present had nothing
left to wait on and the layer waited on a fence: for the panel's blend, and so for the whole of the
game's frame, since a fence covers everything before it on the queue. Now the submit signals a
semaphore of the layer's own and the present waits on that, as MangoHud's overlay does (Mesa's overlay
layer before it). For a frame's panel to still be running while the next is recorded:
- a ring of 8 command buffers with their fences and the overlay's descriptor sets (`NR_MENU_SLOTS`),
  each reused after its own fence; ImGui's ring of vertex buffers is as long;
- the semaphore is per swapchain image, because a present's semaphore is free once that image is
  acquired again (validation reports any other reuse); destroyed with the swapchain, through a
  fourth hook, `nr_menu_layer_swapchain_gone`;
- `menu_drain` waits for the whole ring before something shared is destroyed or rewritten: the UI
  image on a resize, the font atlas when ImGui uploads a glyph on the UI's own queue
  (`nrex_vk_drain_hook`), the views of a swapchain going, the pool when the queue family changes;
- the UI render pass's two external dependencies are explicit: the implicit one into the pass starts
  at no stage, so its layout transition was ordered after nothing, the previous frame's blend
  included;
- the view cache keeps a view for each image. A miss dropped every view, so with three images each
  frame destroyed the view the previous frame had used.

Also: the UI image starts UNDEFINED, which its load op clears anyway, and the dump's barriers go from
and back to GENERAL - these were the four `VUID-vkCmdDraw-None-09600` a run; image and view handles
reach the blend without a 32-bit `uintptr_t` cast on x86; `test_gpu_present.py` takes `--panel
shut|open|corner`.

**Measured** (quiet, Intel 101.9150, on AC, best performance). `test_gpu_present`, NR off, 800x600
then 1024x640, 120 frames each at a 10 ms pause; a present's median over three rounds, against
`aec70da` built the same way: the corner 2.67-2.84 ms before, 1.88-1.98 after; the panel open
3.57-3.59, 2.52-2.56; no panel 0.69-0.71 and 0.64-0.67. What the corner still costs is the layer's own
work on the CPU, the driver's present taking 28 us. Taken apart once: 0.41 ms is the panel looking
for its window with `EnumWindows` on every present, since the fixture's window is hidden (a game in
front is found by `GetForegroundWindow` first), 0.08 ms the trigger file's `access()`, and the
panel's frame, recording and submit 0.38 ms. The first two are older than the counter and are paid
whenever the panel is built, open or not; not changed.

**Checked on Windows.** Synchronization validation, no message but the known `rgba8` warning:
`test_gpu_present` with NR off, the panel open, the corner and none, Chinese with the panel open (its
glyphs uploaded during the run), Russian with frames back to back; `test_gpu_present.py --run` with NR
on the GPU path, the panel shut, open, in the corner, and in the corner with the trigger turned off
and on (`--exercise-controls`), 78-80 frames answered each. MSVC x64 and x86 with no new warning,
MinGW with none on a changed line; the ten native fixtures, `test_xess_panel` (both phases),
`test_nr_xess`; `test_daemon`, `test_gpu_transport`, `test_gpu_d3d12`, `test_menu_ui_fixtures`,
`test_menu_rows`, the six tool suites incl. `test_windows_wizard.py` (100); `claims_check`,
`publish_check`, `build_check`.

**Mortal Kombat 11** (DXVK) from a release built at `476d5f0` (`D:\NRonWindows\rel\check-476d5f0`,
preset B, render scale 0.4), through `nr-game\mk11-counter-test.ps1`: Install NR, the counter's place
in that install's settings, Steam's start, NR on, 40 s counted, NR off, Remove NR, the game folder
compared. The screen was locked throughout (LogonUI, no window in front), and Windows let the game
present about 8 frames a second: 7.92 frames answered a second with the corner, 7.90 with the counter
in the panel, 7.97 with 0.0.4 in the same state, against 34.29 for 0.0.4 unlocked on 11-05 17:40.
Four runs, every frame `gpu-shared`, none refused, the game closed through its window, no daemon
left, the folder as before. The UI image the layer wrote in the game (`NR_MENU_DUMP`, through
nr-env.txt) has the corner at the top right of 1280x720: "Screen 5.6 fps   NR off".

**With the screen on** (02:46, the owner back; not quiet, the Epic Games Launcher at 11 % CPU with
Hogwarts' update), the same script and release, the game in a window: the corner read "Screen 60.0
fps   NR off" with NR off, MK11's own limit, and "Screen 33.5 fps   NR 33.5 fps   network 18.3 ms"
with NR on; 30.64 frames answered a second over the 40 s, and 29.69 for 0.0.4 with no counter right
after, in the same state. Both runs clean, the folder as before.

**Pushed** at the owner's word ("Можешь проверить в игре и пушить", 11-06): `fps-counter` fast-forwarded
into `alchemist`, and `alchemist`, `master` and `newnr` to its head on GitHub. **Next on Linux:** as
for `7d70ff1` (Open items).

## A frame counter in the in-game panel (2026-11-05, night, branch `fps-counter`)

The owner asked for a frame counter in the panel the Insert key opens, in both hosts, and then that
the player choose where it shows, in the panel only or in a corner; issue #19's reporter had asked
for an fps display. The change is `7d70ff1`, and `489ee1f` a fault it found, on a local branch
`fps-counter` from `alchemist` at `6ef8041`, in a worktree of its own (`D:\NRonWindows\dlss-nr-fps`);
nothing pushed.

**Two rates, because under frame generation they differ.** NR runs on the frames the game renders;
a generator adds its own after that, so the screen shows more (about twice at XeFG x2: Hogwarts' 27.7
frames a second on 11-05 evening were NR's, the screen's about 55, unmeasured).
- The screen's: every present of the swap chain the panel is drawn on. In the Vulkan layer, the
  presents that include its primary swapchain; in DirectX 12, the Present hook's, of the largest
  swap chain presenting within the last second, which under XeFG is the generator's.
- NR's: the frames the daemon answered and the host put into the picture. The layer's GPU path
  (`gpu == 2`) and its CPU write-backs; the proxy's cut and copy answers, what its "NR: N frames in
  5 s" lines count.
- The network's ms a frame: a FRAME's SUCCESS reply carries it in `detail` now, in microseconds
  (`nr_gpu_wire.h`; it was 0 and no client read it). The CPU path reports none, and none is shown.
- `src/layer/nr_frame_rate.h`: the host keeps totals, the panel samples them once a present; the
  rates are over the last second, read every half second, with a comma for the decimals in Russian
  and Spanish. "NR off" while the trigger is off.

**Where.** A line at the top of the open panel ("Screen 63.3 fps    NR 32.1 fps    network 24.0
ms"), its tooltip saying what the two rates are, and a row "Frame counter": in the panel only, the
default, or one of the four corners, where the counter stays with the panel shut. The corner is an
ImGui window with no input, focus or navigation; with the panel shut ImGui is handed no pointer and
no keys at all. The place is `menu_counter` in `nr_settings.json`: shown at once, written by Save
(once it is not the default), back with Revert, left by Reset, as the language is. Twelve strings in
four languages, `OWN` in `tools/gen_menu_strings.py`, the tables regenerated.

**What the corner costs.** Each host now draws over the part ImGui drew and no more
(`nrex_drawn_rect`): the Vulkan blend's dispatch, and in DirectX 12 the clear and the overlay's
scissor. With the panel shut that is the corner, about 350x24 pixels. In DirectX 12 the panel's work
goes on the presenting queue with no wait on the CPU. In Vulkan the panel's submit is still waited
for before the present, as it always was: with NR on the layer waits for the frame anyway; with NR
off the present now waits for the game's frame on the GPU. `test_gpu_present`, light GPU work, NR
off: a present's median 0.32 ms without the panel, 1.45 with the corner, 2.0 with the panel open. In
a native Vulkan game bound by its GPU, with NR off, that wait takes away the overlap of its CPU and
GPU, so the corner would read the screen's rate lower than it is without it; not measured. DXVK
presents from a thread of its own, which should lose less. The way out is MangoHud's: a semaphore
the panel's submit signals and the present waits on, one per swapchain image; not done.

**Two old faults of the Vulkan panel's submit**, both in `nr_menu_layer.c`, found by the counter:
- Its fence was made unsignalled and reset at the top of each frame, and the UI image's resize waited
  on it: the first frame the panel drew on a swapchain, and the first after each change of size,
  stalled a second. `test_gpu_present` showed 1.02-1.08 s at both sizes, with the 11-05 build as with
  the new one. The fence is made signalled now and reset just before the submit.
- It waited on the game's present semaphores after the GPU path's capture had taken them
  (`present_wait_count` was left as it was), and with NR off the present waited again on the
  semaphores the panel's submit had taken. A binary semaphore is waited once. It now waits only on
  what nobody took and marks them taken, and the present goes without waits, as after `transfer()`,
  its fence having seen the work complete. Under validation (next paragraph) the old rule gives
  `VUID-vkQueuePresentKHR-pWaitSemaphores-03268` (10 reports, the duplicate limit) with the panel
  open and NR off, and the new one none.

**The panel under validation, and a third fault** (`489ee1f`). `test_gpu_present --sync-validation`
with the panel built ended at the panel's first frame with 0xC0000409 in ucrtbase (an abort), with
the 11-05 build too. The cause: the UI's own command buffers, ImGui's for its texture uploads and the
one-shot for the font, were never given the loader's data, so the validation layer found no device by
them. Any layer below ours in a player's game, an overlay's or a capture's, would meet the same.
`vkAllocateCommandBuffers` now reaches ImGui through a thunk that gives each buffer it, and the
one-shot path does the same. With it the panel runs under synchronization validation: the panel open,
the counter in a corner and no panel, NR off, 240 frames each, no synchronization or semaphore error.
Left, older than all this: `VUID-vkCmdDraw-None-09600`, 4 a run, the UI image sampled as GENERAL
after its render pass left it in COLOR_ATTACHMENT_OPTIMAL (UNDEFINED on the first frame); and a
warning that `menu.comp`'s `rgba8` storage image is not a B8G8R8A8 swapchain's format. Not fixed.
vkcube cannot carry the panel (Vulkan 1.0).

**Checked on Windows:** MSVC `build_win.bat` (the three known C4068) and `build_xess.bat` with no new
warning. The native fixtures, all ten (the seven `test_menu_*` and the three of the input path).
`test_menu_panel` gained 15 checks: the rates, every
language's line, the comma, NR off, a pause and totals going back, the corner's bounds, a click on
the corner, the open panel's line. `test_menu_actions` gained 11, on the place. `test_xess_panel` has
a second phase, a process of its own: the panel shut, the counter in the top right corner by the
settings file, every Present counted, the line built, and the rest of the frame the game's.
Also: `test_nr_xess`, `test_gpu_transport.py` (49, two new), `test_gpu_d3d12.py`, `test_daemon.py`,
`test_windows_wizard.py` 100, `test_release.py` 22, the bridge, controls, setup and launch suites,
`claims_check`, `publish_check` and `build_check`. MinGW (gcc 16) CMake of the layer and the D3D12
client: no warning in the changed files. `test_gpu_present` through the layer with NR off, the panel
open and the corner, the panel's image dumped: the line, the row and the corner as meant; and the
same under synchronization validation since `489ee1f` (above). **Not run:** Linux; a game.

**Next on Linux:** `make test` in both memory modes and CTest at `fps-counter`'s head, once it is on
GitHub: `nr_layer.c` and `nr_layer_state.h` (the counts), `nr_gpu_transport.py` (`detail`) and
`test_gpu_transport.py`'s two cases. **Next on Windows:** the owner's word on a game check (Mortal
Kombat 11 with `pacing-game-test.ps1`, the corner on and off, NR on and off; Hogwarts once its
update is in and the game is free), on merging `fps-counter` into `alchemist` and pushing, and on the
semaphore present.

## NR through OptiScaler runs in Hogwarts, and the DLSS 5 update prepared for (2026-11-05, evening, branch `alchemist`)

The owner: work on OptiScaler now, take its new version from GitHub, and get ready for Hogwarts
Legacy's DLSS 5 update, due that night.

**OptiScaler 0.9.4** (its latest release, 07-18; the 7z's SHA-256 is GitHub's digest,
`575cb4df…0ad`), unpacked in `D:\NRonWindows\nr-game\optiscaler-0.9.4\package`: OptiScaler.dll
0.9.4.0, its XeSS 2.0.2.68 (the proxy's 72 exports, as the entry below found), and FSR, XeFG and
fakenvapi files. `nr-game\hogwarts-optiscaler-test.ps1 -Phase install|start|status|clean` puts in
the three files the test needs (OptiScaler.dll as `dxgi.dll`, its `libxess.dll`, `OptiScaler.ini`
with Dx12Upscaler=xess, Dxgi=false so no spoofing, ShortcutKey=0x23 since its menu's Insert opens
NR's panel too, and a log file), then NR from 0.0.4's ZIP through the setup window's backend with the
API `d3d12`, as a player would; it notes the executable's folder and the saves first, and a hidden
watchdog cleans after 40 minutes. 0.0.4's Check took OptiScaler's `libxess.dll` beside the
executable before the plug-in's, as `xess_libraries` orders them.
- **From the game's XeSS** (UltraQualityPlus, the game's XeFG on), 19:47-20:05: OptiScaler took the
  game's XeSS calls, loaded NR's `libxess.dll` from its folder ("loaded from Opti dll path"), and the
  proxy started the daemon itself; every frame was answered on its own frame, 27.7 a second at 1280x720
  and render scale 0.4 (network 512x320, B), 52 ms from the copy to the answer, none declined.
- **From the game's FSR 3** (Quality, chosen in the menu by the owner at 20:05:40): OptiScaler took
  the game's FidelityFX API context (`ffxCreateContext_Dx12`) and made a new XeSS for it; the proxy saw
  the new init (quality 103, flags 0x10a, velocity scale 854x481, output R16G16B16A16_FLOAT) and its
  session took the new output at once. 27.7 frames a second again, 64 ms from copy to answer, none
  declined, to 20:29: 12,148 frames in the run. How the picture looked the owner has not said.
- Editing `UpscaleMode` in `GameUserSettings.ini` does not choose FSR: the game keeps its options in
  `SaveGames\<id>\SavedUserOptions.sav` and writes the ini from it at start.
- The watchdog cleaned at 20:29: game and daemon stopped, Remove NR, OptiScaler's three files out,
  the executable's folder as before. The owner's options, put back from the copy with the save list
  and two crash-reporter session configs, are as before the test (XeSS UltraQualityPlus, XeFG x2);
  against the baseline below the whole game is as it was but Epic's `.ovt` token, rewritten at launch.

**Setup follows OptiScaler** (`d936d63`, local): `optiscaler()` reads `OptiScaler.ini` (beside the
executable or in an `OptiScaler` folder) and takes the `libxess.dll` OptiScaler loads, in its own
order: XeSSPath, its own folder (OptiDllPath, an `OptiScaler` folder, the executable's), the name
alone. Check names it; refuses when OptiScaler upscales with anything but XeSS, or has no
`libxess.dll` where it looks; warns when OptiScaler's menu is on Insert. On the way, a bug of 0.0.4:
for a `libxess.dll` beside the executable (OptiScaler's, or a game's own) the record names no folder,
so Check after Install took NR's `libxess.dll` for one setup has no record of, and Remove NR left
`nr-xess.log`; `_xess_placed` now takes the executable's folder. The window's API reads "DirectX 12
(games with XeSS or OptiScaler)" in its four languages, and the README and the release guide say how.
Checked: `test_windows_wizard.py` 100 (two new), `test_release.py` 22, the window's self-test (89
controls, 197 keys), and the new Check against the Hogwarts installation while the game ran.

**The DLSS 5 update.** NVIDIA's announcement and the press give November 5 for Hogwarts Legacy (DLSS 5
on RTX 50 and GeForce NOW, the game's other DLSS updated), no hour; the owner expects it near
midnight UTC+9. The Epic copy was unchanged at 20:31 (files of 10-15). Ready for it:
- a baseline, `D:\NRonWindows\nr-game\release-tests\hogwarts-1105-before`: every file's size and time,
  and the 111 DLLs and executables with version and SHA-256. No `nvngx_dlssnr*.dll` is in the game.
- `nr-game\dlss5-update-check.ps1`: what the update changed against it, every `nvngx_*`, `sl.*`,
  `libxess*` and `amd_fidelityfx*` DLL with version, SHA-256 and signer, and a new
  `nvngx_dlssnr.dll`'s resources against 310.9.16's (`dlss5-dll-compare.py`: WEIGHTS_HT, preset A,
  147,695,410 bytes; WEIGHTS_CC_SMALL_FP8, preset B, 113,893,343). Both only read.
- Setup takes a DLL of another version (Check says which were tested), and extraction needs the
  `WEIGHTS_*` resources. New or changed ones would be a network to recover, as B was (phase72); no
  DLL in the game would mean the model comes through NVIDIA's driver, not to be had from this game.

**Next on Linux:** `make test` and CTest once `d936d63` is on GitHub (`test_windows_wizard.py`
gained two cases, and `windows_wizard_core.py` changed). **Next on Windows:** the update's check;
the owner's word on pushing `d936d63` and on the release that takes it.

## 0.0.4 built and checked; how NR could reach OptiScaler's and FSR 3.1's games (2026-11-05, later, branch `alchemist`)

The owner: prepare the release, unhurried, and can NR take OptiScaler (#38), or at least hook FSR 3
or newer?

**0.0.4, published on 11-05 at the owner's word** ("После этих изменений можешь релизить"), after
the owner had the notes cut to what a player needs: Python included, the faster network (on Arc
A-series cards, the same driver, expected and not measured), what is needed, how to install, the
limits. Tag `v0.0.4` at `8232603`; `master`, `newnr` and `alchemist` fast-forwarded
`0456e99..9b73d96` on GitHub and here. The release "DLSS-NR 0.0.4 for Windows" is the latest; its
asset's digest is the ZIP's SHA-256 below, and its body is `work\pr-replies\release-notes-0.0.4.md`
without the draft comment. `8232603`: `VERSION` 0.0.4 (as proposed; the
owner did not name another), the README's "You need" without its Python line, Mortal Kombat 11's
row at 0.4 and 1.0 as measured with this code (archive, 11-04 night; below, 11-05), an A770 owner's
three games at 1920x1080 (#37) in the tested-games table and in `players.json`, and that card's
1080p figure in what to expect. The next commit gives Mortal Kombat 11's row as a band (below in
this entry) and fixes `docs/DEVS.md`'s line on what a Windows release needs; neither file is in
the ZIP, whose `README.md` is `docs/RELEASE-QUICKSTART.md`.
- Built by `tools\deploy.bat --release D:\NRonWindows\rel\final-0.0.4\dlss-nr-windows-0.0.4`
  (`NR_PYTHON` the native venv, the checkout clean) and zipped by `nr-game\zip-release.py`:
  **`dlss-nr-windows-0.0.4-x64.zip`, 46 732 436 bytes, SHA-256
  `a08b1ccca842989aa6ef8273a92ef8e10b14a169affb8c6d25b392f7227ece5b`**, `release-metadata.json`
  naming 0.0.4, `8232603`, Python 3.14.10, NumPy 2.5.4, safetensors 0.8.0. The notes are
  `work\pr-replies\release-notes-0.0.4.md`. `rel\dlss-nr-windows-0.0.4` and `rel\check-0.0.4` are
  11-01's first build of 0.0.3.1 under this name, stale, and were left as they are.
- **From the unpacked ZIP** (`rel\final-0.0.4\check`), on the stand-in
  (`nr-game\standin-release-check.ps1`, which now sends the speed test a request naming no network,
  as the button does, and saves the report apart from the bridge's result): discover chose
  `python\python.exe`; Check; Install with both networks in 7 s; 623 frames, none rejected, by a
  daemon on `python\python.exe`; the speed test, A (71 blocks) then B (31) in 39 s, B's network at
  1920x1080 163 ms at 1.0 and 30 at 0.4, A's 333 and 58; Save report, 19 checks, none failing;
  Remove NR, the folder as before.
- **Mortal Kombat 11 from the same folder**, as a player gets it: network B, frames on the GPU,
  the daemon on `python\python.exe` (`nr-game\release-mk11-check.ps1`, render scale 0.4 and then
  1.0 through the root's `nr_settings.json`, put back after). In every run each frame took the GPU
  path with nothing refused, no daemon outlived the game, and the folder was as before. The first
  pair ran while `explorer.exe` held a core and Defender read the unpacked files: 32.1 frames a
  second at 0.4 (network 17 ms) and 9.7 at 1.0 (85 ms). Once `quiet-check.ps1` read the machine
  quiet: **34.3 at 0.4 (network 17 ms) and 9.8 at 1.0 (84 ms)**. The same code ran 38.3-38.7 and
  10.4-10.6 on 11-04 night (network 16 and 81-82) and 38.0 from `e72be76`'s ZIP this morning: the
  network is within 1-3 ms of those, the rest of the frame is not. The battery was charging (53 to
  66 %); what took the rest is not established. The README's row gives the band, 34-38 at 0.4
  (the next commit).

**OptiScaler and FSR 3.1: read, nothing built.**
- **OptiScaler** (GPL-3.0; its source read at `5bf6d19` in the session's scratchpad, nothing
  copied). On DirectX 12 its upscaler is XeSS by default. `XeSSProxy::InitXeSS` loads
  `libxess.dll` from `[Libraries] XeSSPath` when set, else from its own folder (an `OptiScaler`
  folder beside the executable when there is one, else the executable's), else by name; it matches
  names exactly, so the proxy's `libxess_nr_orig.dll` is not taken for XeSS. So DLSS or FSR inputs
  → OptiScaler → XeSS → NR's proxy should need no code where setup puts the proxy in that folder:
  `xess_libraries` takes the one beside the executable first, and looks in the folders under it
  where that has none. A game's own XeSS in an Unreal plug-in would be taken before an `OptiScaler`
  folder's, but such a game reaches NR without OptiScaler.
- **XeSS SDK 3.0** (intel/xess, 3.0.2 of 07-24): `xess.h` and `xess_d3d12.h` are 2.0.1's but for
  two comment typos, so the init and execute structs the proxy reads are unchanged. Its
  `libxess.dll` is 77 795 704 bytes, as is the 2.0.2.68 that OptiScaler 0.9.0.12 carries, and that
  one exports exactly the proxy's 72 names (`pe_exports`, on the copy below). The "XeSS 3.0" of
  #38 is XeSS-SR 2.0.2.68 beside a newer frame generation, and Check takes it as tested.
- **Untested:** the cut beside OptiScaler's own D3D12 hooks (its frame generation hooks the queue
  and the swapchain); OptiScaler's DirectX 11 path (XeSS through 11on12 or `libxess_dx11.dll`,
  which the proxy does not stand in front of); and #38's Stellar Blade, where it is not known
  whether Check refused, the wrong executable or API was chosen, or the proxy got no frame. The
  reply, posted at the owner's word (issuecomment-6654620978), gives the steps and asks for
  Check's words, Save report and `nr-xess.log`.
- **On this PC:** `Mortal Kombat 1\MK12\Binaries\Win64` holds only mods (OptiScaler 0.9.0.12 with its
  XeSS and FSR files, and a `dxgi.dll` from a d3d12-proxy project that reports an iGPU's memory
  size to the game); the game itself is gone (no `MK12.exe`, no Steam manifest). Hogwarts Legacy
  ships `Phoenix\Binaries\Win64\amd_fidelityfx_dx12.dll` 1.0.1.41314, FidelityFX SDK 1.1's FSR 3.1,
  beside its XeSS 2.0.2.53, so it can carry both tests: OptiScaler with the game's DLSS or FSR in
  and XeSS out, and an FSR 3.1 proxy.
- **FSR 3.1:** games call AMD's FidelityFX API DLL, five exports (`ffxCreateContext`,
  `ffxDestroyContext`, `ffxConfigure`, `ffxQuery`, `ffxDispatch`); FSR 2 and 3.0 are mostly linked
  into the game, with nothing to stand in front of. A proxy as for XeSS would forward all five,
  frame generation and its swapchain included, and on an upscale dispatch
  (`FFX_API_DISPATCH_DESC_TYPE_UPSCALE`) of a context it saw created run NR on FSR's output, as
  `mode=after` does, with the same cut, passes, session and panel. FidelityFX SDK 2.x games call it
  `amd_fidelityfx_loader_dx12.dll`. New would be the FidelityFX structs (AMD's MIT headers), the
  resource states they pass, a setup API and tests: some days, then Hogwarts with FSR 3.1 chosen.
  It reaches games with FSR 3.1 and no XeSS without OptiScaler.

**Next on Linux:** `git fetch`, fast-forward `master`, `newnr` and `alchemist` to GitHub's, then
`make test` in both memory modes and CTest at its head (Open items). **Next on Windows:** #38's
reporter's answer; the owner's word on OptiScaler in Hogwarts (the 0.9.0.12 copy on this disk, or
0.9.4), and on the FSR 3.1 proxy.

## `cleanup`: a shorter brief, `make test` in parallel, and the leftovers removed (2026-11-05, branch `cleanup`, PR #39)

At the owner's word, on a branch of its own from `newnr` at `0456e99`, pushed and opened as a draft
into `newnr`. Nothing in it has run on a GPU.

- **The brief.** `notes/CLAUDE.md` is 8.9 KB from 31.2 KB, about 6k tokens less at every session
  start. The full text is `notes/archive/CLAUDE-until-2026-11-04.md`, word for word, and what older
  notes cite as "notes/CLAUDE.md" with a phase number is there.
- **`make test`** runs `src/tools/run_tests.py`: the old list in its order, each check with a kind.
  The 27 that open no device run in a pool beside the GPU queue, which still holds one process on
  the device; the heavy ones (the whole network, a daemon) have it alone; a timing gate and a
  process-list check run last. A failure no longer stops the run; every check's output is kept in
  `work/test-logs` and its time in `work/test-times.json`. `XMX_STAGING=1 make test-gpu` runs only
  what the memory mode reaches, and `TEST_ARGS=--serial` the old order one at a time. CTest lost
  its five `host_image_*` duplicates, and `make test` took CTest's two Linux-only checks.
- **Gone:** four tests no runner ran; the `hnet_*` harness on the misread decode; `NR_JOINT_QKV`
  with its kind in `attention.comp` and `xmx_rec_qkv` (off since 09-21, unreached since the QKV
  epilogue); the `NR_FUSE_BRANCHED_FFN` switch (measured and not kept);
  `src/probe/coopmat_store_bug.comp`, whose header claimed the bug phase38 withdrew; the
  unbuilt `gemm_smallm.comp`.
- **Back in a runner:** `tools/check_ui_resize.py`, broken since the panel moved on 10-31, as
  `src/layer/test_ui_resize.py`; and the Windows setup's six suites, 187 tests that ran only by
  hand. `make test` is 70 checks on Linux, CTest 69 entries.

**Checked** in a cloud container with no Intel GPU and no weights: the list against the old recipe;
the runner's scheduling on stand-in checks; the device-free checks, all but `test_dump_raw` (it
needs ImageMagick 7); the shaders and libxmx compile; the six setup suites (21 of 187 skipped as
Windows-only); CMake and `ctest -N`; `build_check`, `claims_check`, `publish_check --history`,
`knob_doc --check`.

**Next on Linux:** `git fetch`, `git switch cleanup`, stop the daemon, `make test` in both memory
modes and CTest. `work/test-times.json` then holds each check's time, which no run has measured.
**Next on Windows:** `build_win.bat` and CTest. **Waiting for the owner:** the merge, after both;
whether the working rules take `make test-gpu` for the second memory mode, and CTest on Linux only
when CMake changes. Proposed and not done: INT8 out of the main line (it is in libxmx, the shader
lists and the release's file lists), Configure/Restore Steam (tied to the live launch path), one
build system instead of three.

## Windows releases carry their own Python (2026-11-05, branch `alchemist`)

The owner asked for an easier install, with no Python for a player to set up. Python's own share
of a frame is half a millisecond (archive, 11-04 night, latest), so a daemon in C would buy nothing
worth its weeks; the release carries the interpreter instead. The change is `e72be76`.

- **What a release carries:** python.org's embeddable CPython 3.14.10 for x64 in `python\`, with
  NumPy 2.5.4 and safetensors 0.8.0 unpacked from their PyPI wheels into
  `python\Lib\site-packages`. `dist-tools/python.json` pins the three files by URL and the SHA-256
  python.org's release page and PyPI publish. `scripts/fetch_python.py` takes them into
  `work\python` once, refusing any other file, a wheel member pip would place elsewhere and one
  that would land outside, and adds the `._pth` line for the wheels' folder; `tools\deploy.bat
  --release` runs it beside `fetch_dxvk.py`, and `release-metadata.json` names the versions.
  NOTICE names the three and where their licences are in a release.
- **The embeddable package is isolated by its `._pth`, as `-I` would make it:** no PYTHONPATH, no
  user site-packages, and not the script's own folder on sys.path (`sys.flags.safe_path`,
  checked here). `windows_wizard.py` and `nr_daemon.py` now put their own folder there; every
  other script a release runs already set its paths, or is loaded by its path (MLX-DLSS's tools
  import only the standard library, NumPy and safetensors). The package has no `venv`,
  `ensurepip` or `tkinter`.
- **What chooses it:** the window's `Find-BootPython`, the backend's `discover_python` and
  `setup.bat`, each after `NR_PYTHON`, a developer's choice. `discover_python` probes it with the
  time a first NumPy import takes under Windows' scanner (`_python_info`, 30 s), not the 2 s each
  other candidate gets. Prepare Python leaves a Python with both packages as it is.
- **Size:** the ZIP is 46.7 MB, against 0.0.3.1's 21.6; `python\` is 64 MB of the 116 unpacked.

**Checked on Windows:** `test_release.py` 22 (a new fetch case), `test_windows_wizard.py` 98 (three
new), the bridge 7, `test_windows_launch.py` 43, and the window's self-test (89 controls, 197
keys). The shipped interpreter itself, run without its `._pth`: the 50 CMake GPU, image, ref and
tool tests, and the Windows layer tests; four Linux-only layer tests fail there as on 3.12.
- **From the unpacked ZIP of `e72be76`**, on a stand-in (the SDK's vkcube in a scratch folder,
  `D:\NRonWindows\nr-game\standin-release-check.ps1`), every call on `python\python.exe`: discover
  chose it; Check (3.14.10, NumPy 2.5.4, safetensors 0.8.0); Install with both networks out of the
  310.9.16 DLL, in 6 s; 649 frames, all answered, by a daemon on `python\python.exe`; the speed
  test of A and B in 34 s (B at 1280x720: 16 and 80 ms at 0.4 and 1.0, A 29 and 152); Save report
  with 19 checks and none failing; Remove NR, the folder as before.
- **In Mortal Kombat 11** from the same folder, B at 0.4: 1520 frames in 40 s, 38.0 a second, network
  16 ms, every one through the GPU path; one daemon process (a venv's was two, its launcher and
  the interpreter); the game's folder and its ten writable files as before, no launch options in
  Steam. `quiet-check.ps1` read the machine busy just before (14 % CPU, the scanner reading the
  unpacked files, likely), so the rate is a check, not a measurement.

**Next on Linux:** `make test` in both memory modes and CTest at `e72be76` (Open items). **Next on
Windows:** the next release, at the owner's word (Open items).

---

The reference sections. 1 ("Do this first") and 6 ("Honest standing") are in the archive; the
rest keep their numbers, because other files cite them.

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

---

## 7. Hardware (probed, trust it)

Intel Arc 140V-class Xe2, Mesa ANV, Vulkan 1.4.354, subgroup 32. Six cooperative-matrix
configs, all scope=subgroup, all M=8 N=16; the path is **fp16 × fp16 → fp32** (config 1).
`cooperativeMatrixRobustBufferAccess = false`, so edge tiles need explicit padding.
Shared-memory APU: correctness and capacity are unaffected (141 MiB of weights against
an 11.46 GiB heap); it caps **bandwidth**, already the measured ceiling at 0.7–1.9
TFLOP/s with a naive kernel. `libxmx.c` still memcpys into a mapped HOST_VISIBLE buffer
on every dispatch — on an integrated GPU those copies are free to remove.

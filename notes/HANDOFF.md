# HANDOFF — read this first

State of the DLSS-NR on Intel Xe2 project as of **2026-11-03**. notes/CLAUDE.md holds the
original brief; **this file overrides it wherever they disagree**, and after
2026-09-09 they disagree about something foundational.

**How to read this file.** "Where things stand" and "Open items" are the current state. The dated
entries after them are the latest, newest first. Everything older is in `notes/archive/`, word
for word: the graph's recovery, the kernel work, the measurements and the experiments that did not
pay in `HANDOFF-until-2026-10-07.md`, the first Windows releases and the testers' reports in
`HANDOFF-2026-10-08-to-2026-10-15.md`, and DirectX 12 through XeSS, preset B and PR #20's review
in `HANDOFF-2026-10-16-to-2026-10-28.md`. It is history: do not read it to start a task. Open it
only when a line here points there, or before an optimisation, to see whether it was tried and
not kept. `notes/INDEX.md` says which note settles what.

Keep this file short. Every session loads it whole and sends it again with every request it
makes, so its size is paid for on every turn, and `publish_check` refuses a file over 200 KB. When
the dated entries grow long, move the older ones the same way, into a new archive file: on 11-01
the entries before 10-30 went, and the file fell from 156 KB to 69 KB.

---

## Where things stand (2026-11-03)

- **It works on both systems of this laptop** (Arc 140V): Linux with Mesa, Windows with Intel's
  driver. A Vulkan layer hands each presented frame to a daemon that runs NVIDIA's network on the
  GPU and writes the answer back into the swapchain. DirectX 8-11 games go through DXVK. DirectX 12
  runs on Linux through VKD3D-Proton, and on Windows through the game's own XeSS (since 0.0.2).
- **The releases are Windows-only so far**: v0.0.1 (`7f9c364`), v0.0.1.1 (`f9ec5f1`), v0.0.1.2
  (`2f90064`, 10-15), v0.0.1.3 (`d89563f`, 10-18), v0.0.2 (`81b0a59`, 10-31), v0.0.3
  (`86a5799`, 11-01) and **v0.0.3.1** (`c0b7f9d`, 11-01, the latest), a ZIP with the setup window, DXVK 3.1.1, both layers, a
  `vulkan-1.dll` proxy and NR's `libxess.dll` as `nr_xess.dll`. 0.0.2 brings DirectX 12 through
  XeSS, the in-game panel (PR #20), network B and frames on the GPU by default, and an About tab
  that checks GitHub for a newer release (below, 10-31). 0.0.3 brings the panel inside DirectX 12
  games, its keys and clicks kept from the game, and `nr_gpu_probe.exe`, the GPU report for Arc
  A-series owners (below, 11-01). 0.0.3.1 is what the first A-series reports asked for: the speed
  test measures both networks and names each, and setup finds a game's XeSS in a subfolder (below,
  11-01 later). All three went out on the owner's word before Linux's `make test` at their heads,
  as 0.0.1.3 did: 0.0.2's ran green after, at `eb9b281`, and 0.0.3's and 0.0.3.1's changes wait
  for it (Open items). Linux users build from source (README).
- **Branches**, on GitHub since 10-30:
  - `master`, the default: 0.0.1.2 (`2f90064`) until 10-30, then fast-forwarded to `newnr`, and
    with it to 0.0.2 (`81b0a59`, 10-31), 0.0.3 (`86a5799`, 11-01) and 0.0.3.1 (`c0b7f9d`, 11-01);
  - `newnr`: everything since, with PR #20 merged at `097d6d4` and `dx12` on top of it
    (`cc2b8b7`, 10-30);
  - `media`: the README's pictures;
  - `improve-int8`, kept for INT8 experiments;
  - `alchemist`: the Arc A-series work, from `newnr` at `0a6000f`, on GitHub since 11-01 (below,
    10-31 evening), level with `newnr` at 0.0.3.1.

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
  changes it, and in Hogwarts NR stayed on through A to B and back (below, 10-30). Since that
  evening the setup window installs it, **DirectX 12 (games with XeSS)**, and the proxy starts the
  daemon itself (`6c503ae`, below). Since 10-31 the in-game panel draws there too, and in both
  hosts the game no longer gets the panel's keys and clicks; in no release yet (below, 10-31 later).
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
  Windows, where Intel's compiler loses time in the staged GEMM's loop. The README's table
  (`nr_knobs.RATES`) was measured with `live_rates.py --warmup 3`, again on 10-18 with the clock
  hint, and the default warm-up of 25 now reads the same. On Linux libxmx asks the driver for its
  low-latency clock strategy since 10-18 (`XMX_LOW_LATENCY`): without it the GPU's governor
  clocked a frame with gaps around it down to 700-1200 MHz of 1950 (archive, 10-18).
- **On the players' GPUs** (below, 11-03; `notes/players-20261103.md`): a B580 runs network A at
  1280x720 and 0.4 in 9 ms, 3.9x an A770. An A770 plays level with this laptop at that size,
  because its network pays ~55 us a pass whatever the field. Eleven A-series cards compute NR's
  shape exactly, and a Pro B70 offers it natively.
- **Working rules.** Nothing goes to GitHub without the owner's word, and nothing untested at all:
  `make test` in both memory modes (`XMX_STAGING=1` is the second), CTest, and on Windows the MSVC
  build. Commits use the noreply address. NVIDIA's DLL, and anything derived from it, never enters
  the repository or a release. A Linux agent and a Windows agent work on this one laptop; the owner
  relays between them, and this file is how they hand work over. A release updates the README's
  GPU and tested-games tables in the same commit (the owner, 11-02).

## Open items (collected 2026-10-14, from this file and its archive)

Each line says where its evidence is: "below" is a dated entry in this file, "archive" the archive
under that date. When an item is done, strike it through here and name the commit.

**Waiting for the owner**
- Answers to the reports of 11-01 to 11-03 (#29-#34), and two questions the data raised: #25,
  which network each API ran; #23, whether NR was on in Hogwarts. Below, 11-03.
- Judgements in a game, pending since September:
  - Whether render scale 0.9 still looks better than 1.0 for screenshots, now that 1280x720 runs
    on the vendor's field. Archive, 09-27.
  - Whether Mortal Kombat 1 fits at render scale 1.0 now that the daemon needs 0.7-1.2 GiB. It
    was OOM-killed there on 09-16. Archive, 09-26.
  - ~~`min_extent` below 320 on a small window~~: no problems at high and low resolutions (the
    owner, 10-14).
- GitHub on 10-14: discussion #14 (a B570 in Mafia 3, and a Reddit post) has no answer. Issue
  #17 (DOOM at 3 fps with the GPU at 10 %) is answered and its GPU-path bug fixed, archive, 10-16.
  For #17 the owner's reading: when the whole frame waits on the network, the game has nothing to
  render past the network's own frame rate, so its load stays low. #12 and #15 work after a reinstall, and #12 waits for the next release. #16 is
  Paimonshen's in-game menu.
- ~~The clock hint in a game (`XMX_LOW_LATENCY`, on by default on Linux since 10-18): whether it
  stays on by default~~: the owner looked on 10-18 and keeps it on. What it costs in power and
  heat, on battery above all, is unmeasured. Archive, 10-18.
- ~~Arc A-series reports: asking issue #18's reporter and the A-series owners on Reddit~~: asked on
  11-01 (#18, the owner's Reddit post), and six A770 reports came in (below, 11-01 later). The
  answers to them wait for the owner's word.
- Preset B against the vendor's own B: a capture on the RTX 5060 Ti with
  `DLSSNR.Hint.Render.Preset` set to B, when the owner opens that machine. Archive, 10-17 later.
- The two files that came with the 310.9.16 DLL (`nvngx_dlssnr_compat-fp16-310.9.16-cuda13.4.3.rar`
  and `renodx-dlss.addon64`, in the Windows Downloads). The owner says, as their source claims,
  that the first is the DLL patched for RTX 30 and 40 cards, with FP16 kernels, and the second
  lets RenoDX pick preset A or B. Untrusted and unopened; they wait until one is needed.

**Linux**
- `alchemist`'s GPU report changes (`8f96b58`, `eab7eb3`, `c231bdc`, `9ceff90`, below, 10-31
  evening): `make test`.
- `speed_test.py`'s `network_of` and the probe's warm-up (below, 11-01 later): `make test`.
- The README split into `docs/DEVS.md` and `docs/SETTINGS.md` (below, 11-02): `make test`, for
  `test_toggle.py`'s page check and `nr_daemon.py`'s message.
- `player_reports.py`, its test and `knob_doc.py`'s third block (below, 11-03): `make test` and
  CTest, which now run `test_player_reports.py`.
- `test_toggle.py`'s "status has no hardware-independent FPS guess" fails about one run in 200:
  it looks for `7x` in `nr-ctl report`'s output, which prints the log's path, and the path is a
  temporary folder with a random name. Not from 11-03's change, and not fixed (11-03).
- ~~`81b0a59` (0.0.2): `make test` in both memory modes and CTest~~: green at `eb9b281` (10-31),
  which holds it and the panel in DirectX 12. Since `cc2b8b7` the setup window's core changed (its
  PE readers, the folder of each placed file, frames on the GPU and B by default), the release
  builder (`VERSION`, the icon) with its tests, and `nr_focus.h` and `nr_layer_state.h`, which
  Linux compiles. Both layers and CMake build with no warning; `make test` 60 of 60 in both memory
  modes, CTest 67 of 67, `make test-proton`, all 12 `D3D12ProtocolTests`; the Python suites of
  `src/tools` 217 tests, the 24 skipped all Windows-only. Below, 10-30 evening and 10-31.
- ~~`dx12`'s protocol 3 (`455a6fe`), step 3 (`04c4817`) and `330ef95`: `make test` in both memory
  modes and CTest~~: green at `cc2b8b7` (10-30), which is `newnr` with them and PR #20: `make test`
  60 of 60 in both memory modes, CTest 67 of 67, `make test-proton`, all 12 `D3D12ProtocolTests`.
  `test_image_history.py`'s device check skips on Mesa; run ungated on ANV, `image_history.comp`
  is within 3.1e-05 of `History.follow` and no STALE or NOTHING decision differs. `speed_test.py`
  puts `cc2b8b7` level with `2d1d550`. `publish_check --history` finds nothing new: what it lists
  (Paimonshen's local committer address on four commits of 10-02, two HANDOFF versions over 200 KB
  of 10-11 and 10-12) has been on GitHub since then, and rewriting `master`, `newnr` and the tags
  for it is not worth it.
- ~~**Preset B, the new model: recover its graph**, then the CPU reference against it~~: A's
  graph at depths `{2,2,2,2,2}`, running on CPU and on the device (`2bb16f6`, archive, 10-17
  later; phase72).
- Preset B in a game: `NR_PRESET=B` beside A, the look and the frame rate. Archive, 10-17 later.
- ~~A slight flicker of the network's own, which the owner saw on 10-18 and puts down to the
  network's low resolution~~: not the resolution. `flicker.py work/good`: what the network makes
  up on pixels the game left as they were is the same at the 320 floor and at full scale (A
  0.82 and 0.75 levels with history, B 0.39 and 0.44); B makes up half of A's and moves the
  frame half as far. The history holds it down 4x; what is left is detail drawn again where a
  pixel changed by a level or two, which the hold floor does not cover. Phase72.
- ~~Preset B's frame: the full-resolution blocks and the bottleneck are 45% of it at 1280x736.
  They are the next place to look for its speed~~: looked at on 10-18. Those passes are where
  the earlier work left them, and what was holding B's live frame back was the GPU's clock,
  which libxmx now holds up (`fe00b06`, archive, 10-18). In the graph, the two bottleneck GEMMs
  improve-b measured on 32-row blocks are worth 2 % of B's frame at 320x320. Phase72.
- ~~`improve-e4m3`'s four red tests on Linux~~: green here on `newnr`, which holds all of
  `improve-e4m3` (`bad7bcf`). `gpu_device_pick` takes Mesa's answer, and the image path's three
  parity tests skip with the reason, as `1a146c2` meant (10-18).
- ~~`dx12` changes the shared daemon (`MAGIC_MOTION`, `History.follow`) and `nr_image.c`
  (`nr_history_sample`): `make test`, and `src/ref/test_native_image.py` with
  `src/layer/test_temporal.py` in particular, please~~: green on `newnr`, which holds `dx12`,
  in both memory modes and in CTest (`2bb16f6`, 10-17).
- No Linux release has been published, but `deploy.sh --release` would build one without a 32-bit
  layer, so a 32-bit game under Proton (DoA5) could not use it. Archive, 10-07. Left until a
  Linux release is planned (10-18).
- A daemon started by a short-lived process (a launcher that makes a Vulkan instance before the
  game) ends with that process, and a game whose instance found it alive gets none after: the
  layer starts a daemon only in `vkCreateInstance`. The same on Windows. Not seen in a game, and
  left until it is (10-18).
- Optional: `build_check.py` could hold `build_win.bat` to the same floor (`/DNR_F16C`,
  `/fp:precise`, `/openmp`). The owner does not mind either way. Archive, 10-07.
- ~~Steam keeping a game "running" while its daemon lives~~: it did, and the daemon now ends with
  its game on Linux too. Archive, 10-14, later.
- ~~The README's live table~~: re-measured at `--warmup 3`, 10-14.

**Windows**
- ~~The GPU report for Arc A-series owners (`nr_gpu_probe.exe`, branch `alchemist`, below, 10-31
  evening): the MSVC build, a run on Intel's driver, and a double click from a release's ZIP;
  then the next release~~: in 0.0.3 (`86a5799`, 11-01).
- An Arc A-series path with 8x8 matrix tiles, from OUCO86's A770 port: begun on 11-01 and paused
  at the owner's word, on the local branch `arc-a-n8` (`6be5235`, not on GitHub). Below, 11-01.
- Where an A770's ~55 us a pass goes: a per-pass profile from an A-series tester
  (`frame_profile.py --warm`, timestamps per pass) would say whether passes or the gaps between
  them. It is half of a frame at 1280x720 there. Below, 11-03.
- The speed test measures the CPU path (no `NR_` setting reaches its daemon), while the Windows
  release keeps a game's frames on the GPU since 0.0.2. At 1920x1080 its round trip reads 10-20 ms
  worse than a game waits for. Below, 11-03.
- The GPU's clock: whether Intel's Windows driver also clocks a light network down between
  frames, as Linux's governor did (archive, 10-18). The power mode moved the host passes there
  already (10-15). libxmx's hint is Mesa's alone and does nothing on Windows.
- ~~Preset B: the setup window extracts preset A only. For a 310.9.16 DLL it could also run
  `src/tools/unpack_preset.py`, and offer `NR_PRESET` (A or B) in `nr-env.txt`~~: `a75e5da`, not
  yet in a game. Archive, 10-18, Windows.
- Holding the GPU's clock up on Windows, as libxmx does on Linux: no per-process hint found.
  Archive, 10-18, Windows.
- ~~PR #20 (Paimonshen, 10-16): the in-game settings menu on Insert, in four languages, with Dear
  ImGui vendored, against `master`~~: merged into `newnr` on 10-30 (`097d6d4`), after the last
  fixes were made on its branch by us; issue #16 closed.
- ~~The unmerged window attention hangs Intel's Windows driver from 32 windows up~~: found and
  avoided on 10-15, and `gpu_window_attention` is in CTest on Windows again. Archive, 10-15;
  phase71.
- The staged GEMM's loop on Intel's compiler takes 1.2-1.5x Mesa's time where the graph is large.
  The packed copies took 6-9 % of it; the rest is open. Archive, 10-02 and 10-03.
- ~~Four daemon processes with Mortal Kombat 11~~: two processes are one daemon (the venv's
  `python.exe` and the interpreter it starts); four were two. The owner lock now stops a second
  one. Archive, 10-15.
- DOOM (2016) through 0.0.1.2 is untested: the owner's own installation from another release is in
  its folder, and its removal was refused by this session's safety check. Archive, 10-15.
- `windows_launch.py`'s Configure Steam, Restore Steam and their wrapper can go, with their tests,
  once no release with those buttons is in testers' hands. The GitHub releases have none. Archive,
  10-09.
- ~~DirectX 12 is not offered by the release~~: the setup window's **DirectX 12 (games with
  XeSS)**, `6c503ae` (below, 10-30 evening). VKD3D-Proton does not run Hogwarts Legacy on Windows
  (archive, 10-16).
- A Vulkan or DirectX 8–11 game that runs elevated, or any with UAC off, never gets the layer; the
  HKLM manifest that would reach it is kept for later. Archive, 10-08 and 10-09.
- ~~The panel inside a DirectX 12 game, and its keys and clicks kept from the game~~: on `newnr`,
  in Hogwarts with the owner, frame generation on and off (below, 10-31 later). For the next
  release after 0.0.2, the owner's call. A game reading keys by `GetAsyncKeyState`, DirectInput or
  a gamepad still gets them while the panel is open.
- DirectX 12 in other games: only Hogwarts Legacy (XeSS 2.0.2.53) has run. Check takes any XeSS
  whose exports NR's proxy carries, and an older XeSS's execute parameters are taken to be laid
  out as 2.0's. Below, 10-30 evening.
- ~~`dx12`: record a real Hogwarts sequence with the owner~~: recorded and replayed on 10-16,
  and the trail found in it fixed (`e803624`). Its look in the game is untested. Archive, 10-16.
- MSVC: the composition is not vectorised (0.5-1.8 ms behind gcc), and `_Pragma("GCC ivdep")`
  gives three C4068 warnings that an `NR_IVDEP` macro would silence. Archive, 10-02 and 10-07.
- Housekeeping: each release root in `D:\NRonWindows\rel` holds ~440 MB of extracted weights, and
  `D:` ran out of space mid-test. Archive, 10-12.

**Planned, not started** (parked by the owner: do not start unasked)
- ~~Motion vectors from the game's own upscaler, through OptiScaler~~: started by the owner on
  10-16 for XeSS, without OptiScaler: `src/xess`, archive, 10-16.
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

## Players' reports as data, and where a frame's time goes on each GPU (2026-11-03, branch `alchemist`)

The owner asked for every player's report to be gathered, the frame's time placed, and the
performance tables updated from them. `notes/players-20261103.md` reads it all, and the numbers
are in `notes/measurements/players.json`.

- **Gathered:** issues #10-#34, the eight reports attached to them, eleven GPU reports (twelve
  GPUs) and two screenshots of the setup window in a game. The session's GitHub proxy refuses
  `user-attachments`, so the files came through the signed links GitHub redirects to. New
  since 11-01: #29 and #31 (A770), #30 (an A380 with a Pro B70 in the same PC), #32 and #33
  (A750), and #34 (B580, Assetto Corsa).
- **On the A770 the network's fixed part is a cost per pass:** 22 ms on network A and 11 on B,
  in proportion to their pass counts (415, 179), although B keeps A's bottleneck. That is
  ~55 us a pass, against 9-10 on the B580 and 23-26 here. At 1280x720 and 0.4 it is 60 % of the
  A770's network time, so the card plays level with this laptop there (35 against 32-34 ms on
  A, 19 against 16 on B), although it does a megapixel in 0.6-0.7 of the time.
- **#23 measured network A.** **#25's "Vulkan 23 fps, DirectX 11 40" is network A against B**:
  the DirectX 11 frames took 24 ms (its screenshot), and the Vulkan ones 42, of which the
  network 37 (its log).
- **Where a game's frame goes:** on the A770 at 1280x720, network A, frames on the GPU, 42 ms.
  About 22 is the passes' fixed cost and 15 the network's work on its pixels. Prep and compose
  take 1.8, and history, statistics and hand-back 3.2. The game's own frame adds a millisecond
  or two. On the CPU path a B580 spent 35 of 60 ms at 1080p outside the GPU, and the GPU path
  took its player's frame rates up 2.1-2.9x.
- `src/tools/player_reports.py` (`digest`, `probe`, `fit`) turns a Save report or a GPU report
  into the data file's entries, keeping no folder, user name or process id.
  `test_player_reports.py` runs in `make test` and CTest. `knob_doc.py` renders a third block
  into `docs/SETTINGS.md`: the network's time on each GPU, from the data file.
- The README's GPU table gains the Pro B-series, the eleven A-series cards and the B580 as the
  fastest. What to expect gives the B580's and the A770's figures, and the tested-games table
  seven players' rows.

**Checked here:** `test_player_reports.py` (7; the privacy case fails when a digest keeps the
report's folder), `knob_doc.py --check`, `test_toggle.py`'s page check, `build_check`,
`claims_check`, `publish_check`. **Next on Linux:** `make test` (Open items). **Next:** the
owner's word on answers to #23, #25, #30 and #32-#34, and on asking an A-series tester for a
per-pass profile.

## The README rewritten for players; the developers' half in `docs/DEVS.md` (2026-11-02, branch `alchemist`)

The owner found the README out of date in places and too long for players. It is now short and
for them: whether a GPU and a game's API are supported, the Windows install in five steps, what
to expect, a tested-games table with the date of each check, and what to do when nothing changes.
The rest moved nearly word for word to `docs/DEVS.md` (how this was built, the build, the layer
and the daemon, the tools, troubleshooting, layout, tests), and the settings and frame times to
`docs/SETTINGS.md`, whose two generated tables `knob_doc.py` now writes. The owner chose the
names and English, and the DLL's source as a game the player owns, such as NBA 2K27.

- Out of date and fixed on the way: DirectX 12 through XeSS missing from the opening, network B
  nowhere, no word of the panel on Insert or of Home, the window's Chinese, Arc A-series called
  unsupported at 1-2 fps against the six A770 reports, a layer-started daemon said to outlive
  its game on Linux (it ends with it since 10-14), the driver 101.9033, and credits without
  DLSSNR-AMD, Dear ImGui and Paimonshen's panel.
- `docs/RELEASE-QUICKSTART.md`, the release's own README, had the same stale lines (A-series,
  three languages, no panel, the speed test measuring one network) and is fixed too.
- `test_toggle.py` checks the links of `docs/DEVS.md` and `docs/SETTINGS.md` as well, each
  relative to the page that holds it, as GitHub reads them.

**Checked here:** `knob_doc.py --check`, `test_toggle.py`'s page check, `claims_check`,
`publish_check`. **Next on Linux:** `make test` (Open items). **Next on Windows:** nothing; the
next release carries the new release guide.

## 0.0.3.1 released: the first Arc A-series reports, and the fixes they asked for (2026-11-01, later, branches `newnr`, `master`, `alchemist`)

**The reports** (issues #22-#26 and #28, all A770 16GB, 10-31 19:30 to 11-01 06:00 UTC; five can
run test builds). Every card offers float16 8x8x16 and int8 8x8x32 only, subgroups 8-32, 48 KB of
shared memory, and **runs NR's 8x16x16 at subgroup 32 exactly** (101.9150 on five, 101.9034 on
one): NR computes right there, slowly. Matrix rates on a typical card, TFLOP/s at subgroups 8, 16
and 32: 8x8x16 57-59, 58-60, 36-39; the 8x16x16 it does not offer 64-69, 58-63, 38-41; vector
float32 FMA 19-21 (this 140V: 28-31 and 3.4-3.9). So 8x8 tiles alone (`arc-a-n8`) would not speed
the units up, and the width would: 32 costs these cards ~40 %. The one speed test (#23) against
phase72's Windows table: 32 ms at 640x360 and 0.4 against this 140V's 24-27 under A, 229 at
1920x1080 and 1.0 against 381-404. The A770 does a megapixel in half the time and loses at the
small fields games run at: a fixed ~21 ms a frame, which only a per-pass profile on such a card can
place. Which network it measured is unknown (below). Games: #23, Hogwarts at 1440p, XeSS Quality,
frame generation x2 and render scale 0.4 at 55-60 fps, too fast for NR on every frame by that
test; #25, Nuclear Option through DXVK at 720p and 0.4, 40 fps with NR's picture, and two Vulkan
games where NR did nothing. #27: NBA 2K27 keeps its `libxess.dll` in a folder setup did not look in.

**Fixed:**
- The speed test named no network since 346d300, in 0.0.2 and 0.0.3: the daemon's line became
  `network preset B (dlssnr-b-logical.safetensors), 31 blocks`, and `speed_test.py` read only
  `network preset B, 31 blocks`. `network_of` reads both, and a test holds it to both.
- The setup window's speed test measures every network the release carries, A first, and each
  case keeps its network; the chooser beside the button is gone (the owner, 11-01). The result's
  `networks` lists what each daemon said it ran. A request naming one network measures that one.
- `xess_libraries` also looks in the folders under the game's executable, four deep, NR's own
  `dlss-nr` apart, where neither place it knew has a `libxess.dll`.
- The probe runs half a second of its first rate's work before timing it: #22's first two rates
  came out at half and 0.8 of the rest.

**Checked:** MSVC `build_win.bat` (the three known C4068); `test_windows_wizard.py` 95 (1
skipped), with new cases for both networks, the daemon's two lines and a subfolder's XeSS; the
window's self-test (89 controls, 197 keys), with two networks shown apart; `test_release.py` 21 (5
skipped); the probe on this 140V (1.5 s, "run here"); `speed_test.py --preset B` on the real
daemon records B and 31 blocks.

**The release.** `v0.0.3.1` at `c0b7f9d` (`VERSION` 0.0.3.1, the owner's name for it), on
`master`, `newnr` and `alchemist`, on the owner's word: `dlss-nr-windows-0.0.3.1-x64.zip`,
21 551 982 bytes, SHA-256 `2f07c0f2c87d60556a1046fab649968408b9d8667d93fab9cf615c4a6aab4012`
(GitHub's digest the same), its notes in `work\pr-replies\release-notes-0.0.3.1.md` on Windows.
From the unpacked ZIP: the window's self-test, the probe ("run here"), and on a stand-in (the
SDK's vkcube in a scratch folder) Check, Install with both networks out of the 310.9.16 DLL, the
speed test as the button runs it (A then B, 37 s; the daemons said A, 71 blocks, and B, 31), Save
report with 19 checks and both networks, Remove NR, the folder as before. No game ran on it: no
game-side code changed.

**Next on Windows:** answers to the reporters (#23: which network, and whether NR was on; #25: Save
report for the Vulkan games; #27: where the DLL is), at the owner's word.
**Next on Linux:** `make test` for `speed_test.py` and the probe (Open items).

## 0.0.3 released: the panel in DirectX 12, and the GPU report for Arc A-series (2026-11-01, branches `newnr`, `master`, `alchemist`)

**The release.** `v0.0.3` at `86a5799` (`VERSION` 0.0.3), on `master`, `newnr` and `alchemist`, on
the owner's word: `dlss-nr-windows-0.0.3-x64.zip`, 21 551 071 bytes, SHA-256
`acf22fa84111f8c73436b6a2069622b7c59c2c4e95a9604b371cdcf55eb0f2c1` (GitHub's digest the same), its
notes in `work\pr-replies\release-notes-0.0.3.md` on Windows. It carries the panel inside DirectX 12
games and its keys and clicks kept from the game (below, 10-31, later), and `nr_gpu_probe.exe` with
the fixes of 11-01 (below, 10-31, evening), and asks Arc A-series owners to send the report and to
try NR as it is there. It went out before Linux's `make test` on the probe's changes (Open items).

**Checked from the candidate ZIP:** the eight Windows tool suites (217 tests, 8 skipped); the
window's self-test (90 controls, 201 keys in four languages); the probe double-clicked from the
unpacked ZIP; and setup end to end on a stand-in, the SDK's vkcube in a scratch folder, so that no
game of the owner's was touched: Check, Install with both networks taken out of the 310.9.16 DLL,
Enable NR, 600 frames all answered on network B, Save report with 19 checks and no warning, Remove
NR, the folder as before and no daemon left. No game ran on this ZIP; the panel's code is the one
Mortal Kombat 11 and Hogwarts ran on 10-31.

**The Arc A-series path, begun and paused.** At the owner's word the 8x8 tiles of OUCO86's A770 port
went in, and then, at the owner's word again, were held back from 0.0.3. They are on the local
branch `arc-a-n8` (`6be5235`, Windows checkout only):
- the six matrix shaders on the network's path take `XMX_TN`, 16 by default and 8 for the nine
  `*_n8.spv` builds, a workgroup covering the same block either way, so nothing that dispatches
  them changes;
- Xe2's builds compile byte-identical to before; the 8x8 builds are byte-identical to OUCO86's A770
  shaders compiled with our includes (`window_attention`: theirs with our 10-15 store fix, by a
  three-way merge), and pass `spirv-val`;
- libxmx takes a device offering float16 8x8x16 as able and builds `name_n8.spv` where one exists
  (`XMX_N8=0` keeps the 8x16 kernels, `1` forces the 8x8 ones); it compiles, and has not run;
- left: the Makefile and CMake, the release builder's shader list, a test, OUCO86's credit in
  NOTICE (Apache-2.0), and an A-series card: nothing here can run 8x8x16 right.

**Next on Windows:** the owner's word on asking A-series owners (issue #18, Reddit; drafts in
`work\pr-replies\arc-a-call.md`), and on taking up `arc-a-n8` again.
**Next on Linux:** `make test` on the probe's changes (Open items).

## A GPU report for Arc A-series owners to send in (2026-10-31, evening, branch `alchemist`)

**Why.** An A750 runs NR at 1-2 fps (issue #18, and comments on Reddit). Alchemist's matrix units
offer float16 8x8x16 into float32 (DPAS at SIMD8: Mesa reports `NSize = exec_size`, 8 before Xe2
and 16 on it), and every XMX kernel here is written for 8x16x16 at subgroup 32. libxmx says the
device lacks the shape and runs the kernels anyway, and Intel's driver evidently emulates them. A
path for those cards has to be designed from what they report, and has no card here to be tested
on, so the owner chose the cheapest step first: a report A-series owners can send. The
A-series work has a branch of its own, `alchemist`, from `newnr` at `0a6000f` (the owner, 10-31).

**What.** `coopmat_probe` also measures now: for each float16 8xNx16 into float32 shape a device
offers, at each subgroup size it allows of 8, 16 and 32, one product over K = 64 checked exactly,
the matrix rate, and the vector float32 FMA rate, which a path without XMX would run on. ~~It never
runs a shape the device does not offer.~~ It tries 8x16x16 where it is not offered too, since NR's
kernels run it there (11-01, below). The shaders are `src/probe/coopmat_bench.comp`, built as
`coopmat_bench8.spv` and `coopmat_bench16.spv` and read beside the program or from `work/` beside
it. The report also goes to `nr-gpu-report.txt` in the folder it runs from, and on Windows a double
click keeps the console open. make (`all`), CMake and `build_win.bat` build it; the Windows
release carries it as `nr_gpu_probe.exe`, with the shaders in `work\`. The issue template
`arc-a-series.yml` asks for the report, the driver, NR in a game, and whether they can test builds;
the README and the release quick start send A-series owners there. `make test` runs the probe and
requires "run here".

**Here** (Arc 140V, Mesa 26.2.5): 8x16x16 exact at subgroups 16 and 32, 31.6-31.8 TFLOP/s on the
matrix units, best of eight runs since the first ones climb with the clock; vector float32 FMA
3.5-4.0 TFLOP/s; 8x8x16 not offered. On an A-series card the report says at which subgroup sizes
8x8x16 is exact and how fast: whether NR's kernels can keep 32 lanes with two 8x8 tiles where they
have one 8x16, or need 8 or 16. It also gives the vector units' rate, for a path without XMX.

**On Windows** (11-01, Intel's 101.9150, on mains, quiet before and after): `build_win.bat` builds
both shaders and `coopmat_probe.exe`, which is clean at `/W4` too. The probe finds phase71's four
configurations and no 8x8x16; 8x16x16 is exact at subgroups 16 and 32, at 28.1-30.8 TFLOP/s over
three runs, a little below Mesa's 31.6-31.8 and wider; vector float32 FMA is 3.9 TFLOP/s at 16 and
3.4-3.7 at 32; "run here". A run takes under a second. `test_release.py`: 21, the 5 that need bash
skipped. `deploy.bat --release` puts `nr_gpu_probe.exe` at the release's root, the same bytes as
`work\coopmat_probe.exe`, and both shaders in `work\`. From the unpacked ZIP, started as Explorer
starts it (a console of its own, its folder as the working directory), the report was whole beside
it after 1.3 s, the console still open 3 s later, and Enter, written into that console's input,
closed it with 0.

**Fixed there** (11-01, the owner's call):
- Without its shaders beside it (copied out alone; run from inside the ZIP, untried, it likely is)
  the probe ended "NR's kernels ...: do NOT run here" on this 140V. It now says "not checked" and
  that a release has to be unpacked first (`8f96b58`).
- A release unpacked under a folder outside the ANSI code page (Cyrillic on this en-US Windows,
  code page 1252) found no shader at all: the probe built their path from the ANSI form of its
  own, all question marks. It asks for it in UTF-16 now (`eab7eb3`). Before `8f96b58` that, too,
  read "do NOT run here".
- The last lines give the report's full path, through the wide API on a console, and on the
  terminal only (`c231bdc`): the file is what gets posted, and a path names the user. A failed
  `vkCreateInstance` goes into the report, and a double-clicked console stays open after it too.
- Checked: MSVC (also `/W4`) and MinGW gcc at the Makefile's `-Wall -Wextra`, no warning. A release
  of `c231bdc` unpacked under `проба\` and double-clicked: "run here", the path right on the console
  (read back from its screen buffer) and not in the file, Enter closes it. With the loader pointed
  at no driver: the failure on the console until Enter, exit 1.

**A shape a device does not offer can still run, wrong** (11-01, Intel's 101.9150): a scratch
probe that asked this 140V for 8x8x16 got a pipeline, no error, and 61 of 64 values wrong at
subgroups 16 and 32. So an A-series card, which does not offer NR's 8x16x16, may be computing it
wrong as well as slowly: nobody has said how NR's picture looks there. The probe now tries 8x16x16
where it is not offered, after saving the report in case the driver does not survive it, and says
"exact here, though the GPU does not offer the shape" or WRONG (`9ceff90`). Checked here by a
build that takes 8x16x16 for not offered: it walks that way and finds it exact. And an 8x8 path
cannot be checked on this 140V at all.

**Not tested:** any A-series card: 8x8x16 has never run anywhere here.

**Prior art, found 11-01:** `OUCO86/dlss-nr-on-a770` (Apache-2.0) is this code running NR on an
A770 beside an AMD card that renders the game. Its kernels are ported to the A770's 8x8x16 tiles
(`TN = 8` in the GEMM shaders, `XMX_TN` in libxmx; they no longer run on Xe2), plain GEMMs go to
the tiled kernel because the staged one ran at 0.4-0.6 of it there
(`notes/staged-vs-tiled-a770.md`), and it has an int8 path. Its float16 graph at 768x448 takes 43.5
ms on the A770, 36.4 with int8 (`docs/A770-HANDOFF.md`, driver 101.9030), about this 140V's time,
against the 1-2 fps of #18. Their 8x8 variants, chosen only on a device that offers 8x8x16 without
8x16x16, are the shortest way to an A-series path; nothing here can run them, so the A-series
testers would be its only check.

**Next on Windows:** ~~`build_win.bat`, `work\coopmat_probe.exe` on the Arc 140V, a release built
with it, and a double click on `nr_gpu_probe.exe` in the unpacked ZIP~~: done on 11-01 (above).
Then into the next release, at the owner's word.
**Next on Linux:** `make test` for `8f96b58`, `eab7eb3`, `c231bdc` and `9ceff90`: Linux compiles `say_where`'s
`realpath` arm and `open_beside`'s, the latter rearranged around the Windows one but doing what it
did.

## The panel inside DirectX 12, and the panel's keys and clicks kept from the game (2026-10-31, later, branch `newnr`)

**One panel, two renderers.** The panel's logic left the Vulkan layer's file for one both hosts
share: `nr_panel.c` reads the keys behind the focus gate, holds the pointer, builds the rows and the
ImGui frame and applies what was asked; `nr_menu_layer.c` keeps the Vulkan hooks and the composite.
Drawing is each host's: `nr_menu_ui_vk.cpp` for Vulkan, and for DirectX 12 a renderer of ImGui's
draw data written here (`nr_menu_ui_d3d12.cpp`, its `.hlsl` compiled by fxc at build time), since the
owner chose not to vendor ImGui's D3D12 backend (10-31). It draws the panel into an 8-bit image of its
own and lays that over the back buffer in its encoding: SDR, scRGB, or HDR10 with white at 203 nits.

**Where DirectX 12 draws it** (`src/xess/nr_xess_panel.cpp`): in `IDXGISwapChain::Present` and
`Present1`, by writing into the swap chain class's vtable in `dxgi.dll` as the cut writes into
D3D12's classes, and `SetColorSpace1` for the encoding. `nr_present_probe.exe` finds the vtable in a
process of its own, as DLSSNR-AMD does, because a swap chain made inside the game is seen by whatever
else hooks there. Its answer is kept in the game's `dlss-nr\nr-present.txt` while `dxgi.dll` keeps
its timestamp and size, and goes with Remove NR; here the vtable is at `dxgi.dll` +0xcd688 and the
queue at swap chain +0x140. DXGI does not name the queue (`GetDevice` refuses
`ID3D12CommandQueue`), so the panel reads that field and trusts it only as a direct queue seen
executing command lists (`nr_split_watch` hooks the queue class from the first Present); failing
that, the direct queue the presenting thread last used; failing both, no panel on that Present.
XeSS frame generation presents through a swap chain of dxgi's class too, and in Hogwarts the panel
was there and usable with it on (below). The release carries the probe at its root
(`build_release.py`), and `NR_MENU=0` turns the panel off.

**The panel's input kept from the game** (`nr_input_guard.c`, both hosts). While the panel is open
the game's window is subclassed, and its procedure sees no key going down, character, pointer
movement, button, wheel or `WM_INPUT`. A release reaches it only when it saw the press, so nothing
stays held behind the panel; Alt+F4 still closes the game. The subclass stays for the process,
passing everything while the panel is shut, and the module is pinned. `nr_raw_mouse.c` also takes
the keyboard's raw-input registration while the panel has the pointer. Not covered, as the owner
chose no user32 detours (10-31): a game polling `GetAsyncKeyState`, DirectInput, a gamepad. The
panel's hint no longer says the game sees its keys, in four languages, and the generator and
`nr_menu_strings.py` lose the "Experimental:" GPU label `1b8b926` had dropped from the C table only.

**Mortal Kombat 11 from a test release** of this tree (DXVK, so the Vulkan renderer; B, frames on
the GPU, 1280x720 at 0.4), driven from here at the owner's word while the owner was away:
- The panel opened with INS as before. Render scale 0.40 to 0.45 in it took the daemon's network
  from 512x320 to 576x384 live, and back.
- In the main menu, Right and Down twice given to the open panel left the game's selection where it
  was, and Right after INS moved it. In a practice fight, D held 1.2 s with the panel open left
  Scorpion in place, and the same D with the panel shut walked him to Sub-Zero (MK11 walks on WASD).
- The panel's cursor followed the mouse at its raw gain of 0.5, and a click held 150 ms on the NR box
  turned NR off (no frame for 3 s) and on. A press and release sent at once was not seen: the panel
  samples the button once a frame (`GetAsyncKeyState`), so a click shorter than a frame is lost, as
  a quick real one at a low frame rate could be. The guard now sees the button messages while the
  panel is open, and the panel could take them from there; not done.
- Home off and on with the panel shut. 6025 frames, none rejected, ~25 ms each; the daemon ended
  with the game, and after Remove NR the game's 10 024 files matched the listing taken first.
- While the panel held the pointer, `GetClipCursor` from outside gave the whole screen, not the
  one-pixel box the panel asks for every frame; the pointer stayed put because the panel also sets it
  every frame. The split did not change that code. In Hogwarts a recorder does see the box (below),
  so this one is MK11's or the reading's; not looked into.

**Hogwarts Legacy with the owner** (Epic, XeSS 2.0.2.53, a 1280x720 window centred on the 1920x1200
screen, B at 0.4, `mode=after`), two runs from test releases of this tree installed through the setup
window's backend (`D:\NRonWindows\nr-game\hogwarts-panel-test.ps1 -Phase install|status|remove`):
- The first run took the Present hook from the probe's answer at once and made the panel on the
  game's device. The owner, through a list of checks with frame generation off and then on:
  everything works, the panel and its keys, clicks and mouse look kept from the game, NR off and on
  by a click, except the cursor, which left the window as the camera turned once the panel had been
  open. About 26 frames a second, each answered on its own frame, none declined.
- The cause: Unreal confines the cursor to its client area (`ClipCursor`, here 320,240-1600,960)
  once, when it takes the mouse, and again only when its window is activated; the panel let go with
  `ClipCursor(NULL)`. Now `nr_panel.c` notes the game's box when it pins the cursor
  (`game_clip_note`: the whole desktop is how Windows says "none", and the panel's own pin left from
  an earlier frame is not taken for the game's), keeps it from the client area's origin, and puts it
  back when the panel closes with the game in front. Another window in front, the pointer pushed out
  past an edge, and the panel going with its host still leave the cursor free. `test_menu_keys`
  checks the shape; a running test would have to take the user's own cursor.
- The second run, with the fix and frame generation on throughout (the game's
  `GameUserSettings.ini`: `Intel_XeFG`, x2): a recorder reading `GetClipCursor` four times a second
  saw the game's box before the panel, the one-pixel pin while it was open, and the game's box again
  after each of four closes; pushed out at the bottom and at the left, the cursor left the window
  and came back under the pin; with another window in front, no box. The owner: the cursor stays in
  the window. NR off and on from the panel, every frame answered.
- The game closed with the panel open and left the cursor free. After each Remove NR the game's 568
  files matched the listing taken before the first install but for Epic's launch token
  `.egstore\...ovt` (its time only, rewritten at every start), as on 10-30, and the XeSS folder
  hashed as before.

**Checks.** MSVC `build_win.bat` with the three known C4068; fixtures `test_menu_*` 7 of 7,
`test_raw_mouse` and the new `test_input_guard` (26 checks). `build_xess.bat`: `test_nr_xess` all
passed, the new `test_xess_panel` 8 of 8 (the hook through the probe, the queue from the swap
chain's field, the panel over a green frame). MinGW (UCRT64, gcc 16.2) CMake and CTest 66 of 66,
`test_focus_gate` left out (it moves the focus). Not run: the Makefile on Windows (no make in this
MSYS2), `check_imgui_pristine` (needs imgui-1.93.0.zip; the vendored files are untouched). After
the cursor fix: both MSVC builds again with no new warning, the fixtures again with `test_menu_keys`'s
new check, and the MinGW CMake build (CTest carries no menu fixture).

**Next on Windows:** the release, at the owner's word. The panel could take its clicks from the
guard's button messages, so a click shorter than a frame is not lost; not done. DirectX 12 in
other XeSS games.
**Next on Linux:** ~~`make test` in both memory modes and CTest: `nr_focus.h` and `nr_layer_state.h`
changed, which Linux compiles (`nr_panel.c` is Windows-only)~~: green at `eb9b281` (Open items).

## 0.0.2 released, and what Mortal Kombat 11 found in the panel (2026-10-31, branch `newnr`)

**The release.** `v0.0.2` at `81b0a59`, on `master` and `newnr`, on the owner's word and with the
owner's own text for the notes: `dlss-nr-windows-0.0.2-x64.zip`, 21 001 345 bytes, SHA-256
`f1b71663b6d814bc95d91f626157ce120adba843470aa9a534bce614ed31d242`. Beside DirectX 12 (below, 10-30
evening) the window gained an About tab, with the version and build from `release-metadata.json`
(which carries `VERSION` now), links and credits, that asks GitHub's `releases/latest` at most once
a day and says when a newer release is out (`9ea08ad`); and an icon, a pentagon in Intel's blue,
which `tools/make_icon.ps1` draws at build time since the repository takes no binary (`3e7cce1`).
Network B is chosen by the installation that brings it, and frames on the GPU are on unless
turned off (`1b8b926`, `01fa63c`).

**Mortal Kombat 11 from the candidate ZIPs**, the owner playing, then driven from here at the
owner's word:
- The panel held the cursor in a one-pixel `ClipCursor` box that only the layer let go of, at
  present: in a game that stops presenting without the focus the Start menu and a screenshot's
  selection could not be used, and the cursor never left the game. `nr_clip_guard`, a thread in
  `nr_focus.c`, now lets go of the clip whenever the game is not in front, and a pointer pushed
  48 px past the edge goes out there (`1b8b926`; `test_focus_gate` checks the guard).
- Arrows pressed in the game after the panel closed moved its rows at the next opening: the layer
  hands ImGui every key change on every frame, and ImGui queues events until `NewFrame`, which runs
  only while the panel is drawn. `nrex_listen` keeps keys out of the queue while the panel is shut
  and empties it on opening (`81b0a59`); `test_menu_side`'s new case fails on the old code. In the
  game (`steam://rungameid/976310`, keys sent only while MK11 was in front) the live preview did
  not move after four Lefts given to the game.
- The first B choice ran only where `unpack_preset.py` ran, and a release's `get_weights.py` takes
  B with A, so it never did (`01fa63c`).
- Still so: keys and clicks given to the panel reach the game as well (Open items).
- B on the GPU path ran at about 25 ms a frame (1280x720, render scale 0.4), and each Remove NR
  left the game's 10 024 files as they were.

**Next on Windows:** the panel inside DirectX 12 games, and the panel's input kept from the game.
**Next on Linux:** ~~`make test` in both memory modes and CTest at `81b0a59`~~: green at `eb9b281`
(Open items).

## DirectX 12 through the release: the setup window's third API, and the proxy starts the daemon (2026-10-30, evening, branch `newnr`)

**What a user does** (`6c503ae`). The setup window offers **DirectX 12 (games with XeSS)** beside
Vulkan and DirectX 8–11. **Check** finds the game's `libxess.dll` beside the executable or, for an
executable in `<game>\<project>\Binaries\Win64`, under the project's or the engine's `Plugins`
(Hogwarts: `Engine\Plugins\Runtime\Intel\XeSS\Binaries\ThirdParty\Win64`). It refuses one with an
export NR's proxy lacks (both export tables read, `pe_exports`), NR's own with no record of it (set
aside, it would load itself), and a 32-bit game. **Install NR** copies the game's DLL to
`libxess_nr_orig.dll` beside it, sets it aside in `dlss-nr\game-backup`, puts the release's
`nr_xess.dll` in its place and writes `nr-xess.txt` (`mode=after`, the release's pipe, `env=` the
game's `dlss-nr\nr-env.txt`); no `vulkan-1.dll`. **Remove NR** puts it back; a `libxess.dll` a game
update put over NR's stays, and the rest goes (`updated_by_game`). `game-files.json` now records
each placed file's folder.

**The proxy starts the daemon.** With `env=` set, `nr_xess.c` reads `nr-env.txt`, and when no daemon
answers its HELLO starts one as the layer does: `NR_PYTHON NR_DAEMON --socket --settings --root`,
its output into `NR_LAYER_LOG`, `NR_LAYER_SPAWNED` for `end_with_game`, `NR_GPU_IMAGE=1` and
`NR_GPU_TRANSPORT=1` over the file's, and none of the Vulkan layer's names, so the daemon loads no
layer (`CreateProcessW` with a UTF-16 environment, only its stdin and log inherited). Not while one
it started lives, and three times a game at most. `NR_LAYER_TRIGGER` is looked at once a second in
`xessD3D12Execute`, so Enable and Disable NR work in the running game. Without `env=` nothing
changes (the test scripts); `replay_nr_xess --env` takes the same path.

- Offline, the orbit recording with no daemon running (`work/tools-win/run_replay_spawn.py`): the
  proxy started one, whose session began 5.4 s later; 60 frames answered, the trigger removed (1
  in flight), put back, 121 more; the daemon ended with the replay.
- **Hogwarts from a release build** (`build_release.py` of this tree, installed through the bridge
  as the window does; preset B, render scale 0.4, 1280x720, the owner playing): the daemon started
  at the first XeSS frame and took the session 6 s later. 26-27 frames a second were answered, each
  on its own frame, 34-69 ms from copy to answer, the daemon's frame 33-40 ms at 512x320. Disable NR
  stopped it within a second and Enable NR brought it back. The daemon ended with the game, and
  after Remove NR the game's 568 files matched the listing taken before, Epic's own `.egstore`
  record apart. The owner: it looks good.
- Tests: `test_windows_wizard.py` 91 (3 new: the Unreal layout, the refusals, an update over NR's),
  the bridge, controls, setup, launch and release suites; the window's self-test (75 controls, 179
  keys in four languages); `test_nr_xess.exe`; MSVC `build_win.bat` with the three known C4068.

**Next on Windows:** the release, at the owner's word.
**Next on Linux:** ~~`make test` in both memory modes and CTest at `6c503ae`~~: green at `eb9b281`
(Open items).

## DirectX 12 takes PR #20's live network swap, and `newnr` goes into `dx12` (2026-10-30, branch `dx12`)

**`newnr` into `dx12`, not yet the other way.** PR #20 is merged into `newnr` (`097d6d4`, 10-30):
the in-game panel, and a daemon that changes its network between two frames without dropping the
game's GPU imports. `dx12` takes it (`a4fc09a`, then `568cba3`, the Windows make naming
`nr_file.c` and `nr_focus.c` once) before it goes back into `newnr`.
- `notes/HANDOFF.md`: both lines added dated entries in one place; `dx12`'s, the newer, go above
  PR #20's.
- A conflict git could not see: PR #20 took `max_pixels` out of `unpack_packet` and
  `GPUSharedTransport`, and `dx12`'s protocol 3 tests still passed it. They call the new
  signatures now; no case relied on the ceiling.
- Checked on the identical tree, built as a trial merge in a scratch clone. MinGW CTest with the
  layer off and the GPU smoke tests all pass, `gpu_win32_d3d12` and `_network` among them; with
  the layer on only the four POSIX-only tests fail, as on `newnr`. MSVC: `build_win.bat` with the
  three known C4068, the nine menu fixtures, `build_xess.bat` and `test_nr_xess`, and CTest's
  Python entries.

**A preset changes in the running daemon.** The daemon applies a `preset` from its settings
preview while the process named in it runs. The panel writes that preview in a Vulkan game; for
the DirectX 12 path the test script now writes it, owned by the game's process
(`hogwarts-xess-test.ps1 -Phase preset -Preset B`, outside the repository). The model is swapped
between two frames, the session and its D3D12 imports stay, and the proxy opens no new session.
`-Phase daemon`, a new daemon and the proxy's new session 5 s on, stays for a daemon that died.
- The orbit recording (`work/tools-win/run_replay_swap.py`: A, then B after 30 frames, a frame
  every 200 ms): no frame went unanswered. 0.20 s passed from the last A answer to the first B
  one, the replay's own interval; A answered 31 frames and B 265, every written frame but the
  first carries its answer, and the proxy logged no new session. The restart of 10-28 left 5.1 s.
- **Hogwarts**, the owner playing (1280x720, `render_scale` 0.5, network 640x384, 101.9150, on
  mains). Every frame answered on its own frame, none declined; the game waits for the swap
  about a second, and the owner saw NR stay on (10-28: 5.5 s without it):

| switch | the model's load | the five seconds around it | then |
| --- | --- | --- | --- |
| A to B | 0.6 s | 63 frames answered, against about 76 | B, 21 a second, the daemon's frame 44 ms |
| B to A | 1.2 s | 82 frames in 6.0 s | A, 15 a second, 63 ms |

- `work/` was rebuilt at the merged head first: PR #20 moved `image_compose.comp` and
  `image_preprocess.comp` to a two-dimensional workgroup grid, which gives the same index below
  4 194 240 pixels.

**Next on Windows:** ~~`dx12` into `newnr`, a fast-forward now, and both to GitHub at the owner's
word, after Linux~~: on GitHub at the owner's word before Linux ran (`cc2b8b7`). Then DirectX 12
through the release: the setup window putting the proxy beside a game's XeSS.
**Next on Linux:** ~~`make test` in both memory modes and CTest at `dx12`'s head~~: green at
`cc2b8b7` (Open items).

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
notes/      24 findings documents
src/tools/  PE/resource readers, the weight reader (now superseded), model_spec,
            and the PTX analysis tools: ptx_trace (dataflow), ptx_addrform
            (address → linear form), ptx_chains (accumulator chains)
src/ref/    nr_model, nr_frame, nr_temporal, nr_display, nr_accel, image_io
src/gpu/    xmxres, nr_resident, nr_frame_resident   <- the device path
src/layer/  nr_layer.c, nr_daemon.py, nr-photo       <- into a game
            hnet_*, forward, run_frame            <- superseded, kept for their findings
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
python3 src/gpu/test_gemm.py                      # worst rel 2.4e-06
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

The old harness (`src/tools/model_spec.py`, `src/ref/hnet_*.py`, `run_frame.py`) still
runs but is built on the wrong weight decode; its scores measure scaffolding.

---

## 7. Hardware (probed, trust it)

Intel Arc 140V-class Xe2, Mesa ANV, Vulkan 1.4.354, subgroup 32. Six cooperative-matrix
configs, all scope=subgroup, all M=8 N=16; the path is **fp16 × fp16 → fp32** (config 1).
`cooperativeMatrixRobustBufferAccess = false`, so edge tiles need explicit padding.
Shared-memory APU: correctness and capacity are unaffected (141 MiB of weights against
an 11.46 GiB heap); it caps **bandwidth**, already the measured ceiling at 0.7–1.9
TFLOP/s with a naive kernel. `libxmx.c` still memcpys into a mapped HOST_VISIBLE buffer
on every dispatch — on an integrated GPU those copies are free to remove.

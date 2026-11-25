# HANDOFF — read this first

State of the DLSS-NR on Intel Xe2 project as of **2026-11-05**. notes/CLAUDE.md holds the
brief, shortened on 2026-11-04 (the original is `notes/archive/CLAUDE-until-2026-11-04.md`);
**this file overrides it wherever they disagree**.

**How to read this file.** "Where things stand" and "Open items" are the current state. The dated
entries after them are the latest, newest first. Everything older is in `notes/archive/`, word for
word: the graph's recovery, the kernel work, the measurements and the experiments that did not pay
in `HANDOFF-until-2026-10-07.md`, the first Windows releases and the testers' reports in
`HANDOFF-2026-10-08-to-2026-10-15.md`, DirectX 12 through XeSS, preset B and PR #20's review in
`HANDOFF-2026-10-16-to-2026-10-28.md`, DirectX 12 in the release, 0.0.2, the panel inside DirectX
12 and the GPU report for Arc A-series in `HANDOFF-2026-10-30-to-2026-10-31.md`, and 0.0.3 and
0.0.3.1, the Arc A-series reports, the players' data and the staged GEMM's walk order in
`HANDOFF-2026-11-01-to-2026-11-04.md`, with the Open items closed by 11-11 after them, 0.0.4 and
0.0.5, OptiScaler, the frame counter and the Hogwarts profile in
`HANDOFF-2026-11-05-to-2026-11-06.md`, the E4M3 and by-column weights, PRs #39 to #41, 0.0.6 with
FSR 3.1, the edit cache's first build, the DirectX 12 waits, the Arc A-series switch and PR #45's
merge in `HANDOFF-2026-11-08-to-2026-11-19.md`, with the Open items closed by 11-21 after them, and
the releases' and branches' day-by-day history with the reference sections 0, 2, 3 and 5 in
`HANDOFF-branches-and-reference-until-2026-11-11.md`. It is history: do not read it to start a
task. Open it only when a line here points there, or before an optimisation, to see whether it was
tried and not kept. `notes/INDEX.md` says which note settles what.

Keep this file short. Every session loads it whole and sends it again with every request it makes,
so its size is paid for on every turn, and `publish_check` refuses a file over 200 KB. When the
dated entries grow long, move the older ones the same way, into a new archive file: on 11-01 the
entries before 10-30 went, and the file fell from 156 KB to 69 KB; on 11-04 those before 11-01,
from 100 KB to 77 KB; on 11-11 those before 11-05 and every struck open item, from 138 KB to 91 KB,
and later that day the releases' and branches' history and the reference sections the brief and
`docs/` repeat, from 96 KB to 87 KB, and then the entries of 11-05 and 11-06, from 87 KB to 44 KB;
on 11-21 those of 11-08 to 11-19 and every struck open item, from 151 KB to 50 KB. A closed
open item is struck through where it stands, and goes to the archive at the next such move; what it
still leaves open becomes a line of its own.

---

## Where things stand (2026-11-05)

- **It works on both systems of this laptop** (Arc 140V): Linux with Mesa, Windows with Intel's
  driver. A Vulkan layer hands each presented frame to a daemon that runs NVIDIA's network on the
  GPU and writes the answer back into the swapchain. Since 11-22 the daemon's passes around the
  network run on the GPU by default on Linux too, as the Windows release has them (below, 11-22,
  morning). DirectX 8-11 games go through DXVK. DirectX 12
  runs on Linux through VKD3D-Proton, and on Windows through the game's own XeSS (since 0.0.2).
- **The releases are Windows-only so far**: v0.0.1 (`7f9c364`), v0.0.1.1 (`f9ec5f1`), v0.0.1.2
  (`2f90064`, 10-15), v0.0.1.3 (`d89563f`, 10-18), v0.0.2 (`81b0a59`, 10-31), v0.0.3
  (`86a5799`, 11-01), v0.0.3.1 (`c0b7f9d`, 11-01), v0.0.4 (`8232603`, 11-05), v0.0.5 (`bb9317b`,
  11-06), v0.0.6 (`c473048`, 11-12) and **v0.0.7** (`921d975`, 11-21, the latest), a ZIP with the
  setup window, DXVK 3.1.1,
  both layers, a `vulkan-1.dll` proxy, NR's `libxess.dll` as `nr_xess.dll`, since 0.0.4 its own
  Python, and since 0.0.6 NR's `amd_fidelityfx_dx12.dll` as `nr_ffx.dll`.
  0.0.4 brings that Python and the staged GEMM's walk, which takes 9-19 % off the network on
  Intel's Windows driver (archive, 11-05 and 11-04 night). 0.0.5 brings the `hogwarts`
  profile, Hogwarts Legacy's own tuning, the panel's frame counter and OptiScaler's XeSS in setup
  (archive, 11-06, morning). 0.0.6 brings the network's weights as Intel's Windows driver reads
  them fastest (the E4M3 bytes and the weights by column), the GPU path's frame 9 % shorter, FSR
  3.1 without OptiScaler and the GPU report's network (archive, 11-12). 0.0.7 brings the DirectX 12
  speed-up ("Faster, experimental", the edit cache), a frame 8-12 % shorter at 1080p and 1440p,
  the panel's frame graphs, the A-series kernel switch and the 8-bit outputs (below, 11-21,
  later). What 0.0.2 to 0.0.3.1
  brought is in the archive (10-31, 11-01) and on the release pages. The last seven went out on
  the owner's word before Linux's `make test` at their heads, as 0.0.1.3 did, and Linux has run
  all seven green since: 0.0.2's at `eb9b281`, 0.0.3's and 0.0.3.1's at `d28e841` (archive, 11-04
  later), 0.0.4's and 0.0.5's at `127f6dc` (archive, 11-11), 0.0.6's and 0.0.7's at `d5bbde8`,
  0.0.7's code with `8c82133` on it (below, 11-21, evening). Linux users build from source
  (README).
- **Branches**, on GitHub since 10-30: `master` (the default), `newnr` and `alchemist` were level
  from 11-11 and moved together, each push a fast-forward of all three. From 11-18 `alchemist`
  ran ahead with the A-series switch while Paimonshen finished PR #45; on 11-19 #45 went into
  `newnr` and `master` with our fixes, and `alchemist` was merged with them, so the three are
  level again (archive, 11-19). On 11-20 `alchemist` went ahead alone with the 8-bit outputs of the
  DirectX 12 proxies (below, 11-20), and on 11-21 the three were level again at 0.0.7's head
  (below, 11-21, later). That evening `newnr` took the fused kernels' layout per driver
  (`8c82133`) and Linux's entries, and `master` and `alchemist` followed it, each at the owner's
  word: the three are level again (below, 11-21, evening, later). On 11-22 `newnr` went ahead alone
  with the RTX 5060 Ti machine's notes and the intensity texts (`0169ad9`), the owner having
  asked on 11-21 not to fast-forward the other two for now, and later that day with the E4M3
  rounding by a magic constant on Mesa and the composition's half rounding (below, 11-22, night). `media` holds the README's pictures; `improve-int8` is kept for
  INT8 experiments; `cleanup` (PR #39, merged 11-11) stays.
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
- **NVIDIA's own output, since 11-21** (`notes/phase74-vendor-parity.md`): the owner's RTX 5060 Ti
  runs NVIDIA's DLL over SSH, without a game, on frames of our choosing. Our A and B agree with
  it at 0.989-0.997 correlation of the change, the residual 6-13 % of it, on 18 frames of three
  games, Hogwarts Legacy's among them. Inside the network (`notes/phase75-inside-the-network.md`),
  each of our encoder blocks on the vendor's own input lands within 1-3.4 % of the vendor's
  output, and the residual is those differences compounded. The knobs and the history agree as
  well, but the DLL clamps `Intensity` at 1. The two DLLs are there privately and go nowhere
  else.
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
- ~~The replies to #47-#50 (below, 11-21, evening, later), drafted in the Linux checkout's
  `work/pr-replies/issue47.md` to `issue50.md`: the owner's word on posting them~~: posted at the
  owner's word, 11-21, #49's with network B on a newer DLL added (below).
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
- ~~Preset B against the vendor's own B: a capture on the RTX 5060 Ti with
  `DLSSNR.Hint.Render.Preset` set to B, when the owner opens that machine. Archive, 10-17 later.~~:
  done, preset 2 there; ours agrees at 0.992-0.997 correlation of the change
  (`notes/phase74-vendor-parity.md`; below, 11-21, night).
- The two files that came with the 310.9.16 DLL (`nvngx_dlssnr_compat-fp16-310.9.16-cuda13.4.3.rar`
  and `renodx-dlss.addon64`, in the Windows Downloads). The owner says, as their source claims,
  that the first is the DLL patched for RTX 30 and 40 cards, with FP16 kernels, and the second
  lets RenoDX pick preset A or B. Untrusted and unopened; they wait until one is needed.
- From the `cleanup` branch (PR #39, merged 11-11), proposed and not done: INT8 out of the main
  line (it is in libxmx, the shader lists and the release's file lists) and one build system
  instead of three; the Configure/Restore Steam buttons are a line under Windows. Archive, 11-05.
- The working rules after #39: whether the second memory mode runs as `XMX_STAGING=1 make
  test-gpu`, and CTest on Linux only when CMake changes. Archive, 11-05.

**Linux**
- libxmx tries a device that lacks the float16 8x16x16 shape (`shape_unoffered`, every Arc
  A-series card) without checking that it computes it, and on Mesa an A750 gives 127 of 128 values
  wrong (#48, below, 11-21, evening, later): a game there would get a wrong picture without a word.
  A start-up check of the shape, as `xmxres` checks the E4M3 decode, and a refusal where it fails;
  the owner's word.
- ~~Owed on Mesa from the weights by column, `e4m3-weights` and `fused-bycol`: their timings~~:
  done (below, 11-16, night). The wide weights by column and the E4M3 bytes are exact on Mesa and
  gain nothing there; they stay off, as they are by default. The by-column fused kernels cost it
  7-10 %, the next line.
- ~~**`fused-bycol` costs Mesa 7-10 % of B's network**: a specialisation constant, by column on
  Intel's Windows driver, by row elsewhere~~: constant 4, `8c82133` (below, 11-16, late night).
- ~~`a6699b6`, the interface restore with the change metric off: the owner's word on pushing it~~:
  pushed at the owner's word, 11-16, with `master`, `newnr` and `alchemist` at `f9ad754`.
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
- ~~`8c82133`, the fused kernels' layout as specialisation constant 4, on for this driver (below,
  11-16, late night): the MSVC build, CTest, and B's `frame_profile` at 576x1024 against
  `XMX_FUSED_BY_COLUMN=0`. On GitHub, on all three branches, since 11-21~~: done 11-22, the 4 %
  holds (below, 11-22, midday).
- Arc A-series: the experimental switch for the tiled GEMM kernel (`XMX_PREFER_TILED`, on all
  three branches since 11-19, archive, 11-18), in the setup window and the panel, the same bytes. Off by default;
  A-series owners' GPU reports and frame rates with it off and on decide whether it becomes their
  default. Not run on an A-series card: there is none here. In 0.0.7; the request to them
  (`work\pr-replies\arc-a-tiled-call.md`) went into the 13 open A-series issues on 11-21: their
  answers are what is waited for. The first two, #47 and #50 (two A770s on 101.9175, below, 11-21,
  evening, later), read it 1.5x slower for B and for A; the draft replies say to leave it off.
- #49's DLL, 310.8.2.0 (a B580 in Tomb Raider 2013, below, 11-21, evening, later): not in
  `TESTED_DLLS`, and its network not compared with 310.8.0.0's; the draft reply asks where it came
  from. And Tomb Raider (2013)'s row in the README's tested games at the next release (the row is
  in that entry).
- NR through OptiScaler (#38): runs in Hogwarts with OptiScaler 0.9.4, from the game's XeSS and
  from its FSR 3; setup takes OptiScaler's XeSS (`d936d63`), in 0.0.5 (archive, 11-05 evening).
  Untested: DLSS input through OptiScaler's spoofing, its DirectX 11 path, a game with no XeSS of
  its own, the cut beside OptiScaler's own D3D12 hooks, and #38's Stellar Blade (cause not known).
  ~~An FSR 3.1 proxy without OptiScaler: designed, not built (archive, 11-05 later)~~: built
  (`0195200`) and run in Hogwarts (archive, 11-12, late evening).
- FSR 3.1 without OptiScaler: a game with FSR 3.1 and no XeSS (Stellar Blade's is the case #38
  asked about), FidelityFX SDK 2.0's loader, FSR 3's own frame generation; and whether the setup
  window should offer the choice of upscaler for a game with both. Below, 11-12, late evening.
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
- ~~Where an A770's ~55 us a pass goes: a per-pass profile from an A-series tester
  (`frame_profile.py --warm`, timestamps per pass) would say whether passes or the gaps between
  them. It is half of a frame at 1280x720 there. Archive, 11-03.~~: into the passes; B's 179 add
  up to its frame on two A770s (#47 and #50, below, 11-21, evening, later).
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
  out as 2.0's. Archive, 10-30 evening. #46 (below, 11-20): Cyberpunk 2077's XeSS 1.3.0.28 on an
  A750 never reached the proxy's HELLO, cause unknown until its reporter sends `nr-xess.log`; an
  upscaler's 8-bit output is taken since then, and no game with one has run.
- How the proxy reads a 10-bit output, R10G10B10A2 as linear light (since `60a7312`), and an 8-bit
  one, display-encoded even where FSR 3.1 was told its colour is linear: neither is confirmed by a
  game. Below, 11-20.
- MSVC: the composition is not vectorised (0.5-1.8 ms behind gcc): MSVC vectorises innermost
  loops only, and inside each pixel loop is a three-channel one (`/Qvec-report:2`, reasons 1106
  and 1303). ~~Three C4068 warnings~~: `NR_IVDEP`, `1c49d04` (archive, 11-08). Archive, 10-02, 10-07.
- The bottleneck's weights as E4M3 bytes (archive, 11-08, evening), in `newnr` and on in Mortal
  Kombat 11's runs (archive, 11-11, morning): whether it stays on by default for every GPU on
  Intel's Windows driver; desktop Arc cards and the B580 have not run it, nor the weights by
  column, on by default there too (archive, 11-12).
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

**The RTX 5060 Ti machine** (`notes/phase74-vendor-parity.md`; below, 11-21, night and later)
- ~~It sleeps when idle, and has slept since 16:11 its time on 11-21, until someone at home wakes
  it~~: KDE's PowerDevil, its default on AC; off since 11-21 17:23 its time, and 20 minutes
  without input did not put it to sleep; the setting held through the update to Plasma 6.8.3
  (below, 11-21, night, later).
- ~~The owner's: the full update, `pacman -Sy archlinux-keyring`, then `-Syu xorg-server-xvfb`,
  with `dkms status` checked before the reboot. Then `run.sh` on a private Xvfb display in place
  of the owner's session's Xwayland `:1`, and one frame run again on the new driver against
  610.57.04's bytes~~: done on 11-21, nvidia-open 620.73.04; `run.sh` on Xvfb `:99`; all 19
  frames again, the 38 outputs byte for byte those of 610.57.04 (below, 11-21, night, later).
- ~~Waiting for it: Hogwarts Legacy's six frames without the interface and three with its HUD
  (`work/vendor/frames/set_hog.txt`), DoA5 at 1920x1080 and a desktop capture (their vendor
  outputs made, not fetched)~~: done, 0.991-0.995 on Hogwarts (below, 11-21, night, later).
- ~~Proposed, the owner's word: history (frames in sequence, and the Hogwarts recordings' motion
  and depth), the knobs away from their defaults, a per-pass gate wrapping
  `vkCmdCuLaunchKernelNVX`~~: done at the owner's word, 11-21 (below, 11-21, late night):
  history without motion and with a pan's motion vectors, the knobs, and the per-pass gate for
  network A. Left of them: history with depth and a game's own motion (the Hogwarts
  recordings), network B's taps, the blocks after the encoder one by one.
- Whether `nr_vendor.c`, `run.sh`, `run_set.sh`, `compare.py` and the other tools in
  `work/vendor/` go into the repository: later (the owner, 11-21). So is `docs/ARCHITECTURE.md`'s
  line that preset B was not compared with NVIDIA's.
- ~~**The DLL clamps `Intensity` at 1** (`phase74`): at 1.5 its output is its default's. Ours
  extrapolates above 1, and the panel offers it (#49's player ran 1.25). Whether to say so where
  players read, or to cap it: the owner's call.~~ Say so, the owner, 11-21: the intensity hint
  in four languages (`dist-tools/windows-wizard.ps1`, and the panel's tables from it), the INI's
  comment, `nr_knobs.py` and `docs/SETTINGS.md` say NVIDIA stops at 1; the range stays 0-2.
  On GitHub since 11-22, at the owner's word (below, 11-21, late night).
- ~~`nr_frame.py --help` says the vendor's default local structure is 1.5. The DLL's default is 1,
  which is our `standard` (`phase74`). A one-line fix of the help, at the owner's word.~~ Fixed,
  with `control_atlas.py`'s two headings (structure 1.5, skin 2.0): the skin strength unset
  follows the structure. On GitHub since 11-22.
- `docs/ARCHITECTURE.md`'s controls section says the vendor extrapolates above an intensity of
  1 when a grade or a mask runs its pass (`phase70`). Measured, it does not with style 1's grade
  nor with the automatic mask (`phase74`); a control mask is untested. With the file's other
  line, later.
- The render scale's texts for players (`nr_knobs.py` and `docs/SETTINGS.md`, `Hint.render_scale`
  in four languages) say a lower scale draws coarser detail. It draws a different picture: at
  0.5, 55-60 % of NVIDIA's full-size change goes unreproduced, against 11 % at 1.0, and NVIDIA's
  own network does the same on a smaller frame (`phase76`; below, 11-22, later). The owner's word
  on whether to say so, and how.

**Planned, not started** (parked by the owner: do not start unasked)
- A FAQ in the README. Later.
- INT8 on the bottleneck: the exact kernel exists (`gemm_staged_int8.comp`) and stays out of the
  graph by the owner's decision; `improve-int8` keeps the branch. Archive, 09-26. Measured
  again on 11-11 (the evening's entry): most of its speed is its weights by column, which the half
  kernel has exactly since 11-12 (that day's entry).
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
- ~~`fused-bycol` (archive, 11-15): the 32-channel blocks' fused kernels with their B fragments
  column-major in shared memory, 4 % of a frame at 1920x1080 and 2560x1440. On GitHub since 11-16
  at the owner's word, before Linux timed it (the shaders are the same on Mesa, where by-column
  tiles once spilt: if it is slower there, a specialisation constant can keep Mesa by row)~~: in
  0.0.7; 7-10 % slower on Mesa, which keeps them by row since `8c82133` (below, 11-16, night and
  late night).

---

## What a desktop card's frame holds besides the network; 0.9 ms of CPU and waits off it, which a GPU-bound game does not see (2026-11-26, branch `daemon-cuts`)

The owner, 11-25, on #51 (a B580 in the Bloodborne PC recompile, native Vulkan: network B at
896x512 in 9 ms, 21 ms a frame in the daemon): the speed-ups dropped for a small gain on this
iGPU may matter on desktop cards; study it. Then, each at the owner's word: measure, cut the
daemon, then the layer, then commit. Arc 140V, Intel 101.9150.

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
- **In a game it does not show** (Mortal Kombat 11 at the owner's word, release roots built the
  same way from `newnr` and from this branch, `nr-game\pacing-game-test.ps1`, B, GPU path,
  1280x720 at 0.4, 40 s a run, alternating):
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
  time, so a held one would lock out a second client. The owner's call.
- **Left for desktop cards:** the image passes, ~2.9 ms of a B580's frame, mostly fixed costs.

**Not run:** a B580, Linux, CTest on Windows. **Next on Linux:** `make test` in both memory modes
and CTest: `nr_daemon.py`, `gpu_image_stats.py` and `gpu_image_pipeline.py` changed (the layer's
change is Windows-only). **Next on Windows:** the owner's word on the branch.

## The GPU path's slow state at 1280x720 on Windows is the driver's own clock choice (2026-11-22, afternoon, branch `newnr`)

`gpu_frame_stages.py`, B at 1280x720 and 0.4, Intel 101.9150, quiet, the GPU checked free per
process (`Get-Counter '\GPU Engine(*)\Utilization Percentage'`). In some periods the GPU image path
and the device path take 15-35 ms a frame and the network alone 15-31 ms, against 14-15 and 12.3;
the host path in the same process always runs the network in 12.3-12.8. Logs:
`work/gpu-path-states-1122`, the probe `probe_states.py` beside them.
- **The cause so far: the GPU's clock.** Level Zero sysman (`work/tools-win/gpu_clock.py`) reads
  700-1400 MHz in those phases, the request equal to the actual and no throttle reason; 1950 in the
  host path and in fast periods. The driver's governor chooses it; no power or thermal limit.
- **Ruled out:** constant 5 (seen with 0 and 1); the clock poll itself (with and without, alternating);
  the process's power class (default, high QoS, EcoQoS through `SetProcessInformation`); an idle CPU
  (a busy core in another process beside it changes nothing); the wait (every wait is a blocking
  `vkWaitForFences`). Another GPU user doubles every mode, the host path too: the XeSS session's
  replay did at 12:47-12:50, and those runs are left out.
- **Periods, not processes:** none in 20 processes at 12:44-12:47 and 15:32-15:35; nearly all of 35 at
  15:35-15:45.
- **A game does not see it** (at the owner's word, 17:06-17:15): Hogwarts Legacy from the 0.0.7
  release through the FSR 3.1 proxy (the game is set to FSR 3 Quality with XeFG x4; its settings
  left as they were), B at 768x448 for 1920x1080 at 0.4. Over 9 minutes, 8736 frames from the
  daemon's log: the network 25.0 ms median, 27 at the 90th percentile, 30 at most, never doubled;
  the GPU at 1900-1950 MHz with the request at 4250, as in the host path. The bench right after it
  was low again in its GPU-only modes (1360-1830 MHz, request 1400-2700; the host path 1940, 4232).
  So the governor holds the clock up under a game's load, and the slow state belongs to the bench's
  GPU-only modes: time those with the clock beside them, within one state. Nothing to change in
  NR for it. What sets the request (4250 against ~1400) is not known; forcing the clock stays
  refused (owner, 11-15). Harness: `nr-game\hogwarts-fsr-test.ps1` and `clock_log.py` (beside the
  logs in `nr-game\release-tests\hogwarts-clock-fsr-1122`); the daemon logs to the release root's
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
  before and after). Logs and `results.json` in `work/e4m3magic-win-1122` and `-1122b`.

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
- **The daemon's own paths** (`gpu_frame_stages.py`, its three modes, 5 rounds alternating, B at
  1280x720 8 more; `work/e4m3magic-win-1122c`, `-d`): the outputs the same bytes with 0 and 1 in
  every mode and round. Where a process holds its speed: the host path's network 1.5 % faster, A
  and B at 1280x720 and 0.4; B at 1920x1080 and 0.4 1.8 % faster on the GPU image path (32.18 ->
  31.59 ms, 5 of 5 pairs), its network 1.9 % on the frames that never leave the GPU (24.32 ->
  23.85, 5 of 5). At 1280x720 the two GPU paths change speed state from process to process (B's
  frames 14-35 ms, the network alone 12.4-29.5, while the host path in the same process holds
  12.5-12.9): the constant cannot be seen there; in the fast state both read 14.3-14.9 ms.
- **The checks owed from 11-21:** `8c82133` again, by column against `XMX_FUSED_BY_COLUMN=0`, 5
  rounds alternating (`work/fusedcol-win-1122`): B 576x1024 39.97 against 41.65 ms, A 78.66
  against 81.48, 5 of 5 each, the bands apart; the window blocks 8 % and the feed-forwards 15-17 %
  slower by row: the 4 % holds. The setup window's self-test after `0169ad9`: 105 controls, 217
  keys; and its pacing block (`Merge-PacingFromProfile`, `6c61d486`, 10-23) was a bare `{ }`,
  which PowerShell prints instead of running, so its four checks never ran. Now `& {`
  (`dist-tools/windows-wizard.ps1`): it passes, and fails as it should with the function broken.
  Local `master` and `alchemist` fast-forwarded to `b315822`.

**Verdict, as asked:** stably faster by the median, never slower, the same bits: the constant goes
on for Intel's Windows driver too. **Done on Windows at the owner's word** (11-22, afternoon):
`libxmx.c` turns it on for `VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS` as for Mesa, and the comments
in `publish.glsl`, `e4m3_probe.py` and notes/CLAUDE.md say so. Checked: CMake and MSVC with no
warning, CTest 72 of 72; the default reads on here and `XMX_E4M3_MAGIC=0` turns it off; the heads
with the default are Linux's. **Next on Linux:** fetch and fast-forward; nothing changes on Mesa.
**Next on Windows:** nothing for this; the next release carries it.

## The GPU image path on by default on Linux, and exact on Mesa (2026-11-22, morning, branch `newnr`)

The owner, 11-22, morning, on the night's entry: so only the CPU path has run on Linux all along?
We need the GPU path. Commit to `newnr` and push.

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

The owner, 11-22, going to sleep until about 11:00: speed the network up at full size, after
NVIDIA's own structure (activations in E4M3 and the rest); experiments and research all night,
nothing committed or pushed. Mesa 26.2.7, `frame_profile.py --warm 25 --runs 5`, every pairing
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

**The image path on Linux, the owner's call:** a game on Linux has the daemon's host passes on
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

**Committed and pushed at the owner's word** (11-22, morning), `newnr` alone: constant 5
(`75b4626`: `publish.glsl`, `libxmx.c`, `xmxres.py`, `nr_daemon.py`, and `e4m3_probe.py`
comparing against the constant off, since on Mesa `publish.glsl`'s own `e4m3` is the magic one
now; 0 mismatches again over the same inputs), the composition's half rounding (`fff7ea5`), the
two benchmarks' trailer (`f63c41b`) and these notes. Checked at that state: `make test` 74 of 74 in both memory modes, CMake with no warning, CTest
73 of 73; the heads with the constant 0 and 1 the same at 512x320, 576x1024, 1920x1088 and
2560x1472.

**The owner, 11-22, morning:** the GPU image path goes on for Linux, with the division that makes
it exact on Mesa: done (above, 11-22, morning). **Next on Windows:** once this is on GitHub, the
MSVC build and CTest, and `XMX_E4M3_MAGIC=1` timed against the default on Intel's driver
(`frame_profile.py --size 576 1024` and `--size 320 512`, B and A, rounds alternating): October's
10-22 % loss (10-13) came before November's changes on that driver, the staged walk, the E4M3 and
by-column weights and the fused kernels by column; if it gains now, the constant goes on for that
driver too.

## Render scale against NVIDIA's full picture: NVIDIA has no smaller network, and a smaller frame is a different picture (2026-11-22, later, branch `newnr`)

The owner, 11-22, after the half accumulator: weigh the render scales players use against
NVIDIA's full picture, and NVIDIA's own `DLSSNR.ScalingRatio` below 1. The note is
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

**Waiting for the owner:** the render scale's texts say a lower scale draws "coarser detail, at
about the same strength" (`nr_knobs.py`, and from it `docs/SETTINGS.md`) or is "cheaper, with
coarser detail" (`Hint.render_scale` in four languages, `windows-wizard.ps1`, and the panel's
tables from it). It draws a different picture: whether to say so, and how.

**Next on Linux:** nothing owed. **Next on Windows:** nothing.

## The half accumulator timed again on Mesa: about 1 % of a frame, and no closer to NVIDIA (2026-11-22, branch `newnr`)

The owner, 11-22, on what the RTX 5060 Ti machine can do for speed: start with the FP16
accumulator, NVIDIA's own arithmetic. It was last timed in a frame at phase 31, behind the ANV
store bug that phase 38 found was not there.

- **The build.** `gemm_staged.comp` takes `-DACC16` as `gemm_resident.comp` does: a `float16_t`
  accumulator, widened to float32 at each store, direct or through the stage (a matrix is stored
  only into an array of its own component type, phase 38). Off by default, and the default
  SPIR-V of the three staged builds was byte for byte as before. Dropped at the owner's word
  (11-22): the change is `work/acc16/gemm_staged-acc16.patch`, the builds beside it.
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
own `DLSSNR.ScalingRatio` below 1: done at the owner's word (above, 11-22, later). **Next on
Windows:** nothing.

## Inside NVIDIA's network, its knobs and its history, against ours (2026-11-21, late night, branch `newnr`)

The owner, 11-21, on the proposals left for the RTX 5060 Ti machine: the per-pass gate the
exact way, history, and the knobs; the tools into the repository and `docs/ARCHITECTURE.md`
later. Network A was traced and tapped; B was traced only. The numbers are in `phase75` and
`phase74`. Pushed at the owner's word on 11-22, `newnr` alone: `master` and `alchemist` stay
at `b315822`, the owner having asked on 11-21 not to fast-forward them for now.

- **The per-pass gate** (`phase75`):
  - `nr_vendor.c` now wraps the DLL's Vulkan through its own `vkGetInstanceProcAddr` and
    `vkGetDeviceProcAddr`, traces every launch (`NRV_VKTRACE`) and copies spans of its scratch
    after chosen launches (`NRV_TAPS`). Taps leave the output byte for byte as it was.
  - A's 156 launches are skchen17's 156 slots. Its activations are E4M3 in two layouts, now
    decoded.
  - The feature assembly and block 0 are the same as ours to 0.9998. Each of our 22 encoder
    blocks, on the vendor's own input, gives its output to 0.9994-0.99995 (1-3.4 % RMS). The head
    and the composition, on its tensors, give its output within 0.08 of a level.
  - Run freely, the blocks' small differences compound to 17 % by level 4 and 10.5 % at the
    decoder's end: that is the 6-13 % of `phase74`.
  - FP16 accumulation, emulated, does not account for a block's difference.
- **The knobs** (`phase74`): all as ours at 0.974-0.998, but two:
  - the DLL clamps `Intensity` at 1;
  - its skin strength needs `UseAutoMask`.

  The automatic mask's lanes are the same scalars as ours. Its effect, about one level, is too
  small to correlate past the residual.
- **History** (`phase74`):
  - Eight consecutive DoA5 frames without motion: the two sequences grow closer frame by frame
    (A 0.989 to 0.995, B 0.9925 to 0.996), the history's effect the same size on both sides.
  - A pan with motion vectors: the DLL reads `MVec` as our motion (pixels, to where the pixel
    was, scale 1); the other sign falls to 0.86-0.95.

- **The owner's calls**, 11-21, "yes and yes":
  - The intensity stays 0-2, and the texts players read now say NVIDIA stops at 1. That is
    the hint in four languages, in the setup window's strings and the panel's tables generated
    from them, plus the INI's comment, `nr_knobs.py` and `docs/SETTINGS.md`.
  - `nr_frame.py`'s help and `control_atlas.py` now give the DLL's own defaults.
  - Checked on Linux: `make test` 74 of 74 in both memory modes, CMake with no warning, CTest 73
    of 73. On GitHub since 11-22 (`0169ad9`).

**Next on Linux:** at the owner's word, history with a game's own motion and depth, B's taps.
**Next on Windows:** once this is on GitHub, the setup window's self-test and the MSVC build:
`Hint.intensity` changed in `windows-wizard.ps1` and in the panel's tables.

## The RTX 5060 Ti machine: no more sleep, driver 620.73.04 with the same bytes; Hogwarts Legacy, DoA5 at 1920x1080 and a desktop against NVIDIA's (2026-11-21, night, later, branch `newnr`)

The owner, 11-21: check and set the machine's sleep yourself; it is on Wi-Fi, so it cannot be
woken over the network. Someone at home woke it at 17:16 its time (21:16 here).

- **What put it to sleep: KDE's PowerDevil.** logind, 16:11:39: `suspend requested from client
  PID 926 ('org_kde_powerde')`. `~/.config/powerdevilrc` did not exist, so its default held: on
  AC, sleep after 15 minutes without input, and SSH and the network are not input. logind's own
  `IdleAction` is `ignore`.
- **Set**, user-level: `[AC][SuspendAndShutdown] AutoSuspendAction=0`, "do nothing", with
  `kwriteconfig6` (the names checked in its `libpowerdevilcore.so` 6.7.4), then
  `reparseConfiguration` and `refreshStatus` over the session bus. The screen still goes off when
  idle.
- **Checked:** with no inhibitor, and KDE's idle timers reset (`SimulateUserActivity`) at
  17:32:15, the screen went off at about 17:43 (its DisplayPort audio sink vanished then) and was
  still off at 17:53, so nothing touched the machine; it stayed up with no suspend request in
  logind's journal or the kernel's. The old default would have asked at 17:47.
- **Holding it awake from SSH:** `systemd-inhibit --mode=block` is refused there (polkit: a
  Tailscale SSH session is remote, and `inhibit-block-sleep` has `allow_any=no`), which is why
  the wake watcher's six-hour block never took. `kde-inhibit --power` under `systemd-run --user`
  is honoured (PowerDevil's `HasInhibit` true after ~5 s); it ran while the frames did, and was
  stopped for the check.
- **Parity** (`phase74`, its table): Hogwarts' six frames without the interface, A 0.991-0.994
  and B 0.991-0.992, the residual 11-13 % of the change; its three with the HUD 0.993-0.995,
  9-10 %; DoA5 at 1920x1080 0.995 and 0.996, 8-9 %; a desktop capture 0.985, 13-16 % of a change
  of 3.5-5 levels. Over the 18 frames of games, 0.989-0.997. The vendor's 18 Hogwarts runs, a
  process each through `work/vendor/run_set.sh`, took about 0.5 s apiece, the evaluation A
  5.4-5.8 ms and B 3.3-3.9 at 1280x720.
- **The owner's update**, from the owner's session (18:02-18:08 its time, the reboot at 18:13):
  kernel 7.3.5, nvidia-open-dkms 620.73.04 (DKMS built it for 7.3.5 and the initramfs was made
  before the reboot), wine-staging 11.22, Plasma and PowerDevil 6.8.3, xorg-server-xvfb. The
  machine came back by itself with the session (SDDM's autologin), no unit failed, and the sleep
  setting held. After the reboot there were three X displays where there had been two, so the
  session's number is not fixed: `run.sh` now starts its own Xvfb, `:99`, as a user unit, and
  `NRV_XDISPLAY=session` keeps the old way. **All 19 frames run again with A and B: the 38
  outputs are byte for byte those of 610.57.04.**

**Next on Linux:** the proposals under Open items, The RTX 5060 Ti machine, each at the owner's
word. **Next on Windows:** nothing for this.

## NVIDIA's own NR on the RTX 5060 Ti: our A and B against the vendor's, the first parity figures (2026-11-21, night, branch `newnr`)

The owner, 11-21: the RTX 5060 Ti machine is on, reachable over SSH; do there what is needed,
copy both DLLs and start. It had no game with DLSS 5, and none is needed. What was done, what the
DLL said and the figures are in `notes/phase74-vendor-parity.md`; here, the session.

- Both DLLs copied there over Tailscale at the owner's word (310.8.0.0 and 310.9.16.0, SHA-256
  checked at both ends against `TESTED_DLLS`, mode 0444). The owner installed wine-staging 11.15
  and mingw-w64-gcc 16.1 from their own session (`sudo` needs them); DXVK 3.1.1 and dxvk-nvapi
  0.9.3 came from their GitHub releases, digests checked.
- `work/vendor/nr_vendor.c` (C, built there with mingw) drives the DLL's own Vulkan exports in
  the NGX core's place, after `fernandaspets/dlss5-nr-recipe` (MIT). The walls on the way, each
  named in the note: Wine's desktop needs an X display (Xwayland's `:1` of the owner's Plasma
  session), the DLL needs NvAPI to see its GPU, and the parameter block is MSVC's layout.
- 22 vendor runs once the walls were down, every one created and evaluated with Success: presets 1
  (A) and 2 (B), 1280x720 to 1920x1080, 3.4-9.8 ms a frame.
- Parity on DoA5 (five frames of one screen) and Mortal Kombat 1 (three): A 0.989-0.997
  correlation of the change, B 0.992-0.997, the residual 6-13 % of the vendor's change, 43.6-49.6 dB
  between the two outputs; the other network as a control at 0.86-0.95. The brief's hard
  constraint 2 and its weight-format line now say so.
- At 16:11 its time the machine went to sleep, idle as far as it knows, with a download of DoA5's
  1920x1080 output half done; it waits for someone at home (Open items).

**Next on Linux:** when the machine wakes, the items under Open items, The RTX 5060 Ti machine.
**Next on Windows:** nothing for this.

## Issues #47-#50: the A-series switch slower on two A770s, Mesa wrong on an A750, a B580 in Tomb Raider with DLL 310.8.2.0 (2026-11-21, evening, later, branch `newnr`)

The owner, 11-21: check GitHub's new issues, then draft the replies and write this down. Four new
issues, all of 11-21; none of the 13 A-series issues asked on 11-21 has answered yet, nor #46's
reporter, and no pull request is open. The drafts, for the owner to post, are in the Linux
checkout's `work/pr-replies/`, `issue47.md` to `issue50.md`.

**Posted** at the owner's word, 11-21, as drafted (issuecomment-7060437688, -7060437928,
-7060438192 and -7060438412 for #47 to #50), #49's with a paragraph the owner asked for: try
network B, which needs a DLL of 310.9.16 or later (Hogwarts Legacy since its DLSS 5 update, or
NBA 2K27); setup picks B when the DLL has it, and the panel switches A and B in play. **`newnr`
pushed** at the owner's word the same evening (`c0c17d2`), and then, at the owner's word too,
`master` and `alchemist` fast-forwarded to it with this line: the three level.

**#47 and #50, two A770 16GB, Windows, Intel 101.9175** (0.0.7's GPU report). NR's 8x16x16 is
exact at 8, 16 and 32 lanes, though not offered, as on the eleven A-series cards before. The network
at 1280x720 and 0.4, the default staged kernel with the E4M3 bytes and the weights by column,
against the switch (`XMX_PREFER_TILED=1`, half and by row):

| ms | #47 | #50 |
| --- | --- | --- |
| B, default / switch | 11.9 / 18.2 | 10.8 / 16.4 |
| A, default / switch | 23.5 / 36.8 | 21.3 / 32.7 |

- **The switch is 1.5x slower on both.** OUCO86's measurement behind it (10-10) predates the staged
  walk order, the E4M3 bytes and the weights by column, and those reached A-series cards too: at
  that size an A770 took ~19 ms on B and ~35 on A on 11-03 (`notes/players-20261103.md`).
- **An A770's ~55 us a pass is in the passes.** B's 179 passes add up to 11.7 of #47's 11.9 ms and
  10.5 of #50's 10.8 (the profile and the timing are separate runs), so the gaps between them come
  to 1-2 us each. The staged GEMM is 126 of them,
  58 % of the frame, 49-55 us each; the six window blocks take 292-334 us each and the six fused
  feed-forwards 249-274.
- **NR's shape runs 1.4-1.7x faster at 8 or 16 lanes than at 32 on an A770**: 44-58 TFLOP/s
  against 31-35, and 8x8x16 the same way (46-54 against 27-33). Every pipeline is built for 32
  lanes (`build_pipeline_spec`); the local branch `subgroup-width` (`work/sg16`,
  `XMX_SUBGROUP_WIDTH`) is where a narrower build for A-series cards would start. Not started.
- #50's author (generalninja, #29) offers test builds again.

**#48, an A750 on Linux, Mesa 26.2.5** (0.0.7 built from source; `coopmat_probe` and the GPU
report's network): 8x8x16 exact at 8 to 32 lanes; 8x16x16 wrong, 119 of 128 values at 8 lanes and
127 at 16 and 32. So under Mesa an A-series card cannot run NR's kernels, and libxmx tries it
anyway (`shape_unoffered`) without checking the shape: a game there would get a wrong picture
without a word (Open items, Linux). The report's network timings ran on wrong values; the switch
read slower there too (B 30.6 against 24.6 ms).

**#49, a B580 Founders Edition, Windows, 0.0.7: Tomb Raider (2013)**, DirectX 11 (32-bit) through
DXVK, network A at render scale 0.45, intensity 1.25, profile `cinematic`. The player: 37-40 fps at
"1080p" Ultra, 70-80 with frame generation, with screenshots before and after. Its Save report:
6589 frames on `gpu-shared`, none rejected, no error; frames of 2560x1009, a 1152x512 network in
~21 ms, 24-25 ms a frame in the daemon; the speed test's network A 8 ms at 1280x720 and 0.4 (9 on
#17's B580, `notes/players-20261103.md`) and 15 at 1920x1080. **Its DLL is 310.8.2.0** (165 830 144 B, SHA-256
`6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927`), not in `TESTED_DLLS`; setup
took 649 tensors out of it and the game ran. Whether its network is 310.8.0.0's is not known, and
no report can tell: the logical file's `source_sha256` is the hash of MLX-DLSS's packed file
(`dlssnr-packed.safetensors`), and the Save report carries no hash of the network at all. The
README row for the next release:

`| Tomb Raider (2013) | Arc B580, a player's report ([#49](https://github.com/Uzbekunknown/dlss-nr-on-intel/issues/49)) | Windows, DirectX 11 (32-bit) through DXVK, network A | 37-40 fps at 1080p Ultra and render scale 0.45, 70-80 with frame generation | 2026-11-21 |`

**Next on Linux:** the owner's word on the start-up check of the unoffered shape in libxmx, and
the answers in #47-#50. **Next on Windows:** `git fetch`, fast-forward `master`, `newnr` and
`alchemist` to GitHub's, then the line for `8c82133` under Open items, Windows; the README row at the next release; #49's DLL once its source
is known.

## Checked on Linux: 0.0.7's head, with the fused kernels' layout per driver rebased onto it (2026-11-21, evening, branch `newnr`)

The owner, 11-21: pull the branches from GitHub, then run the tests. GitHub had gone from `f9ad754`
to `ecdce7b`, 0.0.7 and the entries after it, and `master` and `alchemist` fast-forwarded to it.
`newnr` held the three local commits of 11-16, the timings on Mesa, constant 4 and its entry: they
are rebased onto `ecdce7b` as `895d7a6`, `8c82133` (the code, `7944836` before) and `d5bbde8`, and
`backup-20261121-newnr-before-pull` keeps the branch as it was. What met in the rebase is in the
entry of 11-16, late night (below). Nothing pushed. Mesa 26.2.7 (26.2.6 on 11-16), no daemon, swap
empty, a quiet CPU.

- `make test` 74 of 74, in 1 min 30 s;
- `XMX_STAGING=1 make test` 74 of 74, in 1 min 33 s, the seven GPU suites that print the mode
  saying `staging=True`;
- CMake builds with no warning, and CTest passes 73 of 73;
- `XMX_FUSED_BY_COLUMN=1 make test-gpu` 44 of 44: the layout Intel's Windows driver takes, and
  0.0.7's on every driver;
- `frame_replay.py` heads with the constant 0 and 1, both ways those of 11-16: B at 576x1024
  (`edcbf478`, `acbd05d9`) and 320x512 (`df409ac4`, `b6d9870b`), A at 320x512 (`3e37b87c`,
  `29d613d2`). With 1, B's replay at 576x1024 took 52 ms against 48, so the variable takes.
- Skipped inside the scripts, as before: the Windows-only cases, `test_gemm_e4m3.py`'s comparison
  (the E4M3 bytes are off on Mesa) and the GPU image path's parity checks, which hold its bytes to
  the CPU's on Intel's Windows driver alone (`xmxres.image_path_exact`). **So
  `test_image_compose.py`'s strengths case, which the 0.0.7 entry expected to run on Mesa, is
  skipped here** with the other parity checks.

**Next on Linux:** nothing owed. **Next on Windows:** once the owner's word puts `newnr` on GitHub,
`8c82133` (Open items, Windows).

## 0.0.7 built, checked and published: the edit cache, the panel's graphs and fixes, the A-series switch, 8-bit outputs (2026-11-21, later, branch `release-0.0.7`)

The owner, 11-21, after playing the edit-cache build in Hogwarts Legacy (02:25-02:31, NR turned
off and on from the panel 22 times, its screenshot at render scale 0.2 reading NR 22.9 fps; a
Win+Shift+S was saved to the screenshots folder, and its absence from Win+V turned out not to be
NR's): "checked, all is well; we can prepare the release". The test install was taken out (Remove NR clean, the game's
folders as before but for the XeFG capture's own log). GitHub had not moved (`alchemist` at
`8bb67bc`, `master` and `newnr` at `30ec631`), so the local `release-0.0.7` starts at
`edit-cache-1120`'s `2aeed03` and adds `921d975`: VERSION 0.0.7, the README's Hogwarts Legacy
through FSR 3.1 row from 11-20 and a line on the DirectX 12 speed-up, the same in `docs/WINDOWS.md`
and `docs/RELEASE-QUICKSTART.md` with the A-series switch and the 8-bit outputs. Nothing pushed.

**What 0.0.7 brings against 0.0.6:** `dx12-waits` and `fused-bycol` (8 % of a frame at 1920x1080
and 12 % at 2560x1440 on the GPU path, the game's own resets in DirectX 12); `a6699b6`; PR #45's
frame graphs and the network's time on the pipe path; the Arc A-series kernel switch; the DirectX
12 proxies' 8-bit outputs (#46); the edit cache with its fade as a choice; the panel's pointer,
counter and graph fixes and detail and colour strength on the GPU (the entries below).

**Checked on Windows** (Arc 140V, Intel 101.9150):
- MinGW (CMake and Ninja, the layer off): the build, with the 198 warnings, and CTest 72 of 72.
- MSVC: `build_win.bat` and `build_xess.bat` with no warning, and CTest 72 of 72 on its libraries.
- At `24b384a`, the same code: `run_tests.py --only cpu,gpu` 65 of 71, the six Linux-only; the nine
  native fixtures, `test_nr_xess`, `test_xess_panel`, `test_gpu_d3d12.py --network` with the
  strengths' round.
- The release folder from `921d975` (`deploy.bat --release`): 1374 files, against 0.0.6's 1372 only
  `src/gpu/history_sample.glsl` and `src/gpu/test_prefer_tiled.py` more, none fewer; no weights,
  NVIDIA DLL or home path.
- **The ZIP**, `dlss-nr-windows-0.0.7-x64.zip`, 47 451 344 B, SHA-256
  `a0a5a8d3311efd86f6f7622da7712a8ec43feb1c914cc4b3e34f5ecaef8f0537` (on the Windows laptop in
  `%USERPROFILE%\nr-rel\final-0.0.7`). From its unpacked copy (`nr-game\rel007-zipcheck.ps1`,
  `release-tests\zipcheck-0.0.7-summary.txt`): the stand-in (install in 7 s with both networks,
  958 frames, none rejected, the speed test A/71 and B/31, Save report 19 checks, none failing,
  the folder as before) and the window's self-test (105 controls, 217 keys).
- The notes: `work\pr-replies\release-notes-0.0.7.md`, with this commit and SHA-256.

**Not run:** a game from the ZIP (the owner's Hogwarts run was the same code from
`nr-rel\cache-2aeed03`; Mortal Kombat 11 from the ZIP waits for the owner's word, the script takes
`-Game`); Linux; an A-series card; a game with an 8-bit upscaler output.

**Published** at the owner's word ("these lines are not needed; take them out and publish"),
11-21 03:51 +0900: `alchemist` fast-forwarded to `release-0.0.7` in the owner's checkout and
pushed, `master` and `newnr` fast-forwarded to it on GitHub (`8296864`, then this entry), the
lightweight tag `v0.0.7` at `921d975`, and the release "DLSS-NR 0.0.7 for Windows", Latest, with
the ZIP (GitHub's digest `sha256:a0a5a8d3...0537`, the ZIP's own) and the notes without their
draft comment. At the owner's word the notes lost three lines before publishing: the DirectX 12
history following the game's resets, the strengths on the GPU, and the list of fixes.

**Next on Windows:** each at the owner's word, #46's answer (8-bit outputs are in), and the
request to A-series owners (`work\pr-replies\arc-a-tiled-call.md`, "the next build" becoming
0.0.7). **Next on Linux:** `git fetch`, fast-forward `master`, `newnr` and `alchemist` to GitHub's,
then `make test` in both memory modes and CTest at the head: `image_compose.comp`
changed, and `test_image_compose.py`'s strengths case runs on Mesa.

## The edit cache for 0.0.7: merged with `alchemist`, its fade a choice; the panel's pointer, counter and graph; detail and colour strength on the GPU (2026-11-20 and 11-21, branch `edit-cache-1120`)

The owner, 11-20, before 0.0.7: check the edit cache first. `alchemist` (`8bb67bc`) was 52
commits ahead of `edit-cache`, so it is merged into a new local branch `edit-cache-1120`
(`17ab4e7`) in the worktree `D:\NRonWindows\dlss-nr-cache`; `edit-cache` stays at `39f0472`.
Nothing is pushed. Arc 140V, Intel 101.9150, on AC.

**The merge** (`17ab4e7`): five files met: the setup window and its Python core (every key of
both kept), `nr_xess.c` (the sRGB table at `composing[4]`), `nr_xess.hlsl` (COMPOSE with
`answered()`) and this file. New with it: an upscaler's 8-bit display output never takes the
cache, whose MATCH and APPLY work in scene light, so such an execute gets NR on every frame;
`test_nr_xess`'s `run_cache_display` holds it (without the guard the stand-in is asked for 2 of 3
frames and 6300 pixels are not the answer's).

**Hogwarts Legacy, 11-20 20:37-20:40** (release root `%USERPROFILE%\nr-rel\cache-1120\dlss-nr-windows`;
FSR 3.1, 1920x1080, XeFG on, network B at 0.4, standing; real frames from the XeFG capture's
`camera.txt`, 25 s a case; `nr-game\release-tests\hogwarts-cache-1120\summary.txt`):

| | frames a second | frame times, ms |
| --- | --- | --- |
| `cache=0` | 16.0, 16.4 | 61-62, steady |
| `cache=2`, fade on (what the checkbox wrote) | 16.7 | 48-52 / 63-76 in turn |
| `cache=2`, fade off | 18.8 | 44-46 / 58-62 in turn |
| `cache=3`, fade off | 20.4 | 40, 40, ~70 |
| NR off | 29.9 | 33 |

The fade runs MATCH for two edits on every frame, and that took the gain. A carried frame costs
the game's frame + 12 ms, as on 11-15; NR's round trip is 70 ms with the cache, 49-51 without.
How a refresh looks without the fade (offline, 1.6-2.9x NR's own flicker) is for the eye.

**The owner, 11-20:** take it, and give players the choice of the fade; and three bugs seen in
the panel during that run.
- `ce121a9`: the setup window's "Smooth in each new answer of the network (smoother, slower)"
  under the speed-up, four languages, offered with the speed-up for a DirectX 12 game
  (`Profile.nr_cache_fade`, absent = off); `cache_fade` defaults to 0 in `nr_xess.c`. And Remove
  NR after a click: the click rewrites `nr-xess.txt`, and Remove NR, which takes away only what is
  as NR placed it, left the file and the `dlss-nr` folder; the record follows NR's rewrite now.
  `test_windows_wizard` 108 (1 skipped); the window's self-test 105 controls, 217 keys.
- **Win+Shift+S with the panel open left no usable pointer** (`56efde2`). The screenshot's overlay
  deactivates the game with no frame the panel sees, and Unreal turns its raw mouse off and on
  (RIDEV_REMOVE, then its own window), which takes the process's one registration from the
  panel's window. The panel takes it back each frame it holds the pointer, and the clip guard's
  thread says when the game left the front, so the panel takes the pointer afresh.
  `test_raw_mouse`, `test_focus_gate`. **Not run in a game.**
- **The counter said NR 8-9 a second beside a network of 24-25 ms** (`06a66e9`): with `cache=2`
  it counted the answers alone, half the game's rate. The carried frames count now (the network's
  time stays the answers'), and the hint says NR's rate is not one over the network's time, the
  game drawing on the same GPU.
- **The graph lost the network's line**, for two reasons. A network slower than the graph's 100 ms
  ceiling was dropped as if unmeasured, so at render scale 1.0 (166 ms at 1920x1080) every point
  was a gap; it is drawn at the ceiling now, and the ceiling is a second (`06a66e9`,
  `test_menu_panel`). And with detail or colour strength away from 1 the GPU composition refused
  the frame, so in DirectX 12, which has no CPU path, NR went off whole (`24b384a`). The GPU
  composes both strengths now, exactly as the CPU: `compose_detail` in three passes in NumPy's
  float32 order (`test_image_compose`, 30 exact comparisons, 20 against `nr_frame.compose`; OpenCV
  hidden for the case, `f5dc116`, since its blur sums in another order). `test_gpu_d3d12.py
  --network` runs a second round with the strengths away from 1, which fails against the old
  composer as the game did. The two blur passes cost 2.2 ms at 1920x1080 and 4.0 at 2560x1440,
  only while a strength is away from 1. The panel's GPU row no longer says a strength keeps the
  path on the CPU (`gpu_blocked_*` gone, 53 keys), and the extra lookup's names follow the table
  again: since 11-18 every name from `every` on answered a key two places early (nothing shipped
  read them); `test_menu_choices` holds all 42 to their keys.

**Checked on Windows**, at `f5dc116`: `build_win.bat` and `build_xess.bat` with no warning;
`test_nr_xess`, `test_xess_panel`, the nine native fixtures; `run_tests.py --only cpu,gpu` 65 of 71,
the six failing Linux-only (the Makefile's three binaries; Unix socket paths in `test_ui_mask`,
`test_temporal` and `test_present --negative-control`); `test_gpu_d3d12.py --network`;
`test_gpu_present --gpu-only --unsupported meter` and `async` (`settings` leant on the old
refusal and is `meter` now, the daemon's `--meter`).

**Not run:** a game since the merge's run above, so the pointer, the counter, the graph, the
strengths and the fade's checkbox; the window's self-test and `test_windows_setup` after
`ce121a9` (nothing of the window changed since); Linux.

**Next on Windows:** the owner's look in Hogwarts (Win+Shift+S with the panel open, the counter
with the cache on, the strength sliders), then the word on merging `edit-cache-1120` into
`alchemist` for 0.0.7. **Next on Linux:** once it is pushed, `make test` in both memory modes and
CTest: `image_compose.comp` changed and `test_image_compose.py`'s new device case runs on Mesa;
the menu and `src/xess` are Windows-only.

## DirectX 12: an upscaler's 8-bit output taken as a display picture, after issue #46 (2026-11-20, branch `alchemist`)

Issue #46 (11-19): an Arc A750 on 0.0.6 got no frame through NR in two DirectX 12 games. It was
answered at the owner's word (issuecomment-7037721144).
- Assassin's Creed Black Flag Resynced, through OptiScaler's XeSS: the proxy refused every frame,
  "XeSS's output format 28 ... is not handled". `kind_of` took RGBA16F, R11G11B10F and R10G10B10A2
  alone, and the refusal comes before the HELLO, so no daemon was started either.
- Cyberpunk 2077 (XeSS 1.3.0.28): the report's daemon log was last written four hours before the
  launch it records, so the proxy never reached a HELLO. Either XeSS was not the game's upscaler,
  or a refusal came first; the reporter is asked for `nr-xess.log` with XeSS chosen. The panel
  opens either way, from `DllMain`.
- The A750's "lacks the float16 8x16x16 cooperative-matrix shape" lines are every A-series card's.

The owner, 11-20: take 8-bit pictures. **What changed** (`src/xess`):
- `kind_of`: R8G8B8A8 and B8G8R8A8, UNORM, SRGB or TYPELESS, are display pictures
  (`display_kind`): a game that upscales after its tone map, its bytes display-encoded as a
  swapchain's (an upscaler writes through a UNORM view; no SRGB format takes a UAV). ENCODE hands
  NR those bytes as they are, R first, with no `white` and no knee. COMPOSE puts the answer's
  bytes in their place with the game's alpha at strength 1, and at another strength that much of
  the change in linear light, back through the sRGB table (bound at t4 now).
- mode=carry refuses a display picture, and says so: its passes work in scene light.
- FSR 3.1: an 8-bit output is taken whatever the context's non-linear flag says, since a game
  that upscales after its tone map need not set it; the flag still refuses every other format.
- `give_texel` is ENCODE on the CPU, for the carry path's read and replay's frame dumps.

**Checked on Windows** (Arc 140V, Intel 101.9150):
- `build_xess.bat` with no warning.
- `test_nr_xess`: all passed, with 13 new cases: the cut before XeSS with R8G8B8A8_UNORM,
  B8G8R8A8_UNORM_SRGB, strength 0.5 and 0 and a declined frame; after XeSS with R8G8B8A8_UNORM
  (#46's case), B8G8R8A8_UNORM and R8G8B8A8_TYPELESS; after FSR 3.1 with non-linear colour and
  with linear; mode=carry handing the colour on; and R10G10B10A2_UNORM before and after XeSS,
  which no case had run. A display picture's bytes come back exactly (worst error 0 at strength 1
  and 0, 0.0041 at 0.5), and `compare` now checks every format's alpha.
- The control: with the B/R swap and the alpha taken out of the shader, all four BGRA cases fail,
  and every 8-bit case that writes the answer fails its alpha.
- `test_xess_panel` passes.

**Not run:** a game with an 8-bit output (none here: Hogwarts upscales into RGBA16F); the real
daemon on such a frame (its side is the same R8G8B8A8 either way); Linux (nothing in `src/xess`
builds there).

**Pushed** to `alchemist` alone at the owner's word, 11-20; `master` and `newnr` stay at `30ec631`.

**Next on Windows:** a build for #46's reporter, the owner's call, then their answer. **Next on
Linux:** nothing.

## The fused kernels' layout per driver, constant 4: Mesa back to 0624ccc's speed (2026-11-16, late night, branch `newnr`)

The owner, 11-16, on the entry below: do it. `7944836`, local; `8c82133` since 11-21 (below).

**What changed.** `ffn_fused.comp` and `window_block.comp` take specialisation constant 4,
`FUSED_BY_COLUMN`: by column as `19679e9` made them, or by row as `0624ccc` had them; every
fragment holds the same values either way. libxmx gives it to every pipeline beside constants 1-3:
on for Intel's Windows driver, off elsewhere, `XMX_FUSED_BY_COLUMN=0|1` on any driver.
`xmx_fused_by_column()`, `Runtime.fused_by_column`, and the daemon's `runtime options` line say
which ran. `gpu_report_network.py`'s comparison process turns it off with the E4M3 bytes and the
wide weights by column, as 0.0.5 had them. `docs/WINDOWS.md` names it.

**The same bytes.** `test_ffn_fused.py`, `test_glue.py` and `test_window_block.py` bit-exact with
the constant 0 and 1. The heads are the same both ways and as before: B at 576x1024
(`edcbf478`, `acbd05d9`) and 320x512 (`df409ac4`, `b6d9870b`), A at 320x512 (`3e37b87c`,
`29d613d2`). Both shaders carry one `SpecId 4`.

**Timed on Mesa**, as below, three ways alternating; an IDE held ~20 % of a core throughout, so
the figures are ~3 ms above the quiet ones below, the order the same:

| ms, the device | `0624ccc`'s shaders | constant 0 | constant 1 |
| --- | --- | --- | --- |
| B 1024x576 | 46.62-47.21 | 46.63-47.63 | 50.89-51.27 |
| B 512x320 | 15.43-16.35 | 15.55-16.26 | 17.12-17.72 |
| A 1024x576 | 86.75-88.12 | 87.57-89.62 | 93.25-95.90 |
| A 512x320 | 30.57-32.27 | 31.67-32.85 | 33.13-34.15 |

Constant 0 runs as `0624ccc` did, so Mesa has its 4-6 ms back at 1024x576 and 1.5 at 512x320.

**Checked on Linux:** `make test` 73 of 73 in both memory modes, CTest 72 of 72, CMake with no
warning, and `XMX_FUSED_BY_COLUMN=1 make test-gpu` 43 of 43, the path Windows now takes.

**Carried onto 0.0.7's head**, 11-21: GitHub had gone from `f9ad754` to `ecdce7b` (0.0.7), so
the three local commits were rebased onto it, the code as `8c82133`; `backup-20261121-newnr-before-pull`
keeps them as they were. The Arc A-series switch (`XMX_PREFER_TILED`) had come into the same
places, and both are kept: in libxmx the constant beside `prefer_tiled` (on or off with the driver,
whatever that switch says); in the daemon's `runtime options` line `XMX_FUSED_BY_COLUMN` before
`XMX_PREFER_TILED`; in `gpu_report_network.py` all three forms are turned off only where the second
process compares with 0.0.5's weights. An A-series card's second process compares the kernels with
the fused layout as its driver has it, and its line adds `the fused kernels by column` when they
are. The checks above ran at the old base; at `8c82133` they ran again on 11-21, green (above,
11-21, evening).

**Not run:** Intel's Windows driver, where the constant is on and the kernels should compile to
what `19679e9` gave.

**Next on Windows:** once this is on GitHub, the MSVC build and CTest, and one `frame_profile.py
--size 576 1024 --weights work/mlxw/dlssnr-b-logical.safetensors` against `XMX_FUSED_BY_COLUMN=0`,
to see the 4 % is still there. **Next on Linux:** nothing for this.

## Timed on Mesa: `fused-bycol` 7-10 % slower; the weights by column and the E4M3 bytes gain nothing (2026-11-16, night, branch `newnr`)

The owner, 11-16: push (`a6699b6` and its entry, `master`, `newnr` and `alchemist` at `f9ad754` on
GitHub) and run the timings owed on Linux. Mesa 26.2.6, swap empty, on AC. `frame_profile.py
--warm 25 --runs 5 --calls 40`, a process a run, five rounds with the order swapped each round;
device ms, the band over the five. The series driver is the session's own (not in the tree).

**`fused-bycol` against `0624ccc`.** Between the two the network differs only in `ffn_fused.comp`
and `window_block.comp` (`19679e9`), so `0624ccc`'s two were built with the Makefile's command and
loaded through `XMX_FFN_SPV` and `XMX_WINDOW_BLOCK_SPV`, the tree otherwise the head's; the head's
two built the same way are `work/`'s byte for byte. Network B; this series ran quiet, before
another application started (below).

| ms | `0624ccc` | head |
| --- | --- | --- |
| 1024x576, the device | 43.27-44.86 | 47.95-48.97 |
| 1024x576, the fused feed-forwards | 6.93-7.44 | 10.04-10.19 |
| 1024x576, the window blocks | 11.89-12.05 | 13.18-13.29 |
| 512x320, the device | 15.22-16.13 | 16.59-17.17 |
| 512x320, the fused feed-forwards | 2.12-2.46 | 3.14-3.38 |
| 512x320, the window blocks | 3.46-3.66 | 3.82-3.91 |

So on Mesa the by-column fused kernels cost 4.4 ms of B's network at 2560x1440 and 0.4 (+10 %) and
~1.2 ms at 1280x720 (+7 %), where Intel's Windows driver gained 4 %. The feed-forwards lose most,
+40 %. The same bytes: `frame_replay.py` heads of B with `0624ccc`'s two shaders and the head's are
the same at 576x1024 (`edcbf478`, `acbd05d9`, which are Windows' too, so the two drivers agree
bit for bit) and at 320x512 (`df409ac4`, `b6d9870b`).

**The wide weights by column (`XMX_WEIGHTS_BY_COLUMN`) and the E4M3 bytes (`XMX_E4M3_WEIGHTS`) on
Mesa.** Both exact here: `test_gemm_e4m3.py` with `XMX_E4M3_WEIGHTS=1` decodes all 254 values (82
cases), `test_gemm_by_column.py` with `XMX_WEIGHTS_BY_COLUMN=1` 150 cases, and with both 274, and
the runtime takes both (its start-up checks pass). The heads of A and B at 320x512 and 768x1344 are
the same with each of the four settings. Timed, each against Mesa's default (both off); from
20:34:52 an IDE started and took 5-19 % of a core, so these bands are wider than the first table's,
but the order alternated:

| ms, the device (staged GEMM; the bottleneck's four GEMMs) | default | by column | E4M3 |
| --- | --- | --- | --- |
| B 512x320 | 16.41-17.33 (7.99-8.67) | 17.48-17.60 (8.80-9.17) | |
| B 1024x576 | 48.01-48.71 (19.90-20.40) | 49.74-50.61 (21.75-22.16) | |
| A 512x320 | 32.64-33.93 (19.73-20.37) | 33.60-34.78 (20.42-21.24) | |
| A 1024x576 | 89.06-94.44 (48.45-51.41) | 95.02-95.31 (53.41-54.45) | |
| B 512x320, E4M3 run | 16.44-17.10 (3.26-3.66) | | 17.06-17.31 (3.61-3.73) |
| B 768x448 | 31.10-32.26 (4.98-5.38) | | 31.44-33.18 (5.60-5.82) |
| A 512x320 | 31.31-33.31 (3.46-4.33) | | 31.99-34.61 (3.90-4.29) |
| A 768x448 | 58.60-61.12 (6.04-6.51) | | 59.60-61.36 (6.29-6.63) |

By column, the staged GEMM takes 1-5 ms more and the network 2-6 % (`improve-b.md` expected no
gain); with the E4M3 bytes the bottleneck takes up to 0.6 ms more. **Mesa's defaults are the right
ones for both**, and nothing changes.

**What follows.** `fused-bycol` is the one that costs Linux, and it is unconditional. A
specialisation constant, by column on Intel's Windows driver and by row elsewhere, as libxmx does
for the wide weights and the staged walk, would give Mesa its 4.4 ms back; the owner's word.

**Next on Linux:** that constant, if the owner wants it. **Next on Windows:** nothing; the constant
would keep Windows' kernels as they are.

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

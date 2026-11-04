# HANDOFF — read this first

State of the DLSS-NR on Intel Xe2 project as of **2026-11-05**. notes/CLAUDE.md holds the
original brief; **this file overrides it wherever they disagree**, and after
2026-09-09 they disagree about something foundational.

**How to read this file.** "Where things stand" and "Open items" are the current state. The dated
entries after them are the latest, newest first. Everything older is in `notes/archive/`, word
for word: the graph's recovery, the kernel work, the measurements and the experiments that did not
pay in `HANDOFF-until-2026-10-07.md`, the first Windows releases and the testers' reports in
`HANDOFF-2026-10-08-to-2026-10-15.md`, DirectX 12 through XeSS, preset B and PR #20's review in
`HANDOFF-2026-10-16-to-2026-10-28.md`, and DirectX 12 in the release, 0.0.2, the panel inside
DirectX 12 and the GPU report for Arc A-series in `HANDOFF-2026-10-30-to-2026-10-31.md`. It is
history: do not read it to start a task. Open it only when a line here points there, or before an
optimisation, to see whether it was tried and not kept. `notes/INDEX.md` says which note settles
what.

Keep this file short. Every session loads it whole and sends it again with every request it
makes, so its size is paid for on every turn, and `publish_check` refuses a file over 200 KB. When
the dated entries grow long, move the older ones the same way, into a new archive file: on 11-01
the entries before 10-30 went, and the file fell from 156 KB to 69 KB; on 11-04 those before
11-01, from 100 KB to 77 KB.

---

## Where things stand (2026-11-05)

- **It works on both systems of this laptop** (Arc 140V): Linux with Mesa, Windows with Intel's
  driver. A Vulkan layer hands each presented frame to a daemon that runs NVIDIA's network on the
  GPU and writes the answer back into the swapchain. DirectX 8-11 games go through DXVK. DirectX 12
  runs on Linux through VKD3D-Proton, and on Windows through the game's own XeSS (since 0.0.2).
- **The releases are Windows-only so far**: v0.0.1 (`7f9c364`), v0.0.1.1 (`f9ec5f1`), v0.0.1.2
  (`2f90064`, 10-15), v0.0.1.3 (`d89563f`, 10-18), v0.0.2 (`81b0a59`, 10-31), v0.0.3
  (`86a5799`, 11-01) and **v0.0.3.1** (`c0b7f9d`, 11-01, the latest), a ZIP with the setup window, DXVK 3.1.1, both layers, a
  `vulkan-1.dll` proxy and NR's `libxess.dll` as `nr_xess.dll`. 0.0.2 brings DirectX 12 through
  XeSS, the in-game panel (PR #20), network B and frames on the GPU by default, and an About tab
  that checks GitHub for a newer release (archive, 10-31). 0.0.3 brings the panel inside DirectX 12
  games, its keys and clicks kept from the game, and `nr_gpu_probe.exe`, the GPU report for Arc
  A-series owners (below, 11-01). 0.0.3.1 is what the first A-series reports asked for: the speed
  test measures both networks and names each, and setup finds a game's XeSS in a subfolder (below,
  11-01 later). All three went out on the owner's word before Linux's `make test` at their heads,
  as 0.0.1.3 did: 0.0.2's ran green after, at `eb9b281`, and 0.0.3's and 0.0.3.1's at `d28e841`
  (below, 11-04 later). Linux users build from source (README).
- **Branches**, on GitHub since 10-30:
  - `master`, the default: 0.0.1.2 (`2f90064`) until 10-30, then fast-forwarded to `newnr`, and
    with it to 0.0.2 (`81b0a59`, 10-31), 0.0.3 (`86a5799`, 11-01), 0.0.3.1 (`c0b7f9d`, 11-01),
    the README for players (`d5bef2e`, 11-02) and `alchemist`'s head, at `72fb922` and again that
    night (11-04, below);
  - `newnr`: everything since, with PR #20 merged at `097d6d4` and `dx12` on top of it
    (`cc2b8b7`, 10-30); level with `master`;
  - `media`: the README's pictures;
  - `improve-int8`, kept for INT8 experiments;
  - `alchemist`: the Arc A-series work, from `newnr` at `0a6000f`, on GitHub since 11-01 (archive,
    10-31 evening). Level with `newnr` until 11-02, ahead of it from 11-03 (`911e9ba`, the
    players' data, and the entries of 11-04 below), level with `master` and `newnr` again on
    11-04, when all three went to its head at the owner's word (below, 11-04 later), ahead of
    them that evening (`20a0253`, `6ef670e`, `c744581` and the Linux check), and level again
    since that night, when the owner had both fast-forwarded to it (below, 11-04 night, later);
  - ~~`claude/weights-only-quantization-w0o2wh`: one HANDOFF commit (`f06277d`) that reached GitHub
    by accident on 11-01~~: gone from GitHub by 11-04, 08:07 UTC. Its text is carried here (below,
    11-01, latest), and the branch is saved in a bundle in `D:\NRonWindows\backups` on Windows
    (below, 11-04).

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
  or faster (below, 11-04 evening). Most of it was the driver's walk order: since `6ef670e` the
  network takes 9-19 % less there, and B at 2560x1472 is within 2 % of Linux (below, 11-04
  night), and 7-9 % less in Mortal Kombat 11 at 1280x720 and render scale 1.0 (below, 11-04
  night, latest); the speed test's Windows figures predate it. The README's table
  (`nr_knobs.RATES`) was measured with `live_rates.py --warmup 3`, again on 10-18 with the clock
  hint, and the default warm-up of 25 now reads the same. On Linux libxmx asks the driver for its
  low-latency clock strategy since 10-18 (`XMX_LOW_LATENCY`): without it the GPU's governor
  clocked a frame with gaps around it down to 700-1200 MHz of 1950 (archive, 10-18).
- **On the players' GPUs** (below, 11-03; `notes/players-20261103.md`): a B580 runs network A at
  1280x720 and 0.4 in 9 ms, 3.9x an A770. An A770 plays level with this laptop at that size,
  because its network pays ~55 us a pass whatever the field. Eleven A-series cards compute NR's
  shape exactly, and a Pro B70 and two B580s offer it natively.
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
- ~~The first target for the network's speed~~: the owner chose the staged GEMM's short-K calls,
  and `6ef670e` takes 9-19 % off the network on Windows (below, 11-04 night). B's four
  full-resolution passes, the Linux agent's proposal, are not started (below, 11-04 evening).
- Answers to the A-series reports, as GitHub stood on 11-04: we have commented on #18, #19, #23,
  #25, #27, #29 and #34 (#34: two comments from the owner's account, 11-03 and 11-04), and not
  yet on #22, #24, #26, #28, #30-#33 and #36. Two questions the data raised: #25, which network
  each API ran (its reporter wrote on 11-01 that A and B made no difference in either API, and
  that Vulkan gave 23 fps against DirectX 11's 40 at one scale and model; the 11-03 reading below
  puts that down to A against B, so the two are not reconciled); #23, whether NR was on in
  Hogwarts (asked on 11-01, no reply). Below, 11-03 and 11-04.
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
  11-01 (#18, the owner's Reddit post), and seven A770 reports came in (below, 11-01 later).
  #23, #25, #27 and #29 answered on 11-01 at the owner's word (below, 11-01, latest); #22, #24,
  #26 and #28 are unanswered, and #18 was closed on 11-03.
- Preset B against the vendor's own B: a capture on the RTX 5060 Ti with
  `DLSSNR.Hint.Render.Preset` set to B, when the owner opens that machine. Archive, 10-17 later.
- The two files that came with the 310.9.16 DLL (`nvngx_dlssnr_compat-fp16-310.9.16-cuda13.4.3.rar`
  and `renodx-dlss.addon64`, in the Windows Downloads). The owner says, as their source claims,
  that the first is the DLL patched for RTX 30 and 40 cards, with FP16 kernels, and the second
  lets RenoDX pick preset A or B. Untrusted and unopened; they wait until one is needed.

**Linux** (what to pull and run, in order: below, 11-04)
- `e72be76`, the Windows release's own Python (below, 11-05): `make test` in both memory modes and
  CTest. `nr_daemon.py` puts its own folder on sys.path, and `test_release.py` and
  `test_windows_wizard.py` gained cases; nothing Linux runs otherwise changed.
- ~~`6ef670e`, the staged GEMM's blocks read from a Y-major walk where the driver walks so (below,
  11-04 night): `make test` in both memory modes and CTest~~: green at `c744581`, and Mesa's own
  order is the right one for it, by 2-3 % of the frame (below, 11-04 night, later).
- ~~`alchemist`'s GPU report changes (`8f96b58`, `eab7eb3`, `c231bdc`, `9ceff90`, archive, 10-31
  evening): `make test`~~: green at `d28e841` (below, 11-04 later).
- ~~`speed_test.py`'s `network_of` and the probe's warm-up (below, 11-01 later): `make test`~~:
  green at `d28e841`.
- ~~The README split into `docs/DEVS.md` and `docs/SETTINGS.md` (below, 11-02): `make test`, for
  `test_toggle.py`'s page check and `nr_daemon.py`'s message~~: green at `d28e841`.
- ~~`player_reports.py`, its test and `knob_doc.py`'s third block (below, 11-03): `make test` and
  CTest, which now run `test_player_reports.py`~~: green at `d28e841`, CTest 68 of 68.
- ~~`test_toggle.py`'s "status has no hardware-independent FPS guess" fails about one run in 200:
  it looks for `7x` in `nr-ctl report`'s output, which prints the log's path, and the path is a
  temporary folder with a random name~~: fixed in `ce9c513` (below, 11-04 later).
- ~~`81b0a59` (0.0.2): `make test` in both memory modes and CTest~~: green at `eb9b281` (10-31),
  which holds it and the panel in DirectX 12. Since `cc2b8b7` the setup window's core changed (its
  PE readers, the folder of each placed file, frames on the GPU and B by default), the release
  builder (`VERSION`, the icon) with its tests, and `nr_focus.h` and `nr_layer_state.h`, which
  Linux compiles. Both layers and CMake build with no warning; `make test` 60 of 60 in both memory
  modes, CTest 67 of 67, `make test-proton`, all 12 `D3D12ProtocolTests`; the Python suites of
  `src/tools` 217 tests, the 24 skipped all Windows-only. Archive, 10-30 evening and 10-31.
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
- The next release, at the owner's word: the release's own Python (`e72be76`, below, 11-05) and
  the staged GEMM's walk (`6ef670e`, below, 11-04 night) are in none yet. With it the README's
  "You need" loses its Python line, and `VERSION` and the README's tables change.
- ~~The GPU report for Arc A-series owners (`nr_gpu_probe.exe`, branch `alchemist`, archive, 10-31
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
- The staged GEMM's loop on Intel's compiler takes 1.2-1.5x Mesa's time where the graph is large,
  and took 1.6-2.1x at the vendor's 1080p and 1440p fields (below, 11-04 evening). Most of that
  was the driver's walk order, read since `6ef670e`: the GEMM is 1.2-1.3x Mesa's time there now,
  and the residual at K = 64, the QKV gather and the bottleneck's contract are what is left (below,
  11-04 night). The packed copies took 6-9 %. Archive, 10-02 and 10-03.
- ~~Four daemon processes with Mortal Kombat 11~~: two processes are one daemon (the venv's
  `python.exe` and the interpreter it starts); four were two. The owner lock now stops a second
  one. Archive, 10-15.
- DOOM (2016) through 0.0.1.2 is untested: the owner's own installation from another release is in
  its folder, and its removal was refused by this session's safety check. Archive, 10-15.
- `windows_launch.py`'s Configure Steam, Restore Steam and their wrapper can go, with their tests,
  once no release with those buttons is in testers' hands. The GitHub releases have none. Archive,
  10-09.
- ~~DirectX 12 is not offered by the release~~: the setup window's **DirectX 12 (games with
  XeSS)**, `6c503ae` (archive, 10-30 evening). VKD3D-Proton does not run Hogwarts Legacy on Windows
  (archive, 10-16).
- A Vulkan or DirectX 8–11 game that runs elevated, or any with UAC off, never gets the layer; the
  HKLM manifest that would reach it is kept for later. Archive, 10-08 and 10-09.
- ~~The panel inside a DirectX 12 game, and its keys and clicks kept from the game~~: on `newnr`,
  in Hogwarts with the owner, frame generation on and off (archive, 10-31 later). For the next
  release after 0.0.2, the owner's call. A game reading keys by `GetAsyncKeyState`, DirectInput or
  a gamepad still gets them while the panel is open.
- DirectX 12 in other games: only Hogwarts Legacy (XeSS 2.0.2.53) has run. Check takes any XeSS
  whose exports NR's proxy carries, and an older XeSS's execute parameters are taken to be laid
  out as 2.0's. Archive, 10-30 evening.
- ~~`dx12`: record a real Hogwarts sequence with the owner~~: recorded and replayed on 10-16,
  and the trail found in it fixed (`e803624`). Its look in the game is untested. Archive, 10-16.
- MSVC: the composition is not vectorised (0.5-1.8 ms behind gcc), and `_Pragma("GCC ivdep")`
  gives three C4068 warnings that an `NR_IVDEP` macro would silence. Archive, 10-02 and 10-07.
- ~~`tools\build_win.bat` run from a folder other than the checkout stops at the layer's link
  (`LNK1181`): the layer's C objects are compiled with no `/Fo` and land in that folder~~: fixed
  in `2c04f2f`, with `/Fo:"%REPO%\\"` as the x86 step has. Below, 11-04 night, latest.
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

## Windows releases carry their own Python (2026-11-05, branch `alchemist`)

The owner asked for an easier install, with no Python for a player to set up. Python's own share
of a frame is half a millisecond (below, 11-04 night, latest), so a daemon in C would buy nothing
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

## `6ef670e` in a game: Mortal Kombat 11 (2026-11-04, night, latest, branch `alchemist`)

At the owner's word, the new order in a game, and, as the owner asked, at 1280x720 with render
scale 1.0 as well as the release's 0.4. A release built at `0456e99` (`tools\deploy.bat
--release`, the weights copied in) went into Mortal Kombat 11 through the setup window's backend,
as a player's goes: DirectX 11 through DXVK, frames on the GPU, 1280x720.
`nr-game\pacing-game-test.ps1` counts the frames the daemon answers in 40 s after a 15 s warm-up
and reads the network's time from its log, each run in a game and a daemon of its own. The old
order was set with `XMX_STAGED_WALK_Y=0` in the installed `nr-env.txt`, the new one by nothing, as
players get it; two rounds, the second in reverse order. Intel's 101.9150, on mains, "best
performance", `quiet-check.ps1` quiet before and after. The runs are in
`D:\NRonWindows\nr-game\release-tests\walk1104-*` (0.4) and `walk1104s-*` (1.0), and the two
series scripts beside `pacing-game-test.ps1`.

Frames the daemon answered a second, and the network's ms (the daemon's split, in whole ms):

| | old order | new order |
| --- | --- | --- |
| B at 1.0 (network 1344x768) | 9.75-9.82, 87-89 | 10.44-10.57, 81-82 |
| A at 1.0 | 5.40-5.47, 170 | 5.85-5.92, 154-157 |
| B at 0.4 (network 512x320) | 36.1-36.8, 16-17 | 38.3-38.7, 16 |
| A at 0.4 | 23.3-23.8, 32-33 | 22.8-23.8, 32-33 |

- **At render scale 1.0 the network takes 7-9 % less, A and B alike, and the game gets a frame
  6-10 % more often.** At 0.4 B's frame comes 5-6 % more often in both rounds, and A's runs do
  not tell the two orders apart: at 512x320 the GEMMs' operands mostly stay in the cache, where
  the walk matters little (the entry below).
- All 16 runs: every frame on the GPU path, none refused or fallen back; no daemon outlived its
  game; the game's folder as before after each, its ten files a run could write byte-identical to
  the October backup, and no launch options in Steam.
- The picture was not looked at: the bytes are the same by `test_staged_packed.py` and the heads
  (the entry below).

**Python's share of the frame, the same night** (the owner asked whether the daemon's Python
costs speed). The GPU image path run as a game's frame goes through it, the frame already on the
device, with every call into libxmx timed (`D:\NRonWindows\nr-game\profile-frame.py`, `-I`): Python
and numpy between those calls take 0.48-0.55 ms a frame, 2.4 % of B's 20.1 ms at 0.4 and 0.4 % of
A's 155.5 at 1.0, and the process's CPU time is 0.6-1.3 ms a frame. Outside the network's one
graph replay, the frame is the image passes' eight submits with their waits, 3.7-5.9 ms. So a
daemon written in C would take at most half a millisecond off a frame; fewer submits is the larger
lever, unmeasured.

**A trap on the way.** `tools\build_win.bat`, run from a folder other than the checkout, stops at
the layer's link (`LNK1181`, `nr_layer.obj`): the layer's C objects are compiled with no `/Fo`, so
they land in the current folder, and unlike the C++ objects nothing moves them. Here they landed in
`D:\NRonWindows` and were removed. `2c04f2f` gives them `/Fo:"%REPO%\\"`, as the x86 step has:
built from another folder and from the checkout, no object is left in either, all 62 outputs keep
their sizes, and the layer's fixtures and `test_daemon.py` pass.

**Next on Linux:** nothing waits. **Next on Windows:** the residual at K = 64 and the QKV gather,
if the owner wants them; the next release, which takes `6ef670e` to players, at the owner's word.

## Checked on Linux: `6ef670e`, and the walk on Mesa (2026-11-04, night, later, branch `alchemist`)

As the Windows agent asked through the owner: `alchemist` fast-forwarded to GitHub's `c744581`,
which holds `6ef670e` and after it changes only this file. No daemon, swap empty, a quiet CPU;
Mesa 26.2.5.

- `make test`: 62 of 62, in 2 min 42 s. `XMX_STAGING=1 make test`: 62 of 62, in 2 min 10 s, the
  GPU suites that print the mode saying `staging=True`. In both, `test_staged_packed.py` finds its
  960 outputs byte-identical with the packed loader and the walk, each way and both. Skipped as
  before: the Windows-only cases, and the GPU image path's parity tests on Mesa. No compiler
  warning.
- CMake builds with no warning, and CTest passes 68 of 68.

**The walk on Mesa.** Mesa keeps its own order by default. Asked for the Y-major reading anyway
(`XMX_STAGED_WALK_Y=1`), it does not lose what Windows lost, as I had predicted it would: the
remap is not its own inverse. On a walker that runs X fastest it makes interleaved streams of row
blocks. At N = 64-128, two to four column blocks, nearly every row block's column blocks still
start within 12 workgroups of one another, and its A is still read once. In the bottleneck, where N is
1024-4096 and the row blocks are few, it scatters both operands.
- Alone (`gemm_epilogues.py`, the nine shapes of both Windows files, `--rounds 5 --no-sweep`, four
  processes, off on on off; us a call, the gate column, off and on): `235520x128x64` 1124-1144 and
  981-983, `34560x128x128` 249-250 and 226-230, `138240x64x64` 384-385 and 359-363,
  `58880x128x128` 375-382 and 366-367, `14720x128x256` 161-162 and 159, the bottleneck's
  `640x1024x4096` 742-746 and 812-843 and `960x1024x4096` 1070-1075 and 1285-1287. Mesa's own
  order runs `235520x128x64` in the time Windows' new one does (1132-1143), as the entry below's
  explanation expects.
- In the frame (`frame_profile.py --warm 25 --runs 5 --calls 30`, two rounds, the order swapped
  in the second; device ms, with the staged GEMM's share):

| | walk off | walk on |
| --- | --- | --- |
| B 2560x1472 | 254.4-257.7 (GEMM 101.8-104.5) | 259.3-261.7 (GEMM 108.0-109.3) |
| A 1920x1152 | 272.7-273.1 (GEMM 149.3) | 279.9-282.4 (GEMM 156.5-157.8) |

- The bottleneck's expand (`960x4096x1024`, `640x4096x1024`) takes 1.37-1.40x with the walk on,
  and its contract 1.07-1.22x; the gate at K = 64 gains 7 % in B's frame. So Mesa's default is
  the right one, by 2 % of B's frame and 3 % of A's. With it the frame reads as this morning's
  (B 253.6, A 271.4), within the 1-2 % a process moves, so the change costs Mesa nothing seen. An
  order chosen per shape would be worth about a millisecond of B's 254, not worth taking.

**Then, at the owner's word.** `alchemist` was pushed with this entry and with this file's
entries of 10-30 and 10-31 moved word for word to `notes/archive/HANDOFF-2026-10-30-to-2026-10-31.md`
(`0954b48`), and `master` and `newnr` were fast-forwarded to `alchemist`, on GitHub and here.

**Next on Linux:** nothing waits. **Next on Windows:** `git fetch`, then fast-forward `master`,
`newnr` and `alchemist` to GitHub's; then the entry below's: a game with the new build, at the
owner's word.

Done: both on Windows, the game in Mortal Kombat 11 (above, 11-04 night, latest).

## The staged GEMM reads its blocks from Intel's Y-major walk (2026-11-04, night, branch `alchemist`)

The owner chose the staged GEMM's short-K calls as the first target, as long as the picture stays
the same (the entry below, "For the first target"). The change is `6ef670e`.

- **Why it lost.** These GEMMs wait on memory. On Linux the gate GEMMs at K = 64-128 move 80-88
  GB/s, the machine's ceiling, and on Windows 34-44 GB/s: what Linux's traffic would take if every
  row block's A were read once per column block, four times at N = 128. A probe that hands each
  workgroup a ticket as it starts (`work/tools-win/walk_order.py`, untracked) shows why: **Intel's
  Windows driver walks a dispatch with Y fastest.** At `58880x128x128 x4` a row block's four column
  workgroups start 2760 tickets apart, 3 x 920 row blocks, where Mesa's walk, X fastest, starts
  them 3 apart. That fits all three readings below: the N = 32 contracts at Mesa's speed (one
  column block), the loss at N >= 64 and short K, and the loss growing with the field (A at 8640
  rows, 4.4 MB, stays in the cache; at 14720, 7.5 MB, it does not).
- **The change.** `gemm_staged.comp`'s constant 3, `STAGED_WALK_Y`, reads a workgroup's place in
  the Y-major walk column first, so the column blocks of one row block start together. libxmx sets
  it on Intel's Windows driver; `XMX_STAGED_WALK_Y=0|1` sets it either way on any driver, and
  `xmx_staged_walk_y()` says which (`docs/WINDOWS.md`). Only which workgroup computes a block
  changes, not how.
- **The same bytes.** `test_staged_packed.py` runs four processes, the packed loader and the walk
  each way, and its 960 outputs (both specializations, partial blocks, batches, the 32-row builds)
  are byte-identical in all four. The heads are unchanged with the walk off and on: A at 320x320
  (`e62005b8`) and 1280x720 (`c217fd2f`), as in October, at 1920x1080 and at 2560x1472, and B at
  1920x1152 and 2560x1472.
- **Alone** (`gemm_epilogues.py`, us a call, walk off and on): 235520x128x64 gate 2073-2145 and
  1132-1143, residual 4349-4390 and 1627-1630; 58880x128x128 gate 818 and 381; 14720x128x256 gate
  219-232 and 162-165; 138240x64x64 gate 698-701 and 527-528. The N = 32 contract and 8640x128x256,
  whose A stays in the cache, run as before.

In the frame, device ms: the entry below's `frame_profile.py`, three rounds with the walk off and
on in alternate order, quiet before and after, 101.9150 (`D:\NRonWindows\windows-profiles-20261104\walk`).

| | Linux | Windows, walk off | Windows, walk on |
| --- | --- | --- | --- |
| A 1920x1152 | 271.4 | 343.8-353.5 | 288.6-297.3 |
| A 2560x1472 | 448.9 | 599.1-618.6 | 485.1-498.5 |
| B 1920x1152 | 151.5 | 176.9-182.5 | 156.2-165.3 |
| B 2560x1472 | 253.6 | 303.4-312.3 | 258.7-260.0 |
| A 1920x1152, staged GEMM | 148.1 | 234.8-241.2 | 179.6-184.6 |
| A 2560x1472, staged GEMM | 240.0 | 415.4-429.0 | 298.0-306.4 |
| B 1920x1152, staged GEMM | 63.2 | 98.2-101.5 | 76.9-81.8 |
| B 2560x1472, staged GEMM | 101.3 | 168.4-172.9 | 123.2-124.0 |

- **The network takes 9-19 % less, and about two thirds of the GEMM's loss to Mesa is gone**: the
  GEMM is 1.2-1.3x Linux's time, from 1.55-1.8x in these rounds. B at 2560x1472 is within 2 % of
  Linux.
- The gate GEMMs at K = 64-128 take 1.03-1.16x Linux's time, from 2.1-2.4x (`58880x128x128 x4` at
  2560x1472: 37.6-39.0 to 17.8-18.5 ms, Linux 16.3).
- **Left:** the residual at K = 64 (`235520x64x64`, two column blocks: 14.0-14.5 to 9.5-9.6 ms,
  Linux 6.7), the QKV epilogue with the window gather (1.3-1.7x Linux's time where Linux lists it),
  and the bottleneck's contract (`960x1024x4096`: 12.6-14.8 to 10.2-10.5, Linux 8.7). Window
  attention takes 0.5-2.5 ms more with the walk on (A at 2560x1472: 27.8-28.5 to 30.3-30.8); every
  other pass is as before.
- B's four full-resolution passes are now 27-28 % of B's frame on Windows (31-32 on Linux), at the
  same 43-75 ms.

**Checked on Windows:** the MSVC build (the three known C4068), and CMake's 50 GPU, image, ref and
tool tests one by one with the native Python: 48 pass, and the two int8 experiments skip, as they do
without `gemm_runner`, which `build_win.bat` does not build. **Not checked:** a game; the desktop
Arc GPUs, which have the same driver ID and so take the new order, whether their walk is Y-major
too; Linux.

**Next on Linux:** `make test` in both memory modes and CTest at `6ef670e` or later: the shader and
libxmx changed. Mesa keeps its X order by default, and `test_staged_packed.py` now runs Mesa with
the walk both ways too. **Next on Windows:** a game with the new build, at the owner's word; then
the residual at K = 64 and the QKV gather, if the owner wants them.

## The network's passes at the vendor's 1080p and 1440p fields, Intel's driver beside Mesa (2026-11-04, evening, branch `alchemist`)

The owner asked for a per-pass profile of the network on Intel's driver at the fields render scale
1.0 gives 1920x1080 and 2560x1440, 1920x1152 and 2560x1472, set against the Linux agent's of the
same morning: `frame_profile.py --size H W --warm 25 --runs 5 --calls 30`, network B with
`--weights work/mlxw/dlssnr-b-logical.safetensors`.
- Linux: Mesa 26.2.5, one process a case. The outputs are in `D:\NRonWindows\linux-profiles-20261104`.
- Windows: Intel's 101.9150 before and after, on mains, "best performance" as in phase72's Windows
  table, `quiet-check.ps1` quiet before and after each batch of runs, the first run 16 minutes
  after a boot, no daemon. The MSVC build of `c0b7f9d` in `work/` (nothing the network runs has
  changed since), under a CPython 3.12. Each case in three processes, the second round in reverse
  order. The outputs, and the two scratch scripts that made them, are in
  `D:\NRonWindows\windows-profiles-20261104`, as UTF-8 like Linux's.

Device ms a frame, Linux / Windows' three rounds (Windows against Linux):

| pass kind | A 1920x1152 | A 2560x1472 | B 1920x1152 | B 2560x1472 |
| --- | --- | --- | --- | --- |
| staged GEMM | 148.1 / 242.7-251.2 (1.64-1.70x) | 240.0 / 441.6-499.6 (1.84-2.08x) | 63.2 / 99.0-104.1 (1.57-1.65x) | 101.3 / 177.9-197.2 (1.76-1.95x) |
| window block | 57.8 / 46.5-50.4 (0.80-0.87x) | 98.2 / 78.6-80.5 (0.80-0.82x) | 43.6 / 34.7-35.6 (0.80-0.82x) | 74.4 / 59.7-60.2 (0.80-0.81x) |
| fused feed-forward | 33.3 / 34.5-36.2 (1.03-1.09x) | 56.5 / 58.6-59.4 (1.04-1.05x) | 25.5 / 26.9-27.5 (1.05-1.08x) | 43.5 / 46.4 (1.07x) |
| window attention | 20.1 / 18.4-18.6 (0.91-0.92x) | 31.2 / 30.2-31.1 (0.97-1.00x) | 7.1 / 6.5-6.7 (0.91-0.94x) | 11.4 / 11.3-11.7 (1.00-1.03x) |
| global attention | 4.8 / 3.4-3.6 (0.71-0.75x) | 10.7 / 7.5-7.9 (0.70-0.73x) | 4.8 / 3.4-3.6 (0.71-0.74x) | 10.8 / 7.5-8.0 (0.70-0.74x) |
| the four unary kinds | 7.3 / 8.2-9.2 (1.13-1.27x) | 12.4 / 13.5-13.9 (1.10-1.12x) | 7.2 / 7.7-8.2 (1.07-1.14x) | 12.3 / 13.6-13.9 (1.10-1.13x) |
| all but the GEMM | 123.3 / 111.2-117.6 (0.90-0.95x) | 208.9 / 188.5-192.5 (0.90-0.92x) | 88.3 / 79.2-81.6 (0.90-0.92x) | 152.3 / 138.6-140.2 (0.91-0.92x) |
| device total | 271.4 / 355.1-362.4 (1.31-1.34x) | 448.9 / 630.0-691.0 (1.40-1.54x) | 151.5 / 178.1-185.7 (1.18-1.23x) | 253.6 / 316.5-336.3 (1.25-1.33x) |

The host's part, mostly the input's upload from numpy, is 7.9-15.5 ms on Windows and 8.8-12.6 on
Linux.

- **Intel's compiler loses the time in the staged GEMM alone, and more than all of it.** Every
  other pass together takes 0.90-0.95x Linux's time: the window block 0.80-0.87x and global
  attention 0.70-0.75x, while the fused feed-forward and the unary kinds take 3-27 % more, a few
  ms. So the GEMM is 67-72 % of A's frame and 56-59 % of B's here, against 53-55 and 40-42 on
  Linux. At 1344x768 it was 1.54x (archive, 10-02); here it is 1.57-1.70x at 1920x1152 and
  1.76-2.08x at 2560x1472.
- **Per call site, a short K loop under an epilogue loses most.** The gate GEMMs (`0x1300`) of
  three levels do the same 7.7 GFLOP a call at 2560x1472. Windows runs them at 3.7-3.8 TFLOP/s
  against Linux's 6.8 with K = 256 (`14720x128x256 x8`), 2.2 against 5.7 with K = 128
  (`58880x128x128 x4`) and 1.5-1.6 against 3.8 with K = 64 (`235520x128x64 x2`); at 1920x1152 the
  same three run 5.6-5.7, 2.4-2.6 and 1.7-1.8 against 6.7, 5.6 and 3.8. The residual (`0x41100`)
  takes 2.9-3.1x at K = 128 and 2.1-2.2x at K = 64, the QKV epilogue with the window gather
  2.1-2.3x at K = 64-128 and 1.4-1.6x at K = 256. The narrow N = 32 contracts (`0x1100`) stay at
  1.0-1.3x, as on 10-02. Of the loss in the call sites both sides list among their 30 dearest,
  57-74 % is in calls with K = 64-128.
- **The loss grows with the field.** The K = 256 gate GEMM is 1.18-1.20x at 1920x1152 and
  1.78-1.83x at 2560x1472, the residual at K = 256 1.53-1.56x and 2.74-2.79x. Linux's frame takes
  the K = 256 gate GEMM in proportion to its rows (1.69x for 1.70x), Windows' at 2.5-2.6x. Alone on
  Intel's driver (`gemm_epilogues.py --no-sweep`, in the Windows folder) 8640x128x256 to
  14720x128x256 takes 2.4x the time, and 34560x128x128 to 58880x128x128 1.7x, as its rows.
- **At 2560x1472 the GEMM's time moves 11-13 % between processes, while every other pass together
  holds within 2 %**: the trap of 10-02 (section 4). Much of the move is the bottleneck's long-K
  GEMMs at 960 tokens. `gemm+residual 960x1024x4096` took 1.7, 4.3 and 6.1 ms a call in A's three
  rounds and 2.0-2.9 in B's, against Linux's 1.09 in its frame and 1.56 alone here (1.52x the
  640-token time, as its rows). A fourth process timing each call (`bottleneck-calls.txt`) had A
  at 1.5-2.0 and B at 3.2-4.9, steady over its frames, and its frames read A 750-758 ms and B
  361-365. Global attention, run between those GEMMs, keeps 0.70-0.74x in every process, so the
  GPU's clock is not what moves. At 1920x1152 the QKV epilogue `2560x1536x512` moves too, 1.05-1.72
  ms a call against Linux's 0.71. So a 2560x1472 frame on Windows is A 630-758 ms and B 317-365:
  quote a band.
- **B's four full-resolution passes are under a quarter of its frame on Windows, not a third**:
  43.3-44.2 ms of 178-186 at 1920x1152 (24 %) and 74.4-75.0 of 317-336 at 2560x1472 (22-24 %),
  against Linux's 47.2 of 151.5 (31 %) and 80.6 of 253.6 (32 %). They take 0.92-0.94x Linux's
  time: the two window blocks 0.78-0.85x, the merged feed-forward 1.15-1.18x, the stem's
  1.03-1.07x.

**For the first target.** On Windows, where the releases are, the staged GEMM is all of the loss:
B +36-41 ms at 1920x1152 and +77-96 at 2560x1472, A +95-103 and +202-260. At Mesa's GEMM speed
B's frame would be ~142-145 and ~240 ms there, A's ~259-266 and ~429-433, under Linux's own,
because every other pass is faster here. B's four full-resolution passes take about the same
43-81 ms on both systems, so a gain there pays on both, but they are under a quarter of B on
Windows. The staged loop on Intel's compiler was taken apart on 10-02 and 10-03 (archive): the
loss is the K loop's own code, not the stage, four bit-identical variants were no faster, and the
packed copies that took 6-9 % are in. The Linux agent proposes B's four full-resolution passes;
Windows reads the short-K GEMMs (K = 64-128 under the gate, the residual and the QKV gather) as
the larger target for players. The choice is the owner's.

**Not established:** why Intel's compiler loses at short K; why the K = 256 shapes grow faster
than their rows on Intel's driver, alone too; what moves a GEMM's time between processes.

**Next on Linux:** nothing waits. If the owner picks the staged GEMM, the same isolated run beside
`windows-profiles-20261104\gemm-epilogues-bottleneck-shapes.txt`, to say whether Mesa's kernel
alone also takes 14720x128x256 at more than its rows:
`python3 src/bench/gemm_epilogues.py --shapes 640x1024x4096 960x1024x4096 8640x128x256 14720x128x256 34560x128x128 58880x128x128 --rounds 5 --no-sweep`.
**Next on Windows:** the owner's choice of the first target.

## Checked on Linux: `alchemist` at `d28e841`, and `test_toggle.py`'s flake fixed (2026-11-04, later, branch `alchemist`)

The steps of the entry below, at the owner's word of 11-04: `alchemist` fast-forwarded to GitHub's
`d28e841`, which holds `newnr` (`d5bef2e`) and the players' data; no daemon, swap empty, a quiet
CPU; Mesa 26.2.5.

- `make test`: 62 of 62, in 5 min 11 s. `XMX_STAGING=1 make test`: 62 of 62, in 5 min 3 s, the
  seven GPU suites that print the mode saying `staging=True` (`False` in the first run). The probe
  says "run here", `test_player_reports.py` passes its 7, and `test_toggle.py`'s page checks pass
  (`knob_doc.py --check`, and the links of the README, `docs/DEVS.md` and `docs/SETTINGS.md`).
  Skipped as before: the Windows-only cases, and the GPU image path's parity tests on Mesa
  (`xmxres.image_path_exact`). No compiler warning.
- CMake builds with no warning, and CTest passes 68 of 68, `player_reports` the new one.
- **The flake, fixed in `ce9c513`.** Every path `nr-ctl report` prints is in the test's scratch
  folder, so the check now reads the report with that folder's path taken out. With `TMPDIR` at a
  folder named `tmp7x` the old check fails every time and the new one passes; a report saying "the
  strength knobs cost 7x" still fails it; 40 ordinary runs pass. At the head, `ce9c513` and this
  entry: `make test` 62 of 62, the same commands and passes as at `d28e841`.

**GitHub at 08:07 UTC, against the entry below.** The stray branch is gone: `git ls-remote` lists
`alchemist`, `improve-int8`, `master`, `media` and `newnr`. Nothing else has moved. Two B580 GPU
reports came after `911e9ba`'s data: #34's comment of 11-03 and #36 (11-04, on the A-series
template). `player_reports.py probe` reads both: NR's shape at 32 lanes 58.6 and 58.3 TFLOP/s,
vector FMA 7.2, driver 101.9150, "run here". On #34 another B580 owner offers to test and asks for
a Telegram group; the owner answered that a channel is coming.

**Then, at the owner's word.** Both B580 reports went into `players.json` (14 probes, `updated`
11-04), and into a row of `notes/players-20261103.md`'s GPU table: 1.9-2.1x this laptop in NR's
shape, and 1.4-1.5x the six faster A770s, less than the B580's lead in the network. `alchemist`
was pushed, and `master` and `newnr` fast-forwarded to it, on GitHub and here; `media` and
`improve-int8` stay where they were.

**Next on Linux:** nothing waits. **Next on Windows:** `git fetch`, then fast-forward `master`,
`newnr` and `alchemist` to GitHub's.

## GitHub as of 11-04, and what Linux pulls and tests (2026-11-04, branch `alchemist`)

On 11-04 the owner had the local branches follow GitHub, the stray branch saved, this file's
facts brought up to date, and the Linux agent told what to pull and test.

**GitHub, read with `gh` on 11-04.** `master` and `newnr` at `d5bef2e`; `alchemist` one commit
ahead (`911e9ba`, the players' data) and, once pushed, with this entry; `media` and `improve-int8`
as before. The latest release is v0.0.3.1; no pull request is open. Issues: #18 closed 11-03, #19
11-01, #27 11-02 (its reporter: it works now), #35 11-03 (Expedition 33 with native XeSS, closed
with no comment); #36 is new. Who has been answered: Open items, "Waiting for the owner".

**The stray branch.** `claude/weights-only-quantization-w0o2wh` is one HANDOFF commit (`f06277d`)
from another session that reached GitHub by accident on 11-01. The owner has it removed later. Its
text is the entry "The reporters answered" below (`0f378a3`). The branch is saved in
`D:\NRonWindows\backups` as
`github-stray-branch-claude-weights-only-quantization-2026-11-04.bundle` (4.5 MB, SHA-256
`674daf321d7a761906e3f91e3f038823f29574ea56b3f8954bbb3abce0e6d697`);
`git fetch <bundle> 'refs/heads/*:refs/heads/*'` restores it.

**Next on Linux** (the owner, 11-04: pull everything and start testing):
1. `git fetch origin`, then `git switch alchemist` and `git merge --ff-only origin/alchemist`. If
   this entry is not on GitHub yet, fetch `alchemist` from the Windows checkout on the shared disk
   (`D:\NRonWindows\dlss-nr`) instead. The head holds `newnr` (`d5bef2e`) and the players' data, so
   one run covers every Linux item in Open items. Do not build on the stray branch.
2. Stop the daemon, then `make test` in both memory modes (`XMX_STAGING=1` is the second) and
   CTest. New in them: `test_player_reports.py`, and `test_toggle.py` checking the two new pages
   and their generated tables. `make test` requires the probe to say "run here" on Mesa.
3. If the only red test is `test_toggle.py`'s "status has no hardware-independent FPS guess", run
   it again before reporting: it fails about one run in 200 (Open items). Fix it if that is cheap.
4. Write the result as an entry "Checked on Linux" with the commit tested, strike the Linux items
   in Open items and name the commit, and commit. Pushing is the owner's word.

Done: green at `d28e841`, the flake fixed in `ce9c513` (above, 11-04 later).

**Next on Windows:** nothing waits on Linux. The owner's word ~~on pushing `alchemist`~~ (pushed
11-04, above), on `arc-a-n8`, on replies to the issues still without one, and ~~on removing the
stray branch~~ (gone by 11-04, 08:07 UTC, above).

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
owner's word on answers to ~~#23, #25~~ (answered 11-01), #30-#33 and ~~#34~~ (answered 11-03
and 11-04), and on asking an A-series tester for a per-pass profile.

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

## The reporters answered (2026-11-01, latest; nothing in the code)

Posted at the owner's word, from the Linux session under the owner's account, each comment marked
as Claude-written:
- **#23**: asked for 0.0.3.1's speed test (0.0.3's recorded no network, and B is the default since
  0.0.2, so A and B cannot be told apart) and for Save report, whose daemon log carries `buffers in
  ...` (mapped BAR or copies) and each frame's `gpu a+b+c` split. Its eight cases fit **~22 ms a
  frame + 93 ms per megapixel** of network field (1280x720 at 1.0 predicts 119, measured 117): the
  fixed part is what loses to the 140V at 640x360, and only the log places it. Also asked whether
  NR was on in Hogwarts; 55-60 presented with frame generation x2 is 27-30 rendered, which fits NR
  answering each frame at that scale.
- **#25**: Save report after a Vulkan launch (the launch record says whether the proxy ran; a game
  started elevated, or loading the loader by its full path, never gets the layer), and which setup
  mode each game had.
- **#27**: fixed in 0.0.3.1, confirmation asked; no manual path in the window.
- **#19 closed**: resolved by the reporter (not as administrator, frames on the GPU); their request
  for an fps display is noted, with the daemon's log and Save report as the files that carry it.
- **#29**: the seventh A770 report (driver 101.9150, can run test builds, a game later),
  answered as the others.

Unanswered: #22, #24, #26 and #28 (#28 reads 25 % below the other six at the same driver, likely
the card's clock or power, not the code); #18 waits for its reporter.

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

**Next on Windows:** ~~answers to the reporters (#23: which network, and whether NR was on; #25: Save
report for the Vulkan games; #27: where the DLL is), at the owner's word~~: posted on 11-01 from the
Linux session (below, 11-01, latest).
**Next on Linux:** `make test` for `speed_test.py` and the probe (Open items).

## 0.0.3 released: the panel in DirectX 12, and the GPU report for Arc A-series (2026-11-01, branches `newnr`, `master`, `alchemist`)

**The release.** `v0.0.3` at `86a5799` (`VERSION` 0.0.3), on `master`, `newnr` and `alchemist`, on
the owner's word: `dlss-nr-windows-0.0.3-x64.zip`, 21 551 071 bytes, SHA-256
`acf22fa84111f8c73436b6a2069622b7c59c2c4e95a9604b371cdcf55eb0f2c1` (GitHub's digest the same), its
notes in `work\pr-replies\release-notes-0.0.3.md` on Windows. It carries the panel inside DirectX 12
games and its keys and clicks kept from the game (archive, 10-31, later), and `nr_gpu_probe.exe` with
the fixes of 11-01 (archive, 10-31, evening), and asks Arc A-series owners to send the report and to
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

# HANDOFF — read this first

State of the DLSS-NR on Intel Xe2 project as of **2026-10-17**. notes/CLAUDE.md holds the
original brief; **this file overrides it wherever they disagree**, and after
2026-09-09 they disagree about something foundational.

**How to read this file.** "Where things stand" and "Open items" are the current state. The dated
entries after them are the last week, newest first. Everything older (the graph's recovery, the
kernel work, the measurements and the experiments that did not pay) is in
`notes/archive/HANDOFF-until-2026-10-07.md`, word for word. It is history: do not read it to start
a task. Open it only when a line here points there, or before an optimisation, to see whether it
was tried and not kept. `notes/INDEX.md` says which note settles what.

Keep this file short. Every agent reads it first, and `publish_check` refuses a file over 200 KB.
When the dated entries grow long, move the older ones into the archive the same way.

---

## Where things stand (2026-10-17)

- **It works on both systems of this laptop** (Arc 140V): Linux with Mesa, Windows with Intel's
  driver. A Vulkan layer hands each presented frame to a daemon that runs NVIDIA's network on the
  GPU and writes the answer back into the swapchain. DirectX 8-11 games go through DXVK. DirectX 12
  runs on Linux through VKD3D-Proton and is not offered on Windows.
- **The releases are Windows-only so far**: v0.0.1 (`7f9c364`), v0.0.1.1 (`f9ec5f1`), v0.0.1.2
  (`2f90064`, 10-15) and **v0.0.1.3** (`d89563f` on `newnr`, 10-18, the latest), a ZIP with the
  setup window, DXVK 3.1.1, both layers and a `vulkan-1.dll` proxy. 0.0.1.3 adds preset B in the
  setup window. It went out as a pre-release without Linux's `make test` at its head, on the
  owner's word, as a Windows-only release, and the owner made it the latest release the same
  day so that the repository's front page shows it. `master` still holds 0.0.1.2. Linux users
  build from source (README).
- **Branches.** `master` holds everything released and since (`d29b665`, PR #13). `windows` is the
  Windows line, merged into `master`. `improve-e4m3` is where new work goes, with `master` merged in
  on 10-15 (`7e7dfb5`): Codex's experimental GPU-only frame path on Windows and an E4M3 rounding
  experiment, both off by default, and what 0.0.1.2 is built from. The owner asked Paimonshen to open his in-game menu (#16) against it.
  `improve-int8` is kept for INT8 experiments; Codex's own branches are Codex's. `newnr` is the
  preset B work, made from `dx12` on 10-17, on GitHub since 10-18 at the owner's word.
- **DirectX 12 on Windows, before the upscaler** (branch `dx12`, 10-16): a `libxess.dll` in front
  of Intel's hands the daemon the frame XeSS is about to upscale, in the game's own D3D12, and
  adds NR's change to it. It runs in Hogwarts Legacy. The trail behind moving things was found
  in a recording and fixed (`e803624`), **pushed on the owner's word before anyone saw it in the
  game**: what is untested is listed in the entry below, 10-16. Carrying a change onto later
  frames is what makes the trails and patches (10-18), so the proxy now cuts the game's command
  list at XeSS and runs NR on the frame itself, all on the GPU (`mode=cut`, local): the daemon
  takes D3D12 frames (step 1, below, 10-26), the cut runs both Hogwarts recordings with no
  trail (step 2, below, 10-27), and the daemon's history follows the game's motion (step 3,
  below, 10-27). In the game, NR before XeSS gave halos and trails, so NR now runs on XeSS's
  output, before the interface (`mode=after`, below, 10-28). In Hogwarts that way the owner
  found it excellent with preset B: at full scale, and at 720p and 0.5, about 21 frames a second,
  under the game's frame generation too (below, 10-28 later). A daemon restarted under the game,
  for another preset, is taken back by the proxy 5 s later, on a new fence (below, 10-28 evening);
  in Hogwarts A to B left 5.5 s without NR, most of it the new daemon's start, and the owner saw
  NR come back.
- **NVIDIA's faster model is out** (`nvngx_dlssnr.dll` 310.9.16.0, 10-16): preset A is our model
  byte for byte. **Preset B (`hnet-crazy-cuckoo-small-fp8`) is A's graph with every stage at depth
  2**, 31 blocks against 71, with its own weights. On 10-17 it was read out of `.text` with REA,
  unpacked, and run here, on CPU and on the device, at 0.54-0.58 of A's time. Through the daemon
  its round trip is 0.5-0.8 of A's once the GPU's clock is held up (below, 10-18). That is on
  branch `newnr`, on GitHub since 10-18; `NR_PRESET=B` picks it (below, 10-17; phase72). On
  Windows the setup window takes B out of a 310.9.16 DLL and offers it (`a75e5da`, local).
  Intel's Windows driver clocks a gap-separated frame down too, B's to 865-956 MHz, and Windows
  has no hint for it yet (below, 10-18, Windows).
- **Speed on this laptop**, `src/bench/speed_test.py`, network and round trip: 640x360 at 0.4 is
  23 ms and 26-27 ms on both systems; 1920x1080 at 1.0 is 273 and 299 ms on Linux, 384 and 409 on
  Windows, where Intel's compiler loses time in the staged GEMM's loop. The README's table
  (`nr_knobs.RATES`) was measured with `live_rates.py --warmup 3`, again on 10-18 with the clock
  hint, and the default warm-up of 25 now reads the same. On Linux libxmx asks the driver for its
  low-latency clock strategy since 10-18 (`XMX_LOW_LATENCY`): without it the GPU's governor
  clocked a frame with gaps around it down to 700-1200 MHz of 1950 (below, 10-18).
- **Working rules.** Nothing goes to GitHub without the owner's word, and nothing untested at all:
  `make test` in both memory modes (`XMX_STAGING=1` is the second), CTest, and on Windows the MSVC
  build. Commits use the noreply address. NVIDIA's DLL, and anything derived from it, never enters
  the repository or a release. A Linux agent and a Windows agent work on this one laptop; the owner
  relays between them, and this file is how they hand work over.

## Open items (collected 2026-10-14, from this file and its archive)

Each line says where its evidence is: "below" is a dated entry in this file, "archive" the archive
under that date. When an item is done, strike it through here and name the commit.

**Waiting for the owner**
- Judgements in a game, pending since September:
  - Whether render scale 0.9 still looks better than 1.0 for screenshots, now that 1280x720 runs
    on the vendor's field. Archive, 09-27.
  - Whether Mortal Kombat 1 fits at render scale 1.0 now that the daemon needs 0.7-1.2 GiB. It
    was OOM-killed there on 09-16. Archive, 09-26.
  - ~~`min_extent` below 320 on a small window~~: no problems at high and low resolutions (the
    owner, 10-14).
- GitHub on 10-14: discussion #14 (a B570 in Mafia 3, and a Reddit post) has no answer. Issue
  #17 (DOOM at 3 fps with the GPU at 10 %) is answered and its GPU-path bug fixed, below, 10-16.
  For #17 the owner's reading: when the whole frame waits on the network, the game has nothing to
  render past the network's own frame rate, so its load stays low. #12 and #15 work after a reinstall, and #12 waits for the next release. #16 is
  Paimonshen's in-game menu.
- ~~The clock hint in a game (`XMX_LOW_LATENCY`, on by default on Linux since 10-18): whether it
  stays on by default~~: the owner looked on 10-18 and keeps it on. What it costs in power and
  heat, on battery above all, is unmeasured. Below, 10-18.
- Preset B against the vendor's own B: a capture on the RTX 5060 Ti with
  `DLSSNR.Hint.Render.Preset` set to B, when the owner opens that machine. Below, 10-17 later.
- The two files that came with the 310.9.16 DLL (`nvngx_dlssnr_compat-fp16-310.9.16-cuda13.4.3.rar`
  and `renodx-dlss.addon64`, in the Windows Downloads). The owner says, as their source claims,
  that the first is the DLL patched for RTX 30 and 40 cards, with FP16 kernels, and the second
  lets RenoDX pick preset A or B. Untrusted and unopened; they wait until one is needed.

**Linux**
- `dx12`'s protocol 3 (`455a6fe`), step 3 (`04c4817`) and `330ef95`: `make test` in both memory
  modes and CTest. libxmx changed in its Win32 arms and in `external_ownership`'s signature, and reads the
  device's LUID everywhere; `test_image_history.py` is new (its device check skips off Intel's
  Windows driver), and the GPU image path, the transport and the daemon's frame line changed.
  Below, 10-26 and 10-27.
- ~~**Preset B, the new model: recover its graph**, then the CPU reference against it~~: A's
  graph at depths `{2,2,2,2,2}`, running on CPU and on the device (`2bb16f6`, below, 10-17
  later; phase72).
- Preset B in a game: `NR_PRESET=B` beside A, the look and the frame rate. Below, 10-17 later.
- ~~A slight flicker of the network's own, which the owner saw on 10-18 and puts down to the
  network's low resolution~~: not the resolution. `flicker.py work/good`: what the network makes
  up on pixels the game left as they were is the same at the 320 floor and at full scale (A
  0.82 and 0.75 levels with history, B 0.39 and 0.44); B makes up half of A's and moves the
  frame half as far. The history holds it down 4x; what is left is detail drawn again where a
  pixel changed by a level or two, which the hold floor does not cover. Phase72.
- ~~Preset B's frame: the full-resolution blocks and the bottleneck are 45% of it at 1280x736.
  They are the next place to look for its speed~~: looked at on 10-18. Those passes are where
  the earlier work left them, and what was holding B's live frame back was the GPU's clock,
  which libxmx now holds up (`fe00b06`, below, 10-18). In the graph, the two bottleneck GEMMs
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
  its game on Linux too. Below, 10-14, later.
- ~~The README's live table~~: re-measured at `--warmup 3`, 10-14.

**Windows**
- The GPU's clock: whether Intel's Windows driver also clocks a light network down between
  frames, as Linux's governor did (below, 10-18). The power mode moved the host passes there
  already (10-15). libxmx's hint is Mesa's alone and does nothing on Windows.
- ~~Preset B: the setup window extracts preset A only. For a 310.9.16 DLL it could also run
  `src/tools/unpack_preset.py`, and offer `NR_PRESET` (A or B) in `nr-env.txt`~~: `a75e5da`, not
  yet in a game. Below, 10-18, Windows.
- Holding the GPU's clock up on Windows, as libxmx does on Linux: no per-process hint found.
  Below, 10-18, Windows.
- PR #20 (Paimonshen, 10-16): the in-game settings menu on Insert, in four languages, with Dear
  ImGui vendored, against `master`. The owner will take it up later.
- ~~The unmerged window attention hangs Intel's Windows driver from 32 windows up~~: found and
  avoided on 10-15, and `gpu_window_attention` is in CTest on Windows again. Below, 10-15;
  phase71.
- The staged GEMM's loop on Intel's compiler takes 1.2-1.5x Mesa's time where the graph is large.
  The packed copies took 6-9 % of it; the rest is open. Archive, 10-02 and 10-03.
- ~~Four daemon processes with Mortal Kombat 11~~: two processes are one daemon (the venv's
  `python.exe` and the interpreter it starts); four were two. The owner lock now stops a second
  one. Below, 10-15.
- DOOM (2016) through 0.0.1.2 is untested: the owner's own installation from another release is in
  its folder, and its removal was refused by this session's safety check. Below, 10-15.
- `windows_launch.py`'s Configure Steam, Restore Steam and their wrapper can go, with their tests,
  once no release with those buttons is in testers' hands. The GitHub releases have none. Below,
  10-09.
- DirectX 12 is not offered by the release. VKD3D-Proton does not run Hogwarts Legacy on Windows
  (below, 10-16); the route taken instead is the game's own upscaler, `src/xess` on `dx12` (below,
  10-16), for games with XeSS. A game that runs elevated, or any game with UAC off, never gets the
  layer; the HKLM manifest that would reach it is kept for later. Below, 10-08 and 10-09.
- ~~`dx12`: record a real Hogwarts sequence with the owner~~: recorded and replayed on 10-16,
  and the trail found in it fixed (`e803624`). Its look in the game is untested. Below, 10-16.
- MSVC: the composition is not vectorised (0.5-1.8 ms behind gcc), and `_Pragma("GCC ivdep")`
  gives three C4068 warnings that an `NR_IVDEP` macro would silence. Archive, 10-02 and 10-07.
- Housekeeping: each release root in `D:\NRonWindows\rel` holds ~440 MB of extracted weights, and
  `D:` ran out of space mid-test. Below, 10-12.

**Planned, not started** (parked by the owner: do not start unasked)
- ~~Motion vectors from the game's own upscaler, through OptiScaler~~: started by the owner on
  10-16 for XeSS, without OptiScaler: `src/xess` on `dx12`, below.
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

## DirectX 12: a daemon restarted under the game is taken back, and the fence a dead daemon leaves at UINT64_MAX (2026-10-28, evening, branch `dx12`)

**The proxy opens a new session after a broken one.** A frame that goes unanswered (no answer,
or the daemon's session failed) still ends the session, but 5 s later `record_cut` opens the
next: the old one is released to a daemon still running, and the new one's HELLO goes out as
at the start, asked again every 5 s until a daemon takes it. A preset change while the game
runs (`-Phase daemon -Preset B`) no longer needs a new game run.

**Finding: a daemon whose process ends leaves the shared fence at UINT64_MAX.** The first
version kept the fence. On the orbit recording (replay, `mode=after`, A stopped after 30
frames, B started) B answered every frame and none of its answers was used: the stamp in the
output named the frame before (frame 80: 103 against V = 105), and from A's end on the fence
read 0xffffffffffffffff in the game's process. Every wait on it passes at once, so the game's
queue no longer waits for the answer, COMPOSE finds the last frame's stamp and leaves the
frame as it was, and the daemon's work races the next frame's copy into the same buffers.
Seen with Intel's 101.9150 on Windows 11; whether a daemon that exits cleanly does the same
is untried. So the new session gets a new fence (`nr_gpu_d3d12_renew_fence`), starting where
the old one left off, so V keeps rising and no stale stamp names a new frame. The buffers stay.

**Checked here.** `test_nr_xess` has a new case: the stand-in daemon dies with its frame
(unanswered, fence at UINT64_MAX), the pause, the old session released, the HELLO unanswered
while no daemon is there and then taken, the new session's first frame answered and stamped,
its fence where the old one left off. It fails against the first version (fence at
UINT64_MAX, the answer a frame late) and against the proxy before this change. The orbit
replay, A then B: the new session was taken 5.1 s after the unanswered frame; A answered 30
frames, B 246; 13 of the 15 frames written carry their answer (frame 0 came before the
session, frame 40 in the pause), and the output lies 0.12 levels from the answer (p99 2.3)
against NR's change of 6.9, as with B alone (14 of 15); `work/tools-win/run_replay_restart.py`
runs it. MinGW CTest 67/67, the D3D12 smoke test (`test_gpu_d3d12.py`) and the MSVC build
(`/W4`, no warnings) pass.

**In the game, the same evening** (Hogwarts, `mode=after`, 1280x720 at 0.5, 101.9150 on mains,
Windows quiet). The owner started it on A: 15 frames answered a second, the daemon's frame
62 ms. `-Phase daemon -Preset B` ran under it at 22:06:41 (the auto-mode classifier lets this
phase through; it refuses `-Phase start`). The frame in flight went unanswered at 41.5, B took
the new session at 46.5, 5.03 s later, answered its first frame in 0.49 s (the graph build at
640x384) and then 21-22 a second (42-46 ms a frame, `history=moved`), none declined until the
watchdog stopped the game at 22:29. The logs cannot tell whether B's answers reach the frames,
since the first version logged every frame answered too; the owner, watching, saw NR go for a
few seconds and come back. The 5.5 s without NR: 0.7 s of the harness waiting for A to end
before it starts B, about 3 s of B's start (Python, its imports, the model in 1.6 s), about 1 s
left of the pause once B was listening, and B's first frame. B's start is estimated from A's
at the test's beginning, as the daemon's log carries no times. The watchdog put the folder back
(Intel's DLL, the XeSS folder and the game folder as before); the saves and the game's two
`.ini` files had changed with the owner's play, and stay.

**Next on Windows:** the owner's call on the gap. The pause can go: the first new session at
once, since the HELLO itself waits up to 10 s for the pipe, and 5 s only after a session that
broke before answering anything, which saves about a second. A switch with no gap needs the
daemon to load the other network beside the one it runs and change between frames, and
`nr_frame.py` and `nr_temporal.py` fix the preset at import, in code shared with Linux; that is
also why 0.0.1.3's window says its network choice takes effect when the game starts next.
Then DX12 through the release, and `dx12` to GitHub, both at the owner's word.
**Next on Linux:** as before, `make test` in both memory modes and CTest for `455a6fe`,
`04c4817` and `330ef95` (Open items); this change builds on Windows only.

## DirectX 12 in Hogwarts after XeSS: excellent with preset B, and at 720p under the game's frame generation (2026-10-28, later, branch `dx12`)

**`330ef95`: NR fell behind on what came on screen.** In `mode=after` the owner saw NR lag where
something new appeared. Where a pixel's motion was not known (off screen the frame before),
`History.follow` gave the frame itself as the game's last frame there. The composition's hold
floor then took it for a pixel the game had handed back unchanged and held the history firmly,
and the history there was the frame itself. On the orbit recording NR changed those pixels by
1.54 levels where it changes the rest by 8.30 (8.32 with the history standing still). The
game's last frame there is now `NOTHING` (-1), so the release lets go and the floor holds
nothing: NR's own answer from the first frame. Host and device agree byte for byte, and
`test_temporal.py` has a check that what came on screen composes exactly as with no history.
Its daemon checks need a Unix socket and run on Linux only.

**The game in `mode=after`**, the owner playing (101.9150, on mains). A daemon restart ends NR
for the rest of a game run (below), so each preset and its starting scale meant a new test;
the scale was changed live:

| preset | XeSS's output | `render_scale` | network field | network | daemon's frame | frames NR answered a second | the owner |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A | 1920x1200 | 0.5 | 960x640 | 103 ms | 130 ms | 7.7 | fewer halos than before XeSS; after `330ef95`, "a slight glow, but much better" |
| B | 1920x1200 | 1.0 | 1920x1216 | 202 ms | 236 ms | 4 | "now everything is excellent" |
| B | 1920x1200 | 0.5 | 960x640 | 55 ms | 79 ms | 9 | |
| B | 1280x720 | 0.5 | 640x384 | 27 ms | 46-50 ms | 21 | with the game's frame generation on: no artefacts |

- The slight glow at A and 0.5 is most likely the head drawn at half the output's extent and
  stretched 2x; at 1.0 there is no stretch. B's change is smaller (0.024 against A's
  0.037-0.052 a frame), as phase72 found off the game.
- Frame generation runs after our cut, at present, on frames that carry NR already; the proxy
  does not see it. NR still answers every real frame.
- The game was restarted twice for its settings (720p): each time the proxy hooked the new
  process and the daemon, still running, took the new session. None declined.

**Not done:** a daemon restarted while the game runs (`-Phase daemon`) leaves the proxy's session
broken, and NR stays off until the colour or the device changes; in `mode=after` the output's
extent seldom changes. The proxy should open a new session after a broken one. Done the same
evening (above).

**Next on Windows:** the proxy reopening a session after a broken one. Then DX12 through the
release (the setup window putting the proxy beside a game's XeSS), and `dx12` to GitHub, both
at the owner's word.
**Next on Linux:** `make test` in both memory modes and CTest at the head, for `455a6fe`,
`04c4817` and `330ef95` (Open items).

## DirectX 12 in Hogwarts: NR before XeSS gives halos and trails, so it moves after XeSS (2026-10-28, branch `dx12`)

**The game test**, the evening of 10-27, with the owner playing (Arc 140V, 101.9150, on mains;
A; `04c4817`, the history along the motion). The proxy hooked the list, queue and device
classes, the daemon answered every frame on its own frame with none declined, and its history
followed the game's motion on 579 of the first 590 frames. The owner changed XeSS's quality
and the daemon's `render_scale` while playing:

| XeSS | its colour | `render_scale` | network field | daemon's frame | the game, frames a second |
| --- | --- | --- | --- | --- | --- |
| Performance | 835x522 | 0.5 | 448x320 | 46 ms | about 20 |
| Ultra Quality Plus | 1478x924 | 0.5 | 768x512 | 90 ms | about 11 |
| Ultra Quality Plus | 1478x924 | 1.0 | 1536x960 | 270 ms | about 3.7 |
| Performance | 835x522 | 1.0 | 896x576 | 102 ms | about 9.6 |

- What the owner saw: slight halos and a soft picture at Performance and 0.5; fewer halos and
  trails at Ultra Quality Plus and 1.0; worse again at Performance and 1.0. With NR's change
  set to nothing at the same frame rate (the daemon's `intensity=0`), the halos and trails went.
- So they come from NR before XeSS. XeSS's temporal accumulation meets NR's detail, redrawn a
  little differently each frame at each jitter, and takes it for change it should not trust or
  noise it should average; the further XeSS upscales, the more it accumulates.
- The owner's word: NR after XeSS, before the interface.
- The proxy took no setting while the game ran (the bug below). The test stopped itself at
  00:16; the game's top, exe and XeSS folders matched the snapshot by SHA-256 (55 files), and
  the script left the owner's settings and saves as they were.

**`d3a9bcc`: `mode=after`, the default.**
- XeSS's work goes into the game's list first and the list is cut after it. The first half
  copies XeSS's output and writes the frame NR is given and the offsets for the daemon's
  history (with no jitter: the output has none). The continuation puts NR's answer back into
  XeSS's output, which the game then post-processes and draws its interface on.
- `mode=before` (or `cut`) keeps the cut before XeSS; `mode=carry` is as it was. The test
  script takes `-Mode after|before|cut|carry`, `after` by default.
- Fixed: in both cut modes the worker read `nr-xess.txt` again only when the hand-over queue
  emptied, which it never does while the daemon is slower than the game. It now reads it once
  a second.

**Checked here:** `test_nr_xess.exe` all passed, with two new cases after XeSS (R16G16B16A16 and
R11G11B10): the output the game goes on with is XeSS's plus NR's change to it, and XeSS was
handed the game's own colour. The orbit recording in `mode=after`, through the daemon (0.5) and
Intel's XeSS:
- 1920x1200 frames; 29 of the 30 written answered with a stamp. The output lies 0.21 levels
  from NR's answer (p99 3.7), against a change of 10.3.
- The network takes 102 ms at 960x640, and the replay ran at 7 frames a second.
- Frame 120 beside XeSS alone and beside `mode=before`: NR's look over XeSS's output, and no
  halo round the head against the wall (`work/orbit-cut/`, local).
- A rewrite of `nr-xess.txt` mid-replay was read within the second.

**Not tested:** `mode=after` in the game: the halos and trails, the look, the frame rate (7-9 a
second at 0.5, by the replay); a game whose XeSS output is in another format.

**Next on Windows:** Hogwarts in `mode=after` with the owner. Then the cost at the output's
extent (preset B there, or a smaller `render_scale`).
**Next on Linux:** nothing; `src/xess` is Windows'.

## DirectX 12, step 3: the daemon's history follows the game's motion, on the GPU (2026-10-27, branch `dx12`)

**`04c4817`.** The history standing still carried next to nothing onto moving things (step 2,
below), because `release` drops it wherever a pixel's colour moved: so where things moved, NR
had no history at all. Now:
- The proxy's MOTION pass (`nr_xess.hlsl`) writes, after the frame NR is given, each pixel's
  offset to where its surface was on the frame NR was given before: the game's vector at the
  unjittered position and the two frames' jitter, NaN off screen. That is TRACK's one step from
  a fresh tracker, without CLAIM, since the daemon tells an uncovered surface by the picture.
  Only when that frame came from the execute before this one and `resetHistory` is not set;
  `motion_history=0` turns it off.
- Protocol 3: the input has room for the offsets (`NR_GPU_MOTION_BYTES`), and a FRAME names
  the frame they lead back to by its V, in `driver_uuid`'s first 8 bytes
  (`nr_gpu_d3d12_follow`). The daemon follows them only back to the frame it answered last.
- The GPU image path does `History.follow` (`image_history.comp`, `gpu_image_history.py`): the
  network's last output and the game's last frame read along the offsets with the vendor's
  Catmull-Rom; the frame itself where the motion is not known, or where the frame moved here
  differs by more than STALE; no history where nothing is known; the cut against the frame
  moved here. The daemon's frame line ends `history=moved`, `still` or `none`.

**Checked here:**
- `test_image_history.py`: 32 comparisons with `History.follow`, byte for byte on Intel's
  driver. Three geometries, with crops and a smaller network extent; NaN, infinities and offsets
  past the edge; no motion; motion known only where the network reads none. A wrong STALE
  fails it.
- `test_image_pipeline.py`: the history follows only onto a history; it is dropped on a cut
  against the frame moved here and where nothing is known; without motion the history stands
  still; a new extent rebuilds. `test_gpu_transport.py`, 45: the packet rules, and offsets
  followed only back to the frame answered last.
- `test_nr_xess.exe`: two executes in a row; the second's offsets are (3.5, -2.75) exactly over
  2814 pixels and NaN off screen, and the FRAMEs name 0, then the first.
- MinGW CTest 70 of 70. The setup window's suite, 83, under a native Python
  (`D:\NRonWindows\release-check-04459d2\python-native`); MSYS2's fails one of them on `\\`
  against `/` in a path, with or without this change.
- Found on the way: `build_xess.bat` gives each pass its define from a fixed list, and MOTION,
  missing from it, was built as ENCODE. A pass without a define now stops the build.

**The orbit recording** (A, as in step 2; on battery). `work/tools-win/orbit_flicker.py`
compares each frame's change with the change to the frame before, carried along the offsets
the daemon was told, over surfaces that match within STALE: 295 pairs, levels of 255.

| history | where the colour moved by 24 or more at the same place (97 000 px a frame) | elsewhere (304 000 px) |
| --- | --- | --- |
| along the motion | 2.58 | 1.74 |
| standing still (step 2) | 3.94 | 2.34 |
| none (`temporal=0`) | 4.01 | 2.70 |

- 298 of 300 frames answered with a stamp, 296 of them following the motion. XeSS's input lies
  0.245 levels from NR's answer (p99 3.8); NR's change is 8.1 levels a frame, against 8.8 with
  the history standing still. Beside step 2's output at frame 120, no trail.
- The daemon's frame: 41.0 ms against 40.8. Its image preparation takes 4.8 ms against 3.7, for
  the two passes, a resample and two reductions.

**Not tested:** the game; Linux; MSVC's build of the daemon's side (libxmx did not change); a
game whose vectors are at render resolution (Hogwarts' are at the output's).

**Next on Windows:** `work\xess` rebuilt, and Hogwarts with the owner there.
**Next on Linux:** `make test` in both memory modes and CTest (Open items).

## DirectX 12, step 2: the cut in `src/xess`, and the orbit recording with no trail (2026-10-27, branch `dx12`)

**`61776ff`: `mode=cut`, the default** (`mode=carry` in `nr-xess.txt` keeps the path of 10-16).
- `src/xess/nr_xess_split.cpp` is DLSSNR-AMD's `nr_pe_d3d12split.cpp` at `c71b0f4`, as C++17
  behind a C API (`nr_xess_split.h`); MIT, in NOTICE and `licenses/dlssnr-amd-MIT.txt`. Stubs on
  the list class's vtable, and hooks on `ExecuteCommandLists`, `Reset` and `CreateCommandList`,
  keep each list's state since its Reset and replay it into the continuation. A list with an
  open query, a render pass, predication or a bundle it has not seen is refused, and that frame
  goes to XeSS without NR; so does the first frame after hooking.
- `record_cut` in `nr_xess.c`, at `xessD3D12Execute`: the first half copies the colour, and
  ENCODE (`nr_xess.hlsl`, `encode_proxy`'s white, knee and sRGB table) writes the frame NR is
  given into protocol 3's input buffer. Between the halves the queue signals V, a worker sends
  FRAME, and the queue waits for V + 1. In the continuation COMPOSE adds NR's change to that
  frame's colour where the stamp names V, with APPLY's arithmetic, and XeSS upscales it.
- HELLO goes out on the worker, so the game never waits for it; frames pass whole until it is
  answered, and again after each new colour extent or device. A frame left unanswered gets
  V + 1 from the CPU once its queue has reached V, and the session counts as broken until the
  next. A frame that finds 16 already pending passes without NR.
- `test_nr_xess.exe`: the twelve carry cases, the cut on its own (state carried over, the work
  between the halves run between them), and four cut cases against a stand-in daemon on its own
  queue: R16G16B16A16 and R11G11B10 against the CPU, strength 0 and a declined frame exact. The
  GPU's ENCODE differs from `encode_proxy` in 2 of 9 450 channels, by a level.
  `replay_nr_xess.exe --mode cut|carry`; in cut mode `--answers` writes what NR was given and
  its answer, out of the shared buffers.

**Both Hogwarts recordings through it** (`work/tools-win/run_replay_cut.py`: the daemon with
the GPU transport, A, render scale 0.5, network 448x320, Intel's XeSS 2.0.2; on battery in best
performance today, so the times are a guide):

| recording | frames answered with a stamp | NR's change | what XeSS got against NR's answer, mean (p99) |
| --- | --- | --- | --- |
| idle (10-18), every 4th frame written | 29 of 30 | 7.6 levels | 0.31 (5.2) |
| orbit (10-16), every frame | 297 of 300 | 8.8 | 0.235 (3.4) |

- The frames missing are the first two or three: the first after hooking, then those before
  HELLO's answer. The daemon: 38 ms a frame, the network 30-32 ms.
- Where XeSS's input lies more than 8 levels from the answer (1.6-3.6 % of the orbit's frames
  75, 99 and 147), the frame NR was given is 255: light brighter than white, which an 8-bit frame
  cannot hold. COMPOSE adds the change to the real colour, so it stays brighter than the answer
  there, red by 12-15 levels.
- **No trail.** XeSS gets the change made to that frame. Beside XeSS alone at the orbit's frames
  108 and 120 (10-18 found green fringes at 80 and 120 under carry) there is no copy of the head
  and no fringe (`work/orbit-cut/`, local).
- **The daemon's identity history carries next to nothing onto moving things**
  (`work/tools-win/orbit_ghost.py`, against a replay with `temporal=0`). Over the 27 % of pixels
  whose given colour moved by 24 levels or more, NR's change leans towards the answer before
  with a slope of 0.024, against 0.020 with no history: about 0.16 of the 41 levels it could
  carry. `release` drops it there. Where nothing moved, two answers in a row agree with a slope
  of 0.992, against 0.958 with no history.

**Not tested:** the game; D3D12's debug layer; another game or XeSS version; a refused list
outside the unit test; the first frame at a new extent, which waits for the daemon's graph build.

**Next on Windows:** `work\xess` rebuilt in `mode=cut` and Hogwarts with the owner there. Then
step 3, the history following the game's motion on the GPU, so that it steadies moving things
too instead of being dropped there.
**Next on Linux:** nothing of step 2 builds there; step 1's list stands (the entry below).

## DirectX 12: the game's list cut at XeSS instead of XeSS a frame late, and step 1, D3D12 frames into the daemon on the GPU (2026-10-26, branch `dx12`)

**The plan changes.** Before starting, the owner asked what OptiScaler and ReShade already do.
`mochizuki0323/DLSSNR-AMD` (MIT; its network is Vulkan on RDNA4) runs in native D3D12 games on
Windows as 10-18 wanted, and with no frame of delay. At the upscaler's evaluate it **cuts the
game's command list** (`windows/src/pe/nr_pe_d3d12split.cpp`, ~1 500 lines): stubs on every slot
of the list class's vtable send the game's later recording to a continuation list, which starts
with the state the list had at the cut; the hooked `ExecuteCommandLists` submits the first half
with copies into shared textures and a fence signal, the network runs on a Vulkan device of its
own between, and the continuation after. A list it cannot follow (open queries or render passes,
predication, bundles it has not seen) runs whole, without NR, for that frame. Its front end for
DLSS/FSR/XeSS games is `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass` (GPL-3.0), which calls NGX
feature 18 before the upscaler; DLSSNR-AMD answers as an NGX core and patches that fork in memory
by byte signatures for each version. Mainline OptiScaler has no NR. ReShade's add-ons reach the
queue only between submissions and at Present, after the upscaler. So **XeSS no longer runs a
frame late**: the proxy cuts the list at `xessD3D12Execute`, NR runs between the halves, and
Intel's XeSS gets NR's answer on the same frame. The cut is to come from DLSSNR-AMD under its
licence, credited in NOTICE.

**This driver shares across the APIs** (101.9150, `work/tools-win/d3d12_vk_probe.c`): Vulkan
imports a shared committed D3D12 buffer (`D3D12_RESOURCE_BIT`, dedicated) and a shared D3D12
fence as a timeline semaphore, with no wrong word both ways and the order kept on the GPU.
`D3D12_HEAP_BIT` is not importable (the query says so, and Vulkan's writes are lost).

**Step 1: protocol 3, a D3D12 client of the GPU transport** (`nr_gpu_wire.h` says it in full).
- `src/layer/nr_gpu_wire.h`: the control messages without a graphics API, out of
  `nr_gpu_bridge.h`, so that the proxy can include them; `nr_gpu_rpc.h` needs no Vulkan header
  now and takes version 3. `src/layer/nr_gpu_d3d12.h`: the client's side, for the proxy.
- libxmx: `xmx_buf_import_d3d12`, `xmx_sem_import_d3d12` (Windows enables `timelineSemaphore`
  where the device has it), `xmx_sem_wait` (on the host, with a timeout, so a client that dies
  pins nothing), `xmx_sem_value`, `xmx_sem_pass`, `xmx_external_acquire_at`/`_release_at` (the
  fence at a value; the release first writes a 16-byte stamp after the answer), `xmx_device_luid`.
- The daemon (`nr_gpu_transport.py`): HELLO names the adapter by LUID and the daemon imports both
  buffers and the fence; FRAME carries V, the value the game's queue signals after its copy. The
  daemon waits for V, answers, writes the stamp naming V, and signals V + 1, for which the
  client's queue waits on the GPU. A frame it declines, or one on a session that broke, still
  gets V + 1 once V is reached; never earlier, since D3D12 would take the fence back to V when
  its queue got there. The log calls it `transport=gpu-shared-d3d12`.

**Checked here** (Arc 140V, 101.9150): MinGW build with no warnings and CTest 65 of 65;
`test_gpu_transport.py` 43 (9 new); two new opt-in GPU smoke tests beside `gpu_win32_interop`
(`NR_ENABLE_GPU_SMOKE_TESTS`, all three pass):
- `gpu_win32_d3d12`: a D3D12 client (`test_gpu_d3d12.c`) against the daemon's transport with the
  image pipeline replaced by the frame as it came. Two sessions of four frames: seven come back
  exact and stamped, one declined on purpose comes back untouched, every fence ends at V + 1.
  With Khronos validation and synchronisation validation on the daemon: no message. The layer
  does report here: the probe's heap import gives three errors.
- `gpu_win32_d3d12_network`: the daemon itself with `NR_GPU_IMAGE=1`, 640x360 frames from the
  client through the network (320x320 field at 0.5): six answered and stamped, 82-94 % of the
  channels changed by 2-6 levels on average under A, 63-76 % by about 1 under B; the round trip
  as the client sees it, after the first frame, 26-33 ms under A (the daemon's network 25-29)
  and 15.5-16.4 under B (network 12).
- MSVC, `build_win.bat` at `928add5` (only the three known C4068): both smoke tests pass on its
  libxmx with the same numbers, and so do the 58 CTest entries that need no CMake-built fixture
  (the other 7 are those fixtures). At `/W4` libxmx has no new warning and exports all eight new
  functions, and the client header compiles with none.

**Not tested:** D3D12's own debug layer (Windows' Graphics Tools are not installed); the
motion-following history, which the GPU image path does not have yet (step 3).

**Next on Windows:** step 2, the cut in `src/xess` with DLSSNR-AMD's licence in NOTICE; step 3,
the history following the game's motion on the GPU; then the two Hogwarts recordings through the
replay, and the game with the owner.
**Next on Linux:** `make test` in both memory modes, and CTest. `libxmx.c` changed in its Win32
arms and in `external_ownership`'s signature, and it now reads the device's LUID everywhere;
`test_gpu_transport.py` has the protocol 3 cases.

## `e803624` in Hogwarts, `sync=1`, and the owner's call: NR's answer on its own frame, on the GPU (2026-10-18, branch `newnr`)

**In the game** (A, render scale 0.5, XeSS Performance 835x522). The first run's daemon came from
the checkout without its C library: a MinGW `libnr_image.dll` needs `libgomp-1.dll`, found through
`NR_DLL_PATH`, and the test's environment had none. `History.follow` then resampled in MLX-DLSS's
NumPy, 1.1 s of a 1.2-1.4 s frame, and NR landed 0.8 times a second. The daemon now says so when it
starts (`host passes in NumPy`). In that run: no trails in motion, patches on the robe standing
still. With the history standing still (`motion_history=0`, 5.4 a second): trails and patches. With
no history: worse.

**`sync=1`** (new in `nr-xess.txt`; `--sync` in the replay): the game waits for the answer to the
frame before. In the game: 12 frames a second with A, 83 ms from capture to change, every frame
answered, no wait over 1 s. Trails and patches remain.

**Where they come from** (`--answers DIR`, new in the replay). With `--sync` the daemon answers
every frame, with the motion to the one before, so its answers are NR on each frame as it is. On
the 10-16 orbit recording and today's idle one the answers are clean. What the proxy hands XeSS
differs from them over most of the background while the camera moves (frames 80 and 120, with green
fringes), and by a speckle over textured cloth standing still (a change made at one jitter, laid on
a frame at another). The trails and patches come from carrying a change onto another frame, not
from the network or the daemon's history.

**The owner's call:** frames stay on the GPU, and NR's answer goes onto the frame it was made for.
The plan, for Wednesday 10-21 (the owner put it off till the weekly limit renews): nothing inside a
game's D3D12 list can wait for Vulkan, so XeSS runs one frame late:
1. The proxy copies the colour and the vectors in the game's list, and hands NR the frame on the
   GPU (D3D12 shared resources and a shared fence, imported into libxmx's Vulkan).
2. Once NR signals, the proxy runs XeSS on the answer from its own list on the game's queue: a
   hooked `ExecuteCommandLists` waits on the fence.
3. On the next frame it copies XeSS's result into the game's output.

The cost is that the world shows one frame behind the interface. The starting point is the daemon's
GPU path (`nr_gpu_bridge.h`, `NR_GPU_ONLY`). It needs D3D12's handle types and the
motion-following history on the GPU.

## `newnr` on Windows: green, B measured, the same clock, and B in the setup window (2026-10-18, branch `newnr`)

The owner brought the Linux agent's list of 10-18; here it is in its order. Intel's driver
101.9150, mains, "best performance". The figures are in phase72's Windows section.

1. **Build and tests at `9e94be7`.** MSVC build (only the three known C4068 warnings); MinGW
   CTest 65 of 65; CTest's Python entries against the MSVC build 58 of 58; the Windows tool
   suites 183 (6 skipped: symlinks need a privilege, the Linux scripts need bash); the window's
   self-test. `test_resident.py`: B corr 0.987896 (179 passes), A 0.981917.
2. **B extracted**, the same as Linux's by content: B 281 `72669f54…`, A 649 `6ea1305b…`.
3. **Speed**, `speed_test.py`, network and round trip: 640x360 at 0.4 A 24-27 / 28-31 ms, B
   12 / 15; 1920x1080 at 1.0 A 381-404 / 406-429, B 187-202 / 212-227. B's network takes
   0.46-0.53 of A's.
4. **The clock: yes, as on Linux.** Level Zero's sysman reads it here. Back to back both presets
   hold 1950 MHz. With 25 ms between frames B runs at 865-956 MHz at 1280x720 (A at 1533-1740),
   and its round trip goes from 22-23 ms to 37-39 (A from 39-42 to 42-45). No per-process hint
   like Mesa's was found for Windows; not addressed.
5. **B in the setup window, `a75e5da`.**
   - Install NR takes B out of a DLL of 310.9.16.0 or later, beside A, through
     `src/tools/unpack_preset.py`; the release now carries it at that path. If B fails while
     the profile runs A, NR installs with A and a warning; with B chosen, B's failure stops the
     install, as A's does.
   - NR controls → Network offers A and B, B only when its weights are there. The choice is
     saved as frame pacing is: `NR_PRESET` in `nr-env.txt`, which the proxy sets in the game's
     process.
   - The speed test runs the chosen preset and says which. 310.9.16.0 joins 310.8.0.0 as a
     tested DLL.
   - Checked: the tool suites 187 (four new tests), the self-test (73 controls, 171 keys), and
     end to end from a release built here, on a stand-in game, with the real DLL. Both presets'
     weights came out equal by content; `NR_PRESET` was A, then B; the speed test ran on B
     (31 blocks); Remove NR left the folder as it was. `build_check`, `claims_check` and
     `publish_check` pass.

**B in a game, later that night** (owner: "можешь пушить. А так же надо протестировать"; `newnr`
pushed to `26c5637`). A release built from it, installed with the 310.9.16 DLL, Mortal Kombat 11
at 1280x720 and 0.4 from Steam, frames processed a second, two runs each:
- host path: A 19.5-19.7, B 18.5-20.0, though B's daemon frame is 44-45 ms against 48-53;
- GPU image path: A 22.8-23.9, **B 37.3-37.9**, with B's network at 16 ms.

Dead or Alive 5 (32-bit) on B: 22.0 a second. No frame refused, every daemon ended with its
game, every folder as before. Phase72 has the table.

**Not tested:** the window clicked through by hand (its self-test and the bridge are tested);
how B looks, the owner's to judge; B from the Linux release's `setup.sh`, which extracts A only.

**Next on Linux:** `make test` and CTest at the head. `build_release.py` and the setup window's
suites changed; the Linux runtime did not. `live_rates.py` could take a named-pipe arm, as
`speed_test.py` has; Windows used a scratch copy. **Next on Windows:** B in a game, with the
owner there.

## Preset B through the daemon, and the GPU's clock (2026-10-18, branch `newnr`)

The owner asked for B's speed. The figures, the profile and the commands are in phase72's
section of 10-18.

**Through the daemon.** `speed_test.py --preset B` (new: the test keeps no `NR_` setting of its
own environment, so it asks for the preset), network and round trip, before the clock hint:
640x360 at 0.4 is 22-23 / 25-26 ms on A and 15-18 / 19-22 on B; 1280x720 at 1.0 139-140 / 151
and 75 / 85; 1920x1080 at 1.0 270 / 298 and 152 / 175-178. Over the network field A is 11 ms
+ 118 ms a megapixel, B 4.8 + 67.

**Where B's frame goes.** At 320x320, 11.6 ms of device time: the bottleneck 3.6 (31 %), the
two full-resolution blocks 2.4. At 1280x736, 65.7: the full-resolution blocks 21-22, the
bottleneck 8.8. Those passes are where the earlier work left them: the bottleneck at 64 tokens
waits on its K loop (`notes/improve-b.md`), the window block is its element-wise work. Of what
improve-b measured and did not route, the two N = 1024 GEMMs on 32-row blocks with a 64-deep
step (0.25 ms a frame) are 2 % of B's. Not done.

**The clock.** At 640x360 and 0.4, with the gaps a client leaves between frames, the GPU ran at
1636 MHz on average while busy under A and at 1157 under B, of 1950, with no throttle reason
set: the GuC's governor reads the idle time between frames as time to spare, and the lighter
network leaves more of it. Mesa's driconf option `force_guc_low_latency` puts the kernel's
`DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT` on its queues, and with it the GPU ran at 1908-1936.
**`fe00b06`:** libxmx sets it before it makes its instance, in its own process only; a value
already in the environment is kept, and `XMX_LOW_LATENCY=0` leaves the driver's default. The
daemon's log says `GPU clock: the driver's low-latency strategy`; `test_device_pick.py` checks
the four cases. `live_rates.py --gap MS` waits before each frame, as a game takes for its own.

| `live_rates.py`, 25 warm-up frames, 30 timed | A: default → hint | B: default → hint |
| --- | --- | --- |
| 1280x720 at 0.4 | 62 → 35 ms | 30 → 22 |
| the same, 25 ms between frames | 91 → 37 | 38 → 27 |
| 640x360 at 0.4, 25 ms between frames | 33 → 28 | 27 → 23 |

Without the hint a case reads differently from run to run (the 91 was 62 in another run); with
it, a few milliseconds apart. Back-to-back frames (`speed_test.py`) do not need it, and A on
the README's table moved by at most 2 ms: the table is re-measured with it (`nr_knobs.RATES`),
its runs now 2-6 % apart.

**Not tested:** what it costs in power and heat, and whether it holds beside a game. CPU and
GPU share one power budget on this laptop. On Windows nothing changes: the option is Mesa's.

**Checked** at `fe00b06`: `make test` green in both memory modes (56 scripts each), CTest
64 of 64; `claims_check`, `publish_check` and the README's generated table pass.

**Next on Linux:** B in a game beside A, with the clock held, and the owner to judge the hint.

## Preset B read out of `.text`, unpacked and running here (2026-10-17, later, branch `newnr`)

REA 6.5.0 with Ghidra 12.1.2 on the 310.9.16 DLL. Everything below is in phase72 with its
addresses.

**What B is.** One function, `FUN_18005c5a0`, builds both networks from five stage depths in
`.rdata`: preset A `{4, 4, 6, 8, 8}` and preset B `{2, 2, 2, 2, 2}`. The bottleneck is a loop
fixed at eight ViT blocks, so **B is A's graph with every stage at depth 2: 31 blocks**. The
window phases follow `i & 3` in the encoder and `(depth + i) & 3` in the decoder. That rule
reproduces this port's table for all of A's blocks. The skips are wired by role, as for A. The
two registry entries are identical apart from the name, the id and the resource, so B uses
A's style grades. Every one of B's 77 records has the size of one of A's record families, and
none is byte-identical to A's, so B was trained separately. It has 113 342 375 parameters
(A: 145 755 123), and the bottleneck is 100.7 M of them in both. Its `blend_scale` is
0.76708984375 (A: 0.73974609375).

**The code**, local on `newnr`:
- `src/ref/nr_topology.py` (new) lays out a network from its five depths, and reads the depths
  back from a set of weight names. `src/ref/test_topology.py` is new and runs in `make test`
  and CTest.
- `src/tools/unpack_preset.py` (new) decodes each of B's records through MLX-DLSS's unpacker as
  the A block of the same role. `--preset A` reproduces all 649 of A's logical tensors exactly.
  B goes to `work/mlxw/dlssnr-b-logical.safetensors`, mode 600, local.
- `nr_model`, `nr_frame_resident` and `nr_resident` take every index from the topology the
  weights carry, so A's numbers are no longer written into them.
  **A is unchanged bit for bit**: the CPU graph against `HEAD` at 320x320 and 352x416, the
  device head against `HEAD` at 352x640, and `test_against_torch.py` bit-identical.
- `NR_PRESET=A|B` picks the weights (`nr_frame.WEIGHTS`). The daemon, the GPU image path and
  `nr_temporal.py` take the gate's ceiling from the weights (`nr_frame.blend_scale_of`), and
  the daemon logs the preset on a line of its own. `test_resident.py` also runs B's whole
  graph when B's weights are there. `frame_profile.py` has a `--weights` option.

**Measured.** On the device B tracks its CPU reference at corr 0.9870 (A: 0.9809), in 179
passes against 415. Resident, presets alternating, median of 7: 576x320 is 19.2 ms against
34.0, 1280x736 is 70.3 against 124.3, and 1920x1088 is 154.1 against 267.6, so **0.54-0.58 of
A's time** rather than half. Block 0, the last block with the head, and the bottleneck are
A's own and cost the same: 32 ms of B's 72 at 1280x736. On Dead or Alive 5's frame B moves the
picture as A does: the faces' level goes down 9-16% and level-normalised fine detail up 9-16%,
within A's range.

**310.9.16's kernels.** They are FP8 only now. 66 of the 120 are instruction for instruction
310.8's, and the rest differ by registers, scheduling, fences and addressing. Float
arithmetic changed only in the pre-block, and that change is `DLSSNR.MaskBlendMode`, the one
new hint. The ViT attention gained a `ceil(tokens / 128)` loop bound. Both changes apply to A
as well.

**`DLSSNR.MaskBlendMode`, and milder faces.** The owner asked after news that NVIDIA's October
comparisons show milder faces.
- **What the hint does.** It takes 0-3, default 0. In mode 0, as in 310.8, a game's control
  mask switches the automatic (skin) mask off. In modes 1-3 the automatic mask stays on, and
  its skin and automatic-mask strengths are averaged with, lowered to or raised to the mask's
  structure. Both DLLs default to the automatic mask off, so **the default output did not
  change**.
- **Faces, measured** on five frames with eight faces: B is not milder on faces in particular.
  Under the vendor panel's settings (automatic mask on, skin 2.0, structure 1.5) it changes the
  whole frame about 30% less than A, faces included.
- **A bug this exposed** in the `--control-mask` path. MLX-DLSS's builder put 0 in the
  automatic lanes where the vendor puts -1, and an all-ones mask, which should change nothing,
  moved the picture by two thirds of the pass. `nr_frame.make_features` now fills them as the
  vendor does and takes `--mask-blend-mode`. The daemon never fed a mask to the network, so the
  live path is unaffected. That is `f58a653`: `make test` green in both memory modes and
  CTest 64 of 64. Phase72.

**Checked** at `2bb16f6`: `make test` green in both memory modes (56 scripts each), and
CTest 64 of 64. `test_native_image.py` and `test_temporal.py` are among them, so `dx12`'s
daemon changes have now run on Linux. `build_check`, `claims_check` and `publish_check` pass.

**Not tested:**
- B in a game, with `NR_PRESET=B`.
- B against the vendor's own B, on the RTX 5060 Ti with `DLSSNR.Hint.Render.Preset`.
- The Windows side of B. The setup window extracts A only, and nothing on Windows sets
  `NR_PRESET`.

**Next on Linux:** B in a game, set beside A. Then the full-resolution blocks, now 30% of B's
frame. **Next on Windows:** run `unpack_preset.py` from the setup window for a 310.9.16 DLL,
and offer `NR_PRESET` in it.

## NVIDIA's faster model: `nvngx_dlssnr.dll` 310.9.16.0, presets A and B (2026-10-17)

NVIDIA announced on 10-13 a DLSS 5 model 24 % smaller ("roughly 118 million parameters"), with
half the GPU overhead of September's, live in NBA 2K27 on 10-16 (Hogwarts Legacy gets DLSS 5 on
11-05; the public DLSS SDK 310.9.16 of 10-16 still ships no NR, only dlss/dlssd/dlssg). The owner
supplied the DLL from that update (`nvngx_dlssnr_2026_10_16.zip`, in the Windows Downloads
folder; it goes into `work/`, never into the repository): **310.9.16.0**, 266,424,944 bytes,
sha256 `2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F`, Authenticode valid
(NVIDIA Corporation), PE timestamp 2026-09-25. First look, on Windows, with our own tools:

- **`.rsrc` holds two weight blobs.** `WEIGHTS_HT` (147,695,410 B) is byte-identical to
  310.8.0.0's: **preset A is the model this project implements**. `WEIGHTS_CC_SMALL_FP8`
  (113,893,343 B) is the new one, in the same container format (`hnet_weights.py` walks it to EOF):
  77 packed records against 153, and laid out otherwise — `block10` has layers 0-4, `block11`
  layers 0-3 of ~2.1 M elements each. (`hnet_weights.py`'s element count reads the bytes as FP16,
  the trap of phase61: the parameters are not half the bytes.) ~~So the graph is new, not only
  the weights.~~ The same block families at other depths: the entry above, and phase72.
- **Names.** Config `hnet-crazy-cuckoo-small-fp8`; the old one is `hnet-vigilant-squid`
  (`crazy-cuckoo` was a name in 310.8 already). The preset is chosen by
  `DLSSNR.Hint.Render.Preset` (`CG2RFindWeightByPreset`, `CG2RNetworkManager::ResolvePresetToDescriptor`;
  B is the shipping default). Strings from the vendor's source mention a `.cursor/rules/crazy-cuckoo.mdc`.
- **RTTI**: 35 HNetCpp classes became 40; the new ones are a buffer planner
  (`NetworkRelationAlgebra`, `DescriptorTopology`, `NetworkTopologyView`) — the layer classes
  (Swin, ViT, `CCDecInputUpsampleLayer`) are the same.
- **`.data`**: 17.2 MB of cubins and Zstandard PTX became 3.7 MB of PTX only — 15 modules, 120
  entries, `extract_modules.py` takes them out (it needs Python 3.14's `compression.zstd`; on
  Windows MSYS2's `python3.14.exe`). The layers' configuration should read off their template
  parameters, as phase3-ptx-unlock found for 310.8. `.text` grew from 700 to 873 KB.

**Next on Linux** (the owner moved this work there): REA (`github.com/morluto/rea`, an MCP
server; Node.js >= 22.19, JDK 21+, Ghidra 12.1.x, then `npx rea-agents setup` and a restart; its
Ghidra provider on Windows is an experimental read-only subset, hence Linux) to read how the
crazy-cuckoo-small network is built in `.text`: block order, connections, where the temporal
channels enter. With the container's shapes and the PTX templates, that is the graph; then
`src/ref` against preset B, and the vendor's own output on the RTX 5060 Ti with the preset hint
for ground truth. If B runs about twice as fast here too, NR lands about twice as often beside
a game, which is also what the trail in `dx12` needs.

## DirectX 12 through the game's own XeSS: NR before the upscaler (2026-10-16, branch `dx12`)

The owner asked for NR on the frame before the game's upscaler instead of DXVK/VKD3D: the scene at
render size with no interface on it, where NVIDIA's own pipeline runs the network (phase37), and
the game on its own D3D12. `src/xess/` is a `libxess.dll` that goes in front of Intel's (renamed
`libxess_nr_orig.dll`): `xessD3D12Execute`, `xessD3D12Init` and the two scale setters are its own,
the other 68 exports jump through pointers filled from Intel's DLL (`gen_xess_exports.py`; MSVC's
link.exe will not write `name=dll.name` forwarders). Settings in `nr-xess.txt` beside it, the log
in `nr-xess.log`.

**How.** The game records XeSS into a command list it has not submitted, so nothing can wait for
the network. Ahead of XeSS the proxy copies the colour for the daemon (a `WriteBufferImmediate`
marker says when it is done); a worker sends it over the layer's pipe protocol and keeps NR's
change (answer minus what it was given, in the proxy's linear light, `nr_display` encode with
white 1). Every later frame gets the newest change added before XeSS, read along the game's motion
vectors: a tracker per change follows them frame by frame back to its frame (current to previous,
pixels of the motion texture, no jitter; the picture moves by +jitter). MATCH trusts a change
where what NR was given there lies inside the current 3x3 neighbourhood's per-channel range;
REDUCE and APPLY fill the rest from the trusted change of nearby cells weighted by likeness. A new
change fades in over the frames a change takes to come. The daemon gets each pixel's offset to the
previous frame it was given (`MAGIC_MOTION`, 0x324E524E): `History.follow` reprojects its history
with the vendor's five-tap Catmull-Rom (`nr_history_sample` in `nr_image.c`, byte-identical to
MLX-DLSS's NumPy), asks the hold floor of the game's last frame moved there, and drops history
where that frame differs by more than `STALE` (0.12).

**Measured in Hogwarts Legacy** (UE 4.27, XeSS 2.0.2, flags `HIGH_RES_MV | AUTOEXPOSURE`, colour
R11G11B10 at 1130x706 for Quality and 835x522 for Performance, 1920x1200 out, velocity and jitter
scales 1): it runs; NR at render scale 0.5 lands 5-8 times a second beside the game (the network
~100 ms on the shared GPU). The proxy's warp check: at 25-38 px of motion the last frame NR had,
moved by the trackers, differs from the current one by 2.5-3 levels against 9-11 unmoved. The owner
still saw trails and halos in motion at 11:50. Found since, on `replay_nr_xess` (below): the change
of a moving object was being read onto the background it uncovered (the trackers trace that
background back to the object), first through a luma-only match that a blue and a beige surface of
one brightness both passed, then through the daemon's own history; per-channel matching, the fill
and `STALE` remove the trail from a synthetic replay (a square over a panning texture). Not yet seen
in the game.

**The trail in the game, recorded and found (2026-10-16 evening).** The owner saw it worse in
the game; a 300-frame recording of Hogwarts (the first one held only the vectors — `record_write`
mapped a whole pitch past the colour's buffer, refused; fixed and tested) showed copies of the
character's head on the wall behind it, one for each frame since the capture. NR's own answers
were clean, and so was the daemon's history: the copies were in the colour the proxy handed XeSS.
The tracker carries each pixel back one frame at a time, so background uncovered behind the head
steps onto the head on the frame before and follows it back to the capture: it takes the head's
change, and hair passes the picture's match against dark wood. Holding a surface's vector to the
last frame's does not tell them apart: in this recording the camera's own vectors change by 20-40
px from frame to frame (the game ran at 12 fps while recording). What does: CLAIM marks, on the
last frame's pixels, the vectors of this frame's surfaces that came from each; where two that move
apart by more than `spread` came from about the same place, only the one whose vector matches the
last frame's vector there keeps its tracker (the surface that was there), and the other holds
UNKNOWN. APPLY's fill now weighs cells by how alike they move as well, and fills less where little
round a pixel was trusted. On the replay the copies are gone; the cost is NR's change on surfaces
uncovered within the frames since the capture, which in a fast orbit at 12 fps is a large part of
the frame (mean change 4.3 -> 2.7 levels there, 7.5 -> 5.7 on a slow walk). After a cut the first
change now rises in over a period instead of all at once. `--inputs DIR` in the replay writes the
colour handed to XeSS, to tell the proxy's doing from XeSS's own history.

**Not tested yet** — `e803624` went to GitHub on the owner's word before any of this:
- **In the game, nothing of `e803624`**: CLAIM and the owner rule, the motion-weighted fill
  with its support floor, the rise after a cut. Evidence so far is `test_nr_xess.exe` (12 cases)
  and one 300-frame replay of one scene, recorded while the game ran at ~12 fps.
- **How much of NR's look survives at the game's own frame rate**: the loss on uncovered
  background was measured only on that slowed recording.
- **Cutscenes with many cuts**: the owner saw NR "go mad" at shot changes; the rise is the
  answer to the jump after a cut, and it has been seen only in a unit test.
- **Linux**: no test of any `dx12` commit has run there (`make test`, and
  `test_native_image.py` with `test_temporal.py` for the daemon's `MAGIC_MOTION`).
- **Other games**: the proxy has met only Hogwarts Legacy (XeSS 2.0.2, Performance, 835x522).
- **Not built at all**: a coarser fill for the background the 3x3 cells cannot reach.

**Tools.** `test_nr_xess.exe` (12 cases on the device against a CPU transcription of the shader:
formats, strength 0 exact, motion, jitter, untrusted vectors, the fade, the motion the daemon is
told, background uncovered by a moving block that looks as the block did, the rise after a cut,
a recording); `record=N` + `record_dir=` in `nr-xess.txt` record colour, vectors and jitter;
`replay_nr_xess.exe <recording> <out> work\xess-replay\libxess_intel.dll` runs them through the
proxy, a daemon and Intel's XeSS without the game (`--off`, `--strength`, `--warp`, `--history`,
`--match`); `make_synthetic_recording.py`, `replay_view.py`. The game test script is
`D:\NRonWindows\nr-game\hogwarts-xess-test.ps1` (outside the repo): it stops itself after 30 min.

**Next on Windows:** the owner to see the claim test in the game; a coarser fill (cells of cells)
for what the 3x3 cells cannot reach, so uncovered background takes the background's change rather
than none. Later: frames by GPU sharing instead of the pipe, the FFX and DLSS entry points,
OptiScaler.
**Next on Linux:** `make test`, with `test_native_image.py` and `test_temporal.py`.

## Issue #17: the GPU image path falls back instead of turning NR off (2026-10-16)

A tester's report (Arc B580, i3-12100, DOOM 2016 at 1080p, 0.0.1.2): with the window's GPU option
on, NR did nothing. The option set `NR_GPU_ONLY=1`, and the layer's shared transport stands aside
for a game whose instance asks for Vulkan 1.0 (DOOM 2016, vkcube) and in asynchronous or
every-Nth-frame mode (the tester's profile had both). Strict mode then left the game's frame alone,
and the reason went only to the game's stderr, which nobody sees on Windows. The rest of the
report: the daemon's frame 50-60 ms, 25 of it on the GPU, the GPU 38 % busy; every 2nd frame
doubled the frame rate, asynchronous changed nothing.

**`a31efe6`:**
- The window's option runs every frame in step: asynchronous mode and every Nth frame are greyed
  and kept in the profile for when it is off. It is no longer strict (`NR_GPU_ONLY=0`): where the
  shared path stands aside, NR takes its usual path.
- The layer appends its GPU-path lines (ready, the fallbacks, the GPU-only notice) to
  `NR_LAYER_LOG`, and a reason the path stands aside for good once frames take the usual path,
  so after the daemon's first lines; status and Save report list them in `gpu_fallbacks`.

Checked at the head, MSVC build: CTest 64 of 64, the wizard 79, the bridge 6, the window's
self-test (68 controls, 161 keys). Through a release assembled from the tree: vkcube with the
option, 321 frames on `gpu-image-host` and the Vulkan 1.0 reason in status; Mortal Kombat 11
through Steam, 914 frames on `gpu-shared`, none refused, the folder as before; the same with
`NR_LAYER_ASYNC=1` added by hand, 808 frames on `gpu-image-host` and the asynchronous reason in the
log. The reply on #17 is posted, at the owner's word. Not pushed.

**Hogwarts Legacy (DirectX 12, Epic, Denuvo) does not run through VKD3D-Proton on Windows either**,
as Mortal Kombat 1 did not on 10-08. VKD3D-Proton 3.0.1 with DXVK 3.1.1's `dxgi.dll`: Epic's
overlay (`EOSOVH`) makes a D3D11 device on DXVK's adapter, and Windows' `d3d11.dll` crashes. With
DXVK's `d3d11.dll` too, the layer and the daemon start, and the game crashes in its own code about
15 s in; the same without NR. The game also resets XeSS to FSR in its settings (restored). So DX12
on Windows is the `dxgi.dll` proxy (Open items); on Linux it is Proton, as for Mortal Kombat 1.

**Next on Linux:** `make test` and CTest. `nr_layer.c`'s POSIX arm gained `append_daemon_log`
(open with `O_APPEND`, one write, close), which only the GPU-only notice uses there.

## 0.0.1.2 built and tested on Windows; the window attention hang found and avoided (2026-10-15)

The owner relayed the Linux agent's list of 10-14 and answered its questions. On `improve-e4m3`,
pushed to GitHub at the owner's word:
- **`7e7dfb5`**, `master` (`429855c`) merged in. The three conflicts as the list asked: both of
  `nr_daemon.py`'s new functions, both groups of `notes/INDEX.md` rows, and this file on
  `master`'s structure with `improve-e4m3`'s entries of 10-12 to 10-14 placed by date.
- **`1a146c2`**, Linux's four red tests. `test_device_pick.py` takes Mesa's "is not offered for
  this queue family". The GPU image path's parity tests hold the bytes on Intel's Windows driver,
  where they were proved: libxmx records the device's `VkDriverId` (`xmx_driver`), and
  `xmxres.image_path_exact` asks for 5. Elsewhere they are skipped with the reason, and a new
  host check, run on any driver, holds that the default daemon takes the host path and never
  builds the GPU image pipeline. Checked here with the id forced to differ. **Next on Linux:**
  `make test` in both memory modes and CTest.
- **`4932958`**: `test_setup_proxy.py` failed on Windows: an 8.3 TEMP against the long names
  Profile resolves to, and MSYS2's Python, which `setup.bat` refuses as `NR_PYTHON` (no NumPy or
  safetensors); the batch tests are skipped with the reason under such a Python.
- **`584f1fa`**: 0.0.1.3 is 0.0.1.2, as the owner numbers it.
- **`2f90064`**: no stray Chinese characters in the notes, the owner asked. PR #13 brought a
  section heading, two references to it and two names of the contributor's own files in Chinese;
  they are English now. `docs/WINDOWS-PORT.md`'s two mojibake examples are said in words. The
  Japanese path names in `test_windows_launch.py` stay: they are its Unicode fixtures.
- **`1ef43e0`**: the unmerged window attention, below.

At `2f90064`: MinGW CTest 63 of 63 (64 of 64 with `gpu_window_attention` at `1ef43e0`); against the
MSVC build CTest's Python entries 56 of 56 (57 of 57 at `1ef43e0`), the wizard 79, launch 40, release 19, `setup_proxy` 11
(the native venv), `build_check`, `publish_check`, `claims_check`, and the layer's `spawn_reasons`,
`vulkan_proxy`, `spawn_exit`, `pipe`, `alloc`; the window's self-test, 68 named controls and 161
keys in English, Russian and Spanish. `NR_E4M3_MAGIC` is 0 and the GPU path off, by default.

**The release**, `D:\NRonWindows\rel\dlss-nr-windows-0.0.1.2-x64.zip`: `deploy.bat --release` at
`2f90064`, DXVK 3.1.1 from the pin's cache, 299 entries, 19 805 166 bytes, SHA-256
`df930e0d805250cc79fb3ad8eebb9bf11b786005186fc843f5f4bf164b00bf87`. No NVIDIA DLL, no weights, no
Chinese. The window attention change is not in it, as the list asked. Not tagged, not published.

**From the ZIP, through Steam's Play button, Arc 140V, 101.9150**: a first build at `584f1fa`,
Mortal Kombat 11 624 frames and Dead or Alive 5 (32-bit, the 32-bit proxy) 554, the weights
extracted on the first Install NR; the final one, Mortal Kombat 11 767 frames. Each with none
rejected, Save report with no warning, the daemon ending with the game, Steam letting go, and the
game's folder as before after Remove NR. **DOOM was not run**: its folder holds the owner's own
installation from `D:\NR Test\dlss-nr-windows-7f9c364`, which 0.0.1.2 will not install over. The
owner agreed to remove it; this session's safety check refused the removal twice, so it waits for
the owner's Remove NR in that release's window. The folder's NR files are backed up in
`nr-game\backups\doom-owner-install-2026-10-15`.

**The four daemon processes of 10-09.** Listed with their parents, a daemon is two processes: the
venv's `python.exe`, whose parent is the game, and the interpreter it starts. Four were two
daemons; `nr_daemon_owner.py`'s lock now refuses a second one on the same pipe.

**The power mode.** On 10-14 and the night of 10-15 Windows ran on mains in "best power
efficiency" (the owner found it); on 10-12 it was "best performance". The host passes run slower
there, the same in this release and in a build of 10-12, so it was not the code: Mortal Kombat 11
gave 14-16 processed frames a second against 19-20. `quiet-check.ps1` prints the mode: put it beside
every number.

**The GPU image path in a game** (the window's option, `NR_GPU_ONLY`), Mortal Kombat 11 at
1280x720, render scale 0.4, processed frames a second over 40 s, the runs interleaved: in power
efficiency, off 14.1 and 15.7, on 22.7 and 23.0, every frame through `gpu-shared`, no fallback;
in best performance, off 18.8 and 18.7, on 23.5 and 23.7 (+26 %), every frame through `gpu-shared` again. The path leans far less on the CPU's clock. The daemon's frame falls from 60 to 37-38 ms there, the network staying at 31-33.

**Speed test of the release, 101.9150, best performance**, network and round trip in ms, two runs
(the table of 10-12 was 101.9033; keep them apart):

| window | 0.4 | 1.0 |
| --- | --- | --- |
| 640x360 | 25 / 28 | 45-46 / 50 |
| 800x450 | 24-25 / 29 | 73 / 79 |
| 1280x720 | 33-34 / 41 | 166-170 / 179-183 |
| 1920x1080 | 61 / 76 | 371-426 / 396-452 |

**The unmerged window attention, found and avoided** (`1ef43e0`, the owner: try it on Windows). On
101.9150 the test still lost the device. Case by case, each in a child the parent kills after a
timeout: the unmerged variant alone never hangs (to 512 windows, 300 submits); two of it, in one
submit or two, or it and the merged one in either order, lose the device about 4 s in, with the
bias and from 32 windows (6 and 16 do not), at any scale of the data; two merged ones do not. No
scratch memory in either (the driver does not show its code). Only the unmerged variant, with the
bias, loads a float32 cooperative matrix from global memory and stores float32 accumulators straight
to global memory. It now stores through shared memory and plain vec4 stores, as the merged one did:
the same bytes (48 cases, five runs), no hang in any case, and the graph with
`NR_FUSE_ATTENTION_MERGE=0` passes. `gpu_window_attention` is in CTest on Windows again.
`notes/phase71`. Merged, the default, is unchanged.

## `improve-e4m3` (Codex) on Linux at `20cba61`: the default path unchanged, four tests red (2026-10-14, later)

Checked in a detached copy, `work/e4m3-check`, with `f3220fd`'s test fix applied; Codex's own
worktree untouched. It builds with no warnings, and both layers load. With the GPU path and
`NR_E4M3_MAGIC` off, as by default, it is `master` on Linux: heads `e62005b8…` / `c217fd2f…`
(changed `e7c72789…` / `95f46f67…`), the replayed graph and the live table the same within noise.

CTest 58 of 62. `make test` stops at its first line, `test_device_pick.py`:
- **`gpu_device_pick`**: `XMX_QUEUE_PRIORITY=high`, alone and with the compute family, expects
  libxmx to report the priority. Mesa does not offer high for these families, and libxmx says
  so ("not offered for this queue family; the driver's default"). The test wants Windows' answer.
- **`gpu_image_preprocess`, `gpu_image_compose`, `gpu_image_pipeline`**: the GPU image path is
  not bit-exact on Mesa. Preprocessing is 1 ulp of float32 off on a few values, composition up to
  0.00049, and the full model's answer bytes differ. The shaders are `precise` throughout, and
  `image_preprocess.comp` already notes that `OpFDiv` may be inexact; Mesa lowers division to a
  reciprocal and a multiply. The path is Windows-only and off by default, but the tests run here.

For Codex: make the image shaders exact on Mesa, or hold their tests to the driver they were
proved on, and let `test_device_pick.py` accept "not offered". Merging `master` in brings
`f3220fd`, and a small conflict at the top of this file, where both lines changed it.

## Steam on Linux: the daemon ends with its game; the README table re-measured (2026-10-14, later)

**Steam's reaper waited for the daemon.** Steam starts a game under `reaper`, which makes itself
the subreaper and waits for every descendant, a `setsid` one included. Steam's own
`ubuntu12_32/reaper`, with `work/test_present` through the layer and `NR_LAYER_SPAWN=1`: the
stand-in game ended after 0.6 s, the reaper still waited 20 s later, and it returned 0.02 s after
the daemon was stopped. So a game started from Steam stayed "running", with the model in memory,
until something stopped its daemon. The Linux layer now marks the daemon with the game's process
id, as Windows' does, and `nr_daemon.end_with_game` watches it through a pidfd. The same run
returns as soon as the game ends, and the daemon's log says why. `test_spawn_exit.py` runs on both
systems (make test, CTest).

**The README's table**, re-measured with `live_rates.py --warmup 3`, medians of six: the same
within a millisecond, except 1024x768 at 0.55, 56.5 -> 51.5 ms.

## Local GPU frame path and Windows E4M3 check (2026-10-14)

The tested Windows continuation (`codex/gpu-frame-path`, through `dfb2143`) is
integrated into `improve-e4m3` at the owner's explicit request.
GPU preparation/composition/history and authenticated shared Vulkan resources
are implemented behind an experimental window option, off by default. The same
Arc SDR every-frame synchronous path passed actual Win32 swapchain/full-model
smoke on **101.9150** for API 1.1 and 1.3: 24 presents, 22 GPU frames, resize,
trigger and scale controls, zero synchronization-validation errors. Correct
private command-buffer loader dispatch initialization was required. Owner lock,
primary surface policy, transfer reuse and timeout-safe cleanup have CPU tests.
No tag, release or real-game FPS gain is claimed.

`notes/gpu-frame-path-20261013.md` records the limits and remaining checks.
Current-driver image parity passes; rotating 25-warm/30-sample benchmarks now
cover full 1080p/1440p. At 1440p/0.4 CPU/GPU-host medians are 179.59/122.14 ms;
at full 1440p, 786.39/743.07 ms. These exclude game capture/IPC/return and are
not FPS. Unchanged settings uploads are cached. Forced-fence full-image smoke
also passes with controls and synchronization validation. A paired memory-policy
probe found cached DEVICE memory slower; that experiment was removed. The full-
resolution network gap remains unexplained. Final daemon outage/restart passed:
original frame retained, then compatibility host-image fallback, zero sync errors. Shared mode
renegotiates after device/game restart. Next: real game A/B and B580 measurement.

The GPU window option now selects `NR_GPU_ONLY=1`: unsupported or failed shared
processing retains original GPU frames without legacy pixel capture/processing.
Final GPU reductions return compact statistics. See `notes/gpu-only-20261014.md`
for strict failure checks and complete 1080p/1440p present measurements.

Separately checked remote `improve-e4m3` at `40cb8e5` on 101.9150: zero mismatches
over 1,114,875 inputs, exact full/changed network heads, GEMM/frame regressions
pass. Alternating warmed contexts showed about 9.6% slower paired time at 320
and neutral 720p. Full 1080p/1440p remain exact but are about 10.1%/22.4% slower.
Its source is retained on this branch; keep the compile-time experiment off.

The owner authorized this branch publication; later push/merge/release needs confirmation.

## On Linux at d29b665: green after a test fix, the speed test, and the warm-up (2026-10-14)

`make test` stopped at `test_setup_proxy.py`: `Profile` resolves the interpreter, and here
`sys.executable` is a symlink (`python3` -> `python3.14`). The test now compares with the
profile's. Then: `make test` green in both memory modes (578), CTest 48 of 48, test-proton, and
the Windows tool suites (147, 19 skipped).

`speed_test.py` here, network / round trip, ms. At 0.4: 640x360 23/25.7, 800x450 23/27.3,
1280x720 29/35.5, 1920x1080 52/63.0. At 1.0: 40/44.6, 61/67.0, 140/152.0, 273/298.9. Windows'
table of 10-12 matches up to 800x450; at 1.0 from 1280x720 it is slower (170, 384).

`live_rates.py`'s new warm-up of 25 frames (PR #13) reads this laptop slower: with `--warmup 3`
the table is 10-07's (640x360 at 0.5 26-28 ms, 854x480 31-35), with 25 29-34 and 38-45; 512x288
and 1080p do not move. Not the code: compare with the README table at `--warmup 3`.

## Small E4M3 probe; remaining work deferred (2026-10-13)

Owner requested the cheapest task now and the rest after limits reset.
Branch improve-e4m3 now also has a compile-time NR_E4M3_MAGIC experiment, default 0,
and frame_replay --shader-dir for variant comparisons. 1,114,875 probe inputs
match, and complete-graph original/changed head hashes match at 320x320 and 720p.
Short local Linux timings improve 3–5%, including reversed 320 ordering.
Candidate GEMM-contract and frame-execution regressions pass. This is not a B580,
Windows or game-FPS measurement; default activation awaits independent-driver checks.
See notes/improve-e4m3-probe-20261013.md for reproduction and the queued work:
B580/Windows E4M3 validation, FFN+attention C32 fusion, GPU pre/post with
shared game resources, then dependency-driven multi-block execution.

## PR #13 reviewed on Windows (2026-10-13)

PR #13 is merged into remote `master` at `d29b665`. The independent continuation branch
also contains Claude's local `2658b24`, `471de6e` and `f8afcf4`; those three commits remain
included in this publication. Combined checks pass: device/queue selection, graph execution, CLI installation,
raw geometry, 75 wizard tests (one platform-specific skip), and the WPF self-test.

The continuation reviewed `Paimonshen/perf-recheck` at `1fc7aa7`, rebased onto `041ef3b`.
The MSVC build reaches all six stages. The raw writer avoids per-image ImageMagick and
supports sampling; its measured overhead must still be reported separately from inference.

Two corrections were required before integration:
- raw images now carry their own geometry. Readers reject interrupted metadata publication,
  including a same-area resize in a legacy dump directory. PNG conversion verifies the image
  before removing raw data, and retains legacy sources whose original geometry is ambiguous;
- CLI proxy installation reuses the window's ownership, placement, rollback and UTF-8
  environment functions. Reinstallation preserves the original loader, foreign or changed
  files are refused, and GUI Remove NR recognises the CLI installation. Multiple executables
  require `--exe`; 32-bit games use the window.

Checked in an independent checkout: full MSVC build and 44 CTest entries; the documented
`gpu_window_attention` driver hang is excluded as `docs/WINDOWS.md` requires. MinGW builds
successfully, and seven relevant native/GPU/installer CTest entries pass. The final metadata
failure regression and raw PNG checks were repeated after the last correction. No game or
inference-speed benchmark is claimed by these regression tests.

## The network's GPU, frame pacing in the window, and the daemon's queue (2026-10-12, later)

At the owner's word ("можем пушить") `196c8d1..041ef3b` went to `windows` and `master`, after
the Windows suites and the window's self-test ran again at the head. Then the first three steps
toward more frames on desktop cards that the owner agreed to, from a look at the other ports
(AMD's among them: the network before the upscaler, an asynchronous residual, the game's own
device, a high-priority queue). Local, not pushed:

- **`2658b24` The network runs on a GPU that can run it.** `xmx_init` took the loader's first
  device. It now takes one with the float16 8x16x16 cooperative-matrix shape at subgroup scope
  and `VK_KHR_workgroup_memory_explicit_layout`, a discrete card first; an older Intel iGPU, a
  card of another vendor or a virtual device listed first is passed over. Frames reach the
  daemon through host memory, so a game on another card can have its network on the Arc. With
  none able, the loader's first non-CPU device, as before, and the log says what it lacks.
  `XMX_DEVICE` names one, by index or part of its name; with several devices the daemon's log
  lists them. `src/gpu/test_device_pick.py` puts sixteen machines in front of `xmx_pick_device`.
- **`471de6e` Frame pacing in the setup window**: Asynchronous (`NR_LAYER_ASYNC`) and "Network
  on" every 1st-4th frame (`NR_LAYER_LIVE`), on the NR controls tab. The profile keeps them; a
  bridge action, `pacing`, rewrites only those two lines of an installed game's `nr-env.txt`,
  which the layer reads when the game starts. vkcube's stderr shows the layer taking both.
- **The daemon's queue**: `XMX_QUEUE_PRIORITY` (low to realtime, `VK_KHR_global_priority`) and
  `XMX_QUEUE_FAMILY=compute` (the family without graphics: Intel's compute engine) or a number.
  Off by default, since they bought nothing here: below.

**Mortal Kombat 11 through Steam, 1280x720, render scale 0.4 (network 512x320), Arc 140V on
mains, 101.9033, quiet before and after; processed frames a second over 40 s, two runs each,
the configurations interleaved** (`nr-game\queue-game-series.ps1`):

| | default | high | compute | compute + high |
| --- | --- | --- | --- | --- |
| in step | 19.75, 19.90 | 19.65, 19.22 | 19.59, 19.47 | 19.42, 19.79 |
| asynchronous | 19.47, 19.72 | 19.05, 20.12 | 19.07, 19.12 | |

Nothing outside the spread of single runs. The daemon's frame is 50 ms, 33 of them the network
on the GPU and the rest host passes, and in step the game waits on the present rather than
competing; asynchronous overlaps only the game's few milliseconds. Without a game the speed
test gives the same times in all four queue configurations. A discrete card, where the game's
frame and the network's are closer, is where asynchronous and the priority could show: the
next measure belongs to a B580. (The daemon logs a frame's time to 10 ms, so its median reads
40 or 50 for the same 45; the processed rate is the measure.)

**Next on Linux:** `python3 src/gpu/test_device_pick.py` — ANV must report the shape (else the
daemon now says "trying device 0 ... although it lacks"), an llvmpipe in the list is passed
over, and above medium `XMX_QUEUE_PRIORITY` is likely refused without `CAP_SYS_NICE`, which the
test accepts and the log says. `make test` for the rest. The window is Windows' alone.

## A speed test on a tester's first run, kept for their reports (2026-10-12)

The owner asked for a short measure of frame time on a first run, saved where reports gather it,
for the user to be told what NR does at best, at render scale 1.0 as well as 0.4, and for the
window to say that capturing the frame and the rest take time too. `2afb9e8`, `7cc281e` and
`c15f491`, pushed 10-12:
- `src/bench/speed_test.py` starts the daemon as the layer does and sends it synthetic frames,
  one connection a frame, at 640x360, 800x450, 1280x720 and 1920x1080, at 0.4 and at 1.0. For
  each it keeps the network's own time (the middle of the daemon's `gpu in+run+out`, from its
  log for the same frames) and the round trip a game waits for. Fifteen frames after five a
  case: nine after three left 1280x720 at 38 ms in one run and 50 in the next.
- The setup window runs it after a first installation, and from a **Speed test** button. It is
  refused while a game with NR runs. The window shows the network's time and the frame rate it
  allows, a line a render scale, in three languages, and why a game's frame rate is lower.
- `work/speed-test.json` holds it with the GPU, driver, CPU, memory and power; Save report
  carries it whole, round trips included, and the status a summary.

On the Arc 140V, on mains, network and round trip, ms:

| window | 0.4 | 1.0 |
| --- | --- | --- |
| 640x360 | 23 / 26.9 | 41 / 44.5 |
| 800x450 | 24 / 28.2 | 66 / 71.4 |
| 1280x720 | 31 / 39.1 | 170 / 182.4 |
| 1920x1080 | 54 / 69.8 | 384 / 408.6 |

About 22 s; at 0.4 alone three runs in a row stayed within 5 %. Through the window by UI
Automation, from a release built at `c15f491`: Install NR, then the test, then its lines.

**Next on Linux:** `speed_test.py`'s Unix-socket arm has never run. One `python3
src/bench/speed_test.py` in a checkout with weights, set beside `live_rates.py`'s table.

## Issue #12, further: the layer runs in the tester's DOOM and starts no daemon, silently (2026-10-12)

The tester's game is not elevated, by Task Manager, and elevated it fails the same way; the report
names `DOOMx64vk.exe`, the right executable. So the cause is not either one below. Windows' loader
said what it did when asked: the game started from cmd with `VK_LOADER_DEBUG=error,warn,info,layer`
and `2> file`, which works for a windowed game too (the loader links its C runtime statically, so
its stderr is the handle the process starts with). Checked here on a console and a windowed probe
(`nr-game\loader-log-test.ps1`, `loader-log-gui-test.ps1`). Their 6316 lines show:
- loader 1.4.350 honours `VK_LAYER_PATH`, inserts `VK_LAYER_dlssnr_intel` into the instance and
  the device, and the layer runs: `[nr_layer] active; socket=\\.\pipe\nr_windows_d716f055`, a
  1920x1080 swapchain;
- **no daemon was started and nothing said why.** Once `NR_LAYER_SPAWN` asks, `ensure_daemon` had
  three quiet ways out: no `NR_LAYER_SOCKET` (it was there), an `NR_LAYER_SPAWNED` mark, or a
  daemon already answering on the socket. No `no daemon at` lines: NR was off in that run;
- above ours three implicit layers (two OBS hooks, WeGame's and GamePP's, and CapFrameX's
  overlay), and broken ones: Epic's overlay, Bandicam's 32-bit one, and
  `D:\DLSS5-Demo-Bundle`, geladons/DLSS5_INTEL in HKCU, whose library is under
  `C:\Users\AI\Desktop\...`. That port talks to its daemon over TCP 47990 and never sets
  `NR_LAYER_SPAWNED`.

Asked the tester for `set NR` and `set VK` and whether a `python.exe` of ours runs (10-11).

**`0a4ce03`, pushed 10-12:** each of the three ways out now says so on stderr (`[nr_layer] no daemon
started: ...`), so the same capture names it; `NR_LAYER_SPAWN` unset stays silent. The proxy takes
an inherited `NR_LAYER_SPAWNED` out of the game's own process. `VK_LAYER_PATH` names the manifest
itself, and the loader stops reading the installation's records as manifests. Tests:
`test_spawn_reasons.py` (new, CTest on Windows) makes one instance through the layer for each
way out and for a start, with a stand-in daemon; `test_vulkan_proxy.py` checks the mark. From the
release's ZIP: Mortal Kombat 11 539 frames and Dead or Alive 5 850 through Steam's Play button,
none rejected, folders as before; the probe's loader log shows the one manifest searched.

**Two things found on the way.** Mortal Kombat 11 showed four daemon processes on 10-09, likely
two daemons, since a virtual environment's python.exe starts the interpreter as a second process.
DXVK makes more than one instance, and one made before the first daemon's pipe exists would start
its own. Not checked; both would end with the game, the second costing memory. And `D:` ran out of
space mid-test: the release roots in `D:\NRonWindows\rel` each hold ~440 MB of extracted weights.

**Next on Linux:** `make test` and CTest for the layer's new lines (`nr_layer.c`'s POSIX arm
prints the same three reasons). Nothing else Linux runs changed.

## Raw dumps and performance measurements (2026-10-10; reviewed 2026-10-13)

PR #13 replaces per-frame ImageMagick encoding with raw RGB capture, adds sampling and
converts captures afterwards. The original measurements and their methodology are in
[windows-dump-measurements-20261010.md](windows-dump-measurements-20261010.md). Equal rounded
medians cannot establish zero dump overhead; compare throughput over matched steady-state
intervals and report frame-time percentiles. The review adds per-frame geometry so resizing
cannot silently reshape older raw captures, and validates PNGs before removing raw files.

## The first Windows releases and tester diagnostics (2026-10-09)

The v0.0.1/v0.0.1.1 release record, elevation tests and original issue #12 diagnosis are
preserved in [windows-first-releases-20261009.md](windows-first-releases-20261009.md). The
later issue #12 entry above supersedes the original inference that the layer never loaded.

## NR whatever starts the game: a vulkan-1.dll beside it, and the setup window without Steam (2026-10-09)

The owner could not install NR for Mortal Kombat Komplete Edition, a game outside Steam: a Steam
configuration left by an earlier test locked the profile (`steam_profile_lock`). The request
that followed was for every game started outside Steam or from another store. The layer is
found and configured through environment variables, and only setup's own Launch game, or Steam
launch options naming its wrapper, used to give them.

**`598ccbc`: a `vulkan-1.dll` beside every game** (`src/layer/nr_vulkan_proxy.c`). Windows looks
for `vulkan-1.dll` in the executable's folder before System32, for a game's own import and for
DXVK's `LoadLibrary` alike. In `DllMain` the proxy reads `dlss-nr\nr-env.txt` and, in the process
of the executable named there (compared by volume and file index, not by spelling), sets those
variables and records the launch in setup's `launch-state.json`. Every one of the loader's 265
exports goes on to System32's loader, loaded on the first call and outside `DllMain`, or to the
game's own copy, which install sets aside. `vulkan_proxy_gen.py` writes the wrappers from
`vulkan-1.exports` and the SDK's prototypes; x86 exports undecorated stdcall names through a
`.def`. `DISABLE_NR_PROXY=1` turns it off. `build_win.bat` builds both architectures, releases
carry them, and `layer_vulkan_proxy` is in CTest (Windows).

**Games through the proxy, nothing set in Steam**, setup's backend as its buttons call it, NR on
for 40 s (release built from `cd150e7`):

| game | started by | API | layer | NR frames | rejected | folder after Remove NR |
| --- | --- | --- | --- | ---: | ---: | --- |
| Mortal Kombat 11 | Steam's Play button | DX11 through DXVK | x64 | 370 | 0 | as before |
| Dead or Alive 5 Last Round | Steam's Play button | DX9 through DXVK | x86 | 783 | 0 | as before |
| Mortal Kombat Komplete Edition | its own executable | DX9 through DXVK | x86 | 586 | 0 | as before |
| DOOM (2016) | Steam's Play button | Vulkan | x64 | 388 | 0 | as before |

Every daemon ended with its game, and Steam's RunningAppID was 0 after each. Through the window
itself, from the unpacked `7f9c364` ZIP and clicked by UI Automation: Komplete Edition from its
executable 1256 frames, Mortal Kombat 11 from Steam's Play button 766, none rejected. With the
game in front and nothing clicked, the window started no process in 40 s. Komplete Edition's own
ASI loader rewrites `asiloader.log` at every start; it was put back from a copy.

**Found on the way, `cd150e7`:**
- Status never saw the game: the proxy writes its launch record into `work/windows-wizard` and
  makes no folder, and nothing had made it yet. Install makes it now.
- DXVK opens its first log before it loads `vulkan-1.dll`, so before `DXVK_LOG_PATH` is set, and
  in the folder the game was started in. Steam starts Mortal Kombat 11 in its root, above the
  executable, and Remove NR left `MK11_dxgi.log` there. The proxy lists that folder in
  `dlss-nr\start-folders.txt`, and Remove NR deletes the DXVK logs written there after the
  installation (by time, with 2 s for FAT).

**Two more things the games showed:**
- Once DXVK frees its first instance the proxy can be unloaded, and a later
  `LoadLibrary("vulkan-1.dll")` gets the loader of that name already in the process, System32's
  (MK11's module list, and a probe with the DLL search path). So all the proxy does, it does at
  its first load.
- Steam starts DOOM's `DOOMx64.exe`, which starts `DOOMx64vk.exe` when the game's renderer is
  Vulkan (`r_renderAPI 1`); the proxy works in the second.

A game that loads System32's loader by its path, or keeps its DLL search to System32, never loads
the proxy; Enable NR then warns that the game runs without the layer, and Launch game still
starts it with the variables. A game with anti-cheat may refuse a DLL beside it: the quick start
says to test single-player games offline.

**`7f9c364`: the setup window without Launch mode, Steam App ID, Configure Steam and Restore
Steam.** Launch game starts the game directly. Install NR now returns Steam launch options an
earlier setup left, as Remove NR does, and they no longer lock the profile.
`configure_steam`, `restore_steam` and the wrapper stay in `windows_launch.py` for those leftovers;
they can go, with their tests, once no release with Configure Steam is in testers' hands. The
window's self-test: 55 named controls, 131 keys in English, Russian and Spanish. Docs: the release
quick start says to start the game as usual, and `docs/WINDOWS.md` records the proxy.

Before these, from the owner's own session with the window: `925c21f`, Remove NR returns Steam's
launch options itself and Enable NR says when NR cannot reach the game; `9d472ba`, the window
compact, with a dark theme, sliders on the arrow keys, and each control's description once, as
its name's tooltip.

The ZIP is `D:\NRonWindows\rel\dlss-nr-windows-7f9c364-x64.zip` (256 files, 18.5 MB, SHA-256
`0172a087…2aa2`), with no weights and no NVIDIA DLL. `9f2070c` to `7f9c364` were pushed the same
day, and `7f9c364` is v0.0.1 (the entry above).

**Next on Linux:** the Windows tool suites at the head: `test_windows_wizard.py` and
`test_release.py` changed, and `test_vulkan_proxy.py` is new (it skips off Windows). Nothing
Linux builds or runs changed: the proxy, `build_release.py`'s part and the window are Windows'.

## The setup window through two games, and a daemon that ends with its game (2026-10-08, evening)

The window clicked through by UI Automation, from a release unpacked from its ZIP (`9f2070c`),
Steam mode, NR on for 40 s:

| game | API | layer | NR frames | rejected | Restore Steam after the game closed | game folder after Remove NR |
| --- | --- | --- | ---: | ---: | --- | --- |
| Mortal Kombat 11 | DX11 through DXVK | x64 | 784 | 0 | at once | as before (10 409 entries) |
| Dead or Alive 5 Last Round | DX9 through DXVK | x86 | 809 | 0 | at once | as before (963 entries) |

Install took the weights from the user's own DLL. Configure Steam and Remove NR went through
their dialogs. With the game in front and nothing clicked, the window started no process in 40 s.

**Found on the way: Steam kept a closed game running, because of NR's daemon.** The layer starts
the daemon from inside the game, and Steam counts every process a game starts as the game. After
MK11 closed, RunningAppID stayed 976310 for as long as the daemon ran, and cleared 2 s after it
was stopped. So Restore Steam refused ("A Steam game is running"), and the model stayed in memory.
The backend's game runs never showed it, because their script stopped the daemon first.
`9f2070c`: on Windows the layer puts the game's process id in `NR_LAYER_SPAWNED`, and the daemon
ends when that process does (`nr_daemon.end_with_game`). A daemon started by hand runs on as
before. vkcube through the MSVC layer: 90 frames, and the daemon gone 0.1 s after it. CTest 39 of
39, on MinGW and on MSVC's DLLs (`layer_spawn_exit` is new).

**Two traps in driving the window this way:**
- Any UI Automation call into the WPF window brings it to the front, even a tab selected (checked
  with a stand-in window in front). After "Enable NR" is pressed through UIA, the window is in
  front and polls, as it is after a user's own click. Only a stretch with nothing clicked shows
  whether polling stops behind the game.
- The message boxes' Win32 buttons reach UIA from PowerShell as panes with no patterns, so the
  test sends the dialog WM_COMMAND IDOK. A screen lock does not stop UIA, but a window that was
  active when the screen locked stays active, and polls.

**`geladons/DLSS5_INTEL`** (MIT) ported this tree's layer and credits it. It has what we lack on
Windows:
- a `dxgi.dll` proxy for DX12, live on GTA V Enhanced. Present is hooked by patching the swapchain's
  vtable entries in place, because GTA V kills a process whose swapchain vtable pointer changes.
  BattlEye blocks a `dxgi.dll` beside the game;
- a PE scan that reads the delay-load directory (UE5 loads `d3d12.dll` that way);
- a pause hotkey.

Its DX9 install is `d3d9.dll` alone, because DXVK's `dxgi.dll` crashed GTA IV there. The proxy is the
route to DX12 here, after the release.

**Next on Linux:** nothing runs differently there, since the POSIX layer still marks its child
with 1. But the Linux release's `launch-nr.sh` sets `NR_LAYER_SPAWN=1` too, and Steam's reaper may
keep a game running while its daemon lives. Worth a look through Steam. If it does, the marker can
carry `getpid()` and the daemon watch it with a pidfd, as the Windows side now does.

## On Linux at 0513b60: green, the Linux binaries unchanged, and the DXVK pin is DXVK's (2026-10-08)

`make test` green in both memory modes (571), CTest 45 of 45 with no warnings, `make test-proton`
loads both layers. The five Windows tool suites pass here too (126 tests, 17 skipped for Windows).
`nr_image.c` and `nr_layer.c` compile to the same machine code as at `5fec502`, so the speeds
measured there stand. The Linux release, assembled and installed: metadata `"dxvk": null`, the
host library with F16C, and its layer and daemon answered 124 presents. `dist-tools/dxvk.json`
matches the archive on DXVK's GitHub release (18 041 512 bytes, the same SHA-256), and
`fetch_dxvk.py --archive` runs on Linux too.

## The Windows release, prepared: a lighter setup window, Spanish, DXVK and 32-bit games, Remove NR; no DirectX 12 (2026-10-08)

Seven commits on `windows` after `f1175bb`, for the first GitHub release. Arc 140V, 101.9033.

**The setup window's load** (`aed08e5`), measured with a stand-in process as the launched game:
- The hidden busy bar's indeterminate animation never stopped: 5.9 % of a core with the window
  idle. It animates only while an action runs: 0.2 %.
- WPF draws in software: 30 MB of GPU memory and 45 MB of RAM less (179 -> 130 MB).
- The status check every 5 s started Python, and once a game was launched Python started a
  PowerShell for WMI's whole process list: 0.45 s of CPU in four processes each time. The game is
  found by its PID now (0.11 s in two), and the window checks only while it is in front.

**Spanish** (`8a50654`): the third language, every string; the self-test takes its languages
from the window's list.

**DXVK and 32-bit games, and Remove NR** (`1980e5e`, `2bf9d6f`, `4d21118`, `b8a8a65`):
- A release carries `nr_layer32.dll` and DXVK 3.1.1, pinned by version and SHA-256 in
  `dist-tools/dxvk.json` and fetched by `scripts/fetch_dxvk.py` (`deploy.bat --release` runs it);
  its zlib licence is `dxvk/LICENSE` and in NOTICE.
- The window takes a 64- or a 32-bit game and installs the layer for its architecture. For
  DirectX 8-11 it puts DXVK beside the game; a game file of the same name is set aside in
  `dlss-nr/game-backup`, recorded in `dlss-nr/game-files.json`.
- **Remove NR** deletes NR's files and puts the set-aside ones back; a file changed since is left
  and named; it waits for Restore Steam. DXVK logs into the release's `work/logs` for NR
  launches; Remove NR deletes only the DXVK logs that appeared after the installation.
- The layer started its daemon with `DETACHED_PROCESS`. With a virtual environment's python.exe,
  which Prepare Python makes, that put a console window over the game: DOOM, full screen, lost
  the focus and stopped after 16 frames. `CREATE_NO_WINDOW` now.
- Restore Steam waits up to 15 s for Steam's RunningAppID to clear after the game has gone; it
  refused right after every exit before.

**DirectX 12 is not offered.** VKD3D-Proton 3.0.1 with Mortal Kombat 1: Windows' DXGI gave
E_NOTIMPL creating the swap chain; DXVK's DXGI with Windows' D3D11 crashed in d3d11; all of DXVK
crashed in the game after it asked its D3D11 device for ID3D12Device. Intel's Vulkan driver has
the extensions VKD3D-Proton needs (mutable descriptors, descriptor buffers, GPL), so the stop is
in the D3D11/DXGI interplay, not in the driver.

**Games, through the release, launched by Steam, NR on for 60 s:**

| game | API | layer | NR frames | rejected | game folder after Remove NR |
| --- | --- | --- | ---: | ---: | --- |
| DOOM (2016) | Vulkan | x64 | 423 | 0 | as before |
| Mortal Kombat 11 | DX11 through DXVK | x64 | 942 | 0 | as before |
| Dead or Alive 5 Last Round | DX9 through DXVK | x86 | 1040 | 0 | as before |

These ran the setup window's backend as its buttons call it (`scripts/windows_wizard.py`).
Through the window itself, clicked by UI Automation with Mortal Kombat 11, the fields, the API and
launch-mode choices and Install NR worked and put DXVK beside the game; the screen locked (04:25)
before Configure Steam, so that run stopped there and the installation was removed through the
backend. The window's polling behind a real game is still to be watched; with a stand-in it
started nothing while minimised. Captures of DOOM and MK11 with NR on are in
`D:\NRonWindows\nr-game\release-tests` (local, not for publishing).

**Cleaned up on the way:** Codex's DOOM installation of 10-06 (`DOOM\dlss-nr`, from before there
was a Remove) is in `D:\NRonWindows\nr-game\backups`; Dead or Alive 5's Steam launch options still
ran Codex's test wrapper of 10-04 and are empty again, as before that test. The DirectX 12 test
overwrote Mortal Kombat 1's `vkd3d-proton.cache` (13.5 MB from Proton); Proton rebuilds it.

**Next on Linux:** `make test` and CTest at the head. `scripts/build_release.py` and
`test_release.py` changed; the Linux release path is untouched, and the rest is Windows-only.

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

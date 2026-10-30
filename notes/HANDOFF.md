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
  game**: what is untested is listed in the entry below, 10-16.
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

## PR #20 narrowed to the panel: one Windows-only file, one keyboard reader (2026-10-26, branch `menu-on-e4m3`)

The maintainer looked at five rounds of fixes and proposed the opposite of a sixth: shrink the PR.
Nearly every defect had come from four parts a keyboard-driven panel does not need, so those went.

- **One panel, keyboard only.** The hand-drawn renderer is deleted - `nr_menu.c`'s font, glyph
  atlas, composite pass and cursor - and so is the mouse path and the cursor pin. What stays is the
  state machine the ImGui panel's input goes through, and `nr_menu_draw_overlay`, which is how the
  ImGui image reaches the frame. `nr_menu.c` 1756 -> 1395 lines.
- **One keyboard reader.** Keys were read in `nr_layer.c`, `nr_menu.c` and `nr_menu_ui.cpp`, so the
  focus gate had to be remembered in three places and two of them forgot. `nr_layer.c` now sweeps
  256 levels once per frame behind one gate; `nr_menu_set_key_levels` hands them to the state
  machine and `nrex_key` to ImGui. Zero `GetAsyncKeyState` outside one macro.
- **Windows only.** The menu's glue (`menu_composite`, `menu_read_keys`, `menu_build`, the mouse)
  moved to `nr_menu_layer.c`, built only on Windows; `nr_menu_layer.h` gives the layer three empty
  hooks elsewhere and `nr_layer_state.h` holds the two structs both files need. `nr_layer.c`
  3920 -> 2255 lines, against 2102 on `newnr`. Linux gets no ImGui, no `libstdc++`, no STORAGE on
  a swapchain - verified by configuring with `WIN32` off: zero menu sources in every `build.make`.
- **One network setting.** The `weights` knob, its model table and the manifest are gone;
  `NR_PRESET` maps to one file per preset. This is what fixed `NR_PRESET=B` running A on an install
  whose B had never been extracted, and "network preset A" logged beside a B model.

The architecture is in `docs/MENU-ARCHITECTURE.md`; this is the state and the traps. The one worth
carrying forward: **a check that cannot see the file the defect lived in is not a check.** An
assertion for "exactly one keyboard reader" read three files and not `nr_menu.c`, and the mutation
that put a reader back there passed.

---

## PR #20: the maintainer's review answered, the menu in a real Windows game (2026-10-18, branch `menu-on-e4m3`)

The maintainer reviewed the in-game menu on 10-18 with four asks before merging #20. All four are
answered here, in his order.

**1. Raw mouse input.** `nr_raw_mouse_start` registered the mouse over the game's own registration
and `nr_raw_mouse_stop` removed it, so a WM_INPUT game had no mouse after one panel open. The
game's registration is read with `GetRegisteredRawInputDevices` before ours goes in and put back,
target window and flags, when ours comes out; removed only when there was nothing to restore
(`981a519`).

**2. Hotkeys while another window is in front.** INS, L, Home, S and R were `GetAsyncKeyState`
polls with no focus gate, so an L in a chat window rewrote the settings file and Home in an editor
turned NR off in the game behind it. L now also requires the panel open - a language write goes
around "Save is the only thing that writes", so it is a settings action. Each latch is re-armed
while gated, so the first press after returning to the game acts (`981a519`).

**3. Rebased onto `newnr`** (his third ask, and the one that carries preset B): 30 commits over
`newnr`'s tip, one conflict in `build_release.py` where both sides add files to the release list;
kept both. A full build, 203 Python tests and the C fixtures are green from the rebase (`ef643f5`
lineage). **4. Authorship:** all 30 commits re-authored to
`137417967+Paimonshen@users.noreply.github.com`, dates and messages unchanged.

**What the first real launch found**, and what it cost:

- The daemon died at start-up with `xmx_low_latency` not found. `build_win.bat --layer` skips the
  libxmx step, so the fast path kept the pre-rebase DLL while the daemon came from the rebase and
  expected the new symbol. A full build regenerates the export list; the function answers false on
  Windows, where the low-latency hint is Linux Mesa's. (`f785182`)
- **The setup window's bridge lives in `scripts\`, and only the packer copied it there.** Every
  change to the wizard's backend was verified against `src\tools` and tested there while the
  window kept running the packer's old copy - which reads exactly like the feature being broken,
  and produced `weights must be one of ht, cc_small_fp8` from a Reset with the tree already
  correct. `sync_release.bat` now copies `windows_wizard.py`, `windows_wizard_core.py` and
  `windows_launch.py` and clears a stale `__pycache__` beside them. (`d67dbfd`)
- The cadence combo opened showing `*Unknown item*` as its fourth entry: the row declared four
  options with a store of three names, so the terminator was written over an entry the count still
  counted. Second time this shape has cost something, the first being the profiles combo;
  `test_menu_choices` now asserts the layer's cadence row and every choice knob the bridge
  produces, mutation-tested with the buggy shape. The panel defaulted to 560x380, which fit eleven
  knobs and not fourteen rows, and dragging the border past the swapchain image is an access
  violation: the default is the picture minus a margin clamped to 640x720, and the window is
  NoResize with a scrollbar for the case where the picture is smaller. (`7eff40a`)

**One network, one chooser.** The setup page had a second network dropdown beside the controls
page's network knob, and the two disagreed the first time they were used together. The setup card
is gone; the knob saves into the settings file, which the daemon reads live and at start-up - the
start-up read is new, so a user who chose B last session comes up on B rather than loading A and
swapping on the first settings refresh. A Reset of the knob writes null, and null returns to the
model the daemon came up on. (`1e3ddb3`, `ef643f5`)

**The model table.** Preset B has been loadable since the topology started coming from the weights
themselves; the manifest's "not loadable yet" was stale. `cc_small_fp8` is supported, its logical
file is `dlssnr-b-logical.safetensors` - the name `PRESET_WEIGHTS["B"]` and the installer both use,
so `NR_PRESET=B` at start-up and the panel's knob read one file - and B unpacks through
`unpack_preset.py`, whose role-renaming decoder is the only one that can read B's records.
(`f48912d`)

**Still to do, and it is not the menu's logic:** the wizard's own test suites are non-deterministic
under Windows when several files run in one pytest process. Both files pass alone and pass in a
trio; in the larger combination the failures wander between runs and every traceback is the same
thing - `TemporaryDirectory.cleanup` raising `WinError 145`, a directory not empty, over
`release (candidate)\dxvk\x64` and friends. The failures move, they move with the teardown errors,
and no product assertion is among them: re-running the same command that failed passed 106. The
shape says a handle is still open at cleanup time - the fixtures' own `shutil.copy2` of a DXVK file
into the game folder, or the antivirus, and the assertion failures are unittest reporting both the
error and the abort. A check after a `TearDown` failure is not a check that passed, so this is
filed here rather than assumed away.

**The GPU row also turned on strict mode, which is the bug issue #17 removed** (2026-10-18, same
branch). Found by writing a check for the four variables the panel writes rather than assuming
they were what their names say. `write_pacing` wrote `NR_GPU_ONLY` as a copy of
`NR_GPU_IMAGE`/`NR_GPU_TRANSPORT`; but `NR_GPU_ONLY` is the **strict** half of the path, not the
same switch: with it on, `host_pixel_path_allowed` refuses the CPU pixel fallback and the game's
own image is kept, with the reason going only to stderr — which on Windows is nobody's. That is
precisely the tester's report that became issue #17 (DOOM at 1080p, "with the GPU option on NR did
nothing"), and `a31efe6` answered it by making the window never set strict mode: "Never strict:
where a game cannot keep its frames on the GPU, NR takes its usual path and the layer's line in
the log says why". The panel had put the strictness back, so **turning that row on could make the
effect stop working** in exactly the games the fallback exists for — and the row that did it is
the one promising more speed. `NR_GPU_ONLY` is now written as `0`, always, and deliberately not
omitted: an install that had it on is cleared by the next pacing write.

**Four behaviours, each pinned by a check that is mutation-tested.** Driving the bridge directly
(a real `nr_menu_bridge`, real files, no menu) rather than reading it:

| Behaviour | Check | Mutation that must fail it |
|---|---|---|
| a row moved writes `nr-env.txt` at once | "a row moved writes the file with no Save pressed" | drop the `write_pacing` call |
| Save does not touch pacing | "Save leaves the pacing file byte for byte" | add `write_pacing` to `nr_menu_bridge_save` |
| Reset does not touch pacing | "Reset leaves the pacing file byte for byte" | add it to `nr_menu_bridge_reset` |
| a restart reads back what was written | "a restart reads back the cadence that was written" | (round trip; guarded by the reader) |

Each mutation was applied, built, and run: all four go red. **One of them first passed for the
wrong reason** — "Reset leaves the pacing file unchanged" was green because the Reset never ran,
not because it left the file alone, and the injected mutation made no difference. The check now
proves Reset ran first, by its preview file (`nr_settings.json.live`), and only then that the
pacing file is identical. That is the second time in this work a check passed by doing nothing;
the shape is worth remembering.

The other two mutations on the table are for the two Reset/Save paths that a headless bridge
cannot reach: the menu's `nr_menu_take_reset` / `nr_menu_take_save_request` branches in
`nr_menu_bridge_update`. Mutating those does **not** fail the checks, because the fixture has no
`nr_menu` — so what is covered is the headless functions, which the menu branches call, and the
shared body they run. Said plainly rather than implied.

## `nr-env.txt` is the panel's, and the profile follows it (2026-10-20, branch `menu-on-e4m3`)

**Two writers, one file, no ordering.** `windows_wizard_core.set_pacing` rewrote the pacing lines
of the game's `nr-env.txt` from the profile, and `nr_menu_bridge_set_pacing` (the in-game panel)
wrote the same lines from what the user changed. Neither knew about the other, so a change made in
the panel and then any pacing save in the setup window silently reverted it. Measured on this
machine before the fix: the file said `NR_GPU_IMAGE=1 NR_LAYER_ASYNC=0 NR_LAYER_LIVE=1` while the
profile still said `gpu_image_path=false nr_async=true nr_every=4` — the window's checkbox and the
panel's row describing different settings with equal confidence.

**One owner, and it is the panel** (the owner's rule). It is the only writer that can see what the
layer is actually running; the profile is a record of what the user asked for, not of what is
happening. So:

- The panel writes the file **and** the same three values into `work/windows-profile.json`, editing
  that file in place so the game, the Python, the DLL and the launch options survive
  (`write_profile_pacing`). Keys a profile from before lacks are added inside the object, not after
  its closing brace — a profile the window cannot parse is a window that cannot open.
- `set_pacing` writes **only the profile**. It no longer touches the file, so the revert is gone
  rather than merely reordered.
- `NR_ROOT` in `nr-env.txt` is how the panel finds the profile; a release without one keeps working
  and simply has no profile to keep in step.

**The GPU row now says why it is not running.** `gpu_image_compose.fallback_reason` declines a
frame whose `detail_strength` or `colour_strength` is not 1, and that sentence went only to the
daemon's log — the row read "on" while every frame was composed on the CPU, which cost 0.05-0.07 s
a frame against 0.014-0.016 s on the shared path (measured here, same scene). Same shape as issue
#17: a reason nobody can see. `nr_menu_bridge_pacing_gpu_blocker` returns that sentence in the
panel's language while the path is on and one of the two strengths is not 1, and the row's hint
becomes it; off, there is nothing to explain.

**Not fixed, and it is a feature rather than a bug:** the two strengths themselves are still
CPU-path-only. `compose_detail` is `source + colour*lowpass(change) + detail*highpass(change)` with
a radius-4 Gaussian separable blur, and the GPU composer has no blur pass, so porting it means a
25-tap two-axis convolution, an intermediate buffer and a bit-exact rounding story. Until then the
honest thing is the sentence above, and it is what the panel now says.

Checks: `test_menu_pacing` grew the profile-projection block (the profile carries the three values,
the file and the profile agree, keys the panel does not own survive, a profile without them gains
them and still closes), the blocker block, and "the real installation is reachable again" after the
odd-shape override — that last one exists because the override is process-wide and leaving it set
made two later blocks fail for a reason unrelated to what they tested. Mutation-tested by removing
the profile write, by forcing the blocker to never fire, and by dropping the brace in the append
path; each turns the suite red. The three wizard tests that asserted the old "`set_pacing` writes
the file" behaviour now assert the new contract instead.

## PR #20 round three: the plan, and the architecture it is aimed at (2026-10-23, branch `menu-on-e4m3`)

The maintainer's second review listed 13 items. Two documents now exist for it, and neither is a
promise that the work is done:

- **`notes/ROADMAP-PR20-ROUND3.md`** — the order, the dependencies and the verification gate for
  each stage. Five stages, key path 0→1→2→3→5, stage 4 free to interleave.
- **`docs/MENU-ARCHITECTURE.md`** — the first description of this subsystem *as* a subsystem:
  two UIs over one data authority, the C/C++ ABI boundary, the three input paths, the two files
  and their two readers, and the localisation generator. `docs/ARCHITECTURE.md` covers the network
  itself and never covered this layer.

**Three things the investigation settled that the review did not state:**

1. **The build failures have one cause, and it is not a missing stub.** Five tests
   (`test_device_teardown.c:25`, `test_exchange.c:8`, `test_gpu_only.c:22`, `test_settled.c:6`,
   `test_transfer_resources.c:3`) `#include "nr_layer.c"` rather than linking the library, so they
   inherit every menu symbol the layer calls. `test_gpu_present` already links `nr_layer` via
   `add_dependencies` — the pattern exists, those five did not follow it. So the fix is to link the
   menu sources, not to write no-op arms.

2. **But the no-op arms for `nr_menu_ui.cpp` do not exist, so linking is not enough on Linux.**
   That file's `menu_ui_keys` (L1316-1339) uses `GetAsyncKeyState`, `ToUnicode`, `BYTE`/`SHORT`/
   `WCHAR` and its timer uses `LARGE_INTEGER`/`QueryPerformanceCounter` (L647-651), all without a
   platform gate. The claim in `nr_layer.c` that the menu headers "bring their own no-op arm" is
   true of `nr_focus.h`, `nr_raw_mouse.c`, `nr_menu_font.c` and `nr_menu.c`'s key functions, and
   false of `nr_menu_ui.cpp`. Gate first, link second.

3. **A dispatch layer to decouple the menu is not worth it.** `nr_layer.c` calls into it at 89
   lines from L2125 to L3310, and several of those pass C structures rather than scalars
   (`nrex_row rows[]`, the `nr_menu_strings[lang][key]` tables). Rejected in the roadmap with the
   count, so the next person does not re-propose it.

Also recorded there: the pacing conflict (item 6) is the consequence of last round's
"one owner" decision and is resolved by the window writing both files again, which works because
`write_profile_pacing` keeps the profile current; and the base-branch item (13) is not a rebase —
the local branch is already identical to `upstream/newnr`, so it is a pushed-state and PR-setting
problem.

**All thirteen items are now done, in six stages on `menu-on-e4m3`.** The roadmap is what was
done; the architecture document still describes the tree, defects included, because the parts of
it that were wrong are worth keeping as the record of what was wrong.

- `3f5e7b6` stage 0 — the build links again. Five fixtures `#include "nr_layer.c"` and were given
  no menu sources; CMake and the Makefile now link what `build_win.bat` always did. Also
  `nr_menu_ui.cpp` compiles off Windows (its `menu_ui_keys` had no platform gate at all),
  `test_focus_gate` runs in the Windows build for the first time, and the CTest entry pointing at
  a `test_menu_keys.py` that does not exist is withdrawn.
- `d953809` stage 1 — the 32-bit crash. Three defects, not the one reported: no `VKAPI_PTR` on the
  GPA typedef, `nrex_image`/`nrex_image_view` returning `void *` where a Vulkan non-dispatchable
  handle is `uint64_t` on x86, and a loader context of `void *` slots holding `VkDevice` and
  `VkInstance`. Fixed and pinned by negative assertions in `test_menu_keys.cpp`.
- `7e5b187` stage 2 — the input path. The raw-mouse `cbSize` (24 against 16 on x64, so the game's
  registration was never saved and the stop path stripped its mouse every time), plus the
  `(UINT)-1` truthiness and the missing `ERROR_INSUFFICIENT_BUFFER` retry. The focus gate on
  `nr_menu.c`'s own reader, which had none at all, and `nr_menu_release_keys` so the gate does not
  cost the first press after alt-tab. Item 7 is documented as impossible rather than fixed.
- `ba8571a` stage 3 — the window writes `nr-env.txt` again, so its frame pacing reaches an
  installed game. Safe now because `write_profile_pacing` keeps the profile current; four test
  assertions flipped.
- `99cae31` stage 4 — the Russian cadence (27 bytes against a 24-byte store, and three plural
  forms where one `{}` cannot choose), the lost newline at `nr_layer.c:88`, the proxy's
  non-appending and unconditional launch trace, and the hardcoded `F:\Downloads` path.
- `984754d` stage 4b — the panel's Network row shows `A: HT …` / `B: CC small FP8 …` instead of
  `ht` / `cc_small_fp8`, with the raw name still what gets written.
- `5b46fd2` stage 5 — the PR's base moved from `master` to `newnr`, and all 57 commits re-authored
  to `Paimonshen`; the tree is byte-identical to before the rewrite.

**One trap worth carrying forward.** `nr_menu_extra_key_for` returns `NR_MENU_KNOB_COUNT +
position`, so its hand-written name list must be in the generated table's order. The choice labels
were first put at the END of that list while the generator emits them in the middle, and the
panel showed the GPU-blocker sentence as its two Network options - every entry a valid string, so
nothing was blank and no existing check could see it. `test_menu_choices.cpp` now asks for that
key by name; verified by putting the wrong order back.

**Two more the verification pass found, both fixed here.** They are worth recording because neither
failed anything, and both are the same shape as the bug above - a value that is wrong but valid.

- **The choice-name store was 32 bytes and every label is 33 to 75.** `NR_KNOB_CHOICE_NAME_SIZE`
  was sized for the raw names (`cc_small_fp8` is 12) and then started holding a label. The panel
  showed "A: HT, the model this project r", and the Chinese one was cut inside a 3-byte character
  and rendered as a replacement glyph. Nothing crashed, nothing was written wrong. It is 96 now,
  and the check measures bytes rather than characters - the English label is 41 characters and 40
  bytes, the Russian 43 and 75, which is the same trap the cadence row had.
- **`nr_menu_extra_key_for`'s prototype was in the GENERATED header.** Running
  `tools/gen_menu_strings.py` deleted it, leaving an implicit declaration in `nr_menu_bridge.c` that
  MSVC warns about (C4013) and links anyway. It is in `nr_menu_strings_extra.h` now, which the
  generator does not write, and the sequence that matters - regenerate, then build - is what was
  checked.

Both were found by a teammate reading the code rather than by a test, which is the point of doing
that pass at all.


## The panel spoke two languages over a four-language enum (2026-10-21, branch `menu-on-e4m3`)

Reported from a game with the panel set to Español: the three frame-pacing rows, the GPU row's
refusal message and their tooltips were all in English, beside eleven knobs that were translated.

`nr_layer.c` selected them with `nr_menu_language(...) == NR_LANG_ZH ? "chinese" : "english"` —
eleven sites, including the four cadence option names. The language enum has four values
(EN/ZH/RU/ES), so Russian and Spanish fell through to the English arm. They are table keys now
(`async`, `async_hint`, `gpu_image`, `gpu_image_hint`, `every`, `every_hint`, `every_frame`,
`every_nth`, and the three `gpu_blocked_*` reasons), generated for all four languages from `OWN` in
`tools/gen_menu_strings.py` and read through `nr_menu_text` like the knobs. The cadence options are
formatted from `every_nth`'s `{}`, so a language with a different word order needs no C.

**The generated header gained `extern "C"`.** The tables live in a `.c` file and the menu's UI is
C++, so a C++ caller looked for `?nr_menu_text@@YAPEBDHH@Z`, which nothing defines. It linked only
because every caller had been C; the first C++ fixture to touch the table failed to link, which is
how it was found.

The fixture now walks those eleven keys through all four languages: each non-empty, none still
equal to the English row, and every_nth keeps its `{}`. Mutation-tested by copying the English row
into Spanish (`1 of 11 still English in es`) and by blanking a Russian row.

**A translation that was behind its English, which no check can see.** The setup window's
`GpuImagePathTip` said, in English, Russian and Spanish, that where the shared path cannot be used
NR "takes its usual path, and Save report says why". The Chinese still described the behaviour
issue #17 removed — "the effect is skipped and the game's original frame is shown" — because it was
written before `a31efe6` and never revisited. A Chinese reader was told the option disables NR when
it in fact falls back, which is the opposite of what happens. Aligned to the other three.

The lesson is not the string: it is that **a check for "is every key translated" cannot see a
translation that is merely out of date.** Comparing values catches a copy of the English row;
nothing mechanical catches a Chinese sentence about behaviour that changed underneath it. English
and the other languages carry the same risk, and this one was found by reading all four side by
side, which is what the pacing rows made worth doing.

## Two bugs the first run in a game found (2026-10-21, branch `menu-on-e4m3`)

Both were in the work above, and neither was caught by the checks that shipped with it.

**Writing the profile ate the commas.** `write_profile_pacing` replaced a member by copying the
line up to the key and then `": %s"`, dropping whatever followed the value — including the comma
that separates it from the next member. The result was

```
  "nr_async": false
  "nr_every": 1
```

which is not JSON. The setup window then could not read its own profile and refused to launch the
game with `nr_every must be a whole number from 1 to 4` — a message that names neither the file nor
the panel that wrote it. Found when the owner asked for a relaunch at 800x600 and the launcher
failed. Restored from the backup taken before the session, and fixed by keeping the tail of the
original line past the value.

The check that should have caught it only compared key VALUES, which were all correct; nothing
asserted the file was still JSON. It now counts members and commas in both the replace path and
the append path, and the written file is confirmed against the real Python parser. Mutation-tested
by restoring the comma-eating `snprintf`, which turns it red.

**Reset put the network back to A.** `load_defaults` gives a choice knob index 0, and
`CHOICES["weights"]` is `("ht", "cc_small_fp8")` — so Reset moved a user who had chosen B onto A,
the slower model, with the row simply showing the other name. Seen here as a Reset taking `weights`
from `cc_small_fp8` to `ht` alongside detail 1.75 -> 0.75 and intensity 2.0 -> 1.0, with the
daemon's next start logging `network preset A, 71 blocks`.

The network is a choice about *what runs*, not how much of it is applied, so Reset leaves it
(`load_reset_defaults`); every picture parameter still returns to its default. The owner's call,
and the right one. An install carrying one network behaves exactly as before.

**What this says about the earlier checks.** The profile bug shipped because the tests compared
values and never the file's shape; the Reset bug shipped because no test ever pressed Reset and
then looked at the network. Both are the same mistake in different clothes — a check that
exercises the thing you changed rather than the thing that broke.



## Frame pacing's three rows: not Save's, and the settings file could hide them (2026-10-18, branch `menu-on-e4m3`)

Three questions asked of the panel's pacing rows — do Save and Reset govern them, does Save leave
a file, does what it left take effect at the next start — answered by driving the bridge directly
rather than by reading it, because two of the three answers are about files.

**Save and Reset do not govern them, by design.** The three rows (asynchronous, GPU image path,
every Nth frame) are the *layer's* start-up settings and live in the proxy's `nr-env.txt`; the
knobs and Save live in `work/nr_settings.json`. `nr_menu_bridge_set_pacing` writes `nr-env.txt`
**the moment a row moves** and never touches the settings file, with the reason in the code: "there
is no Save path for them, so a change that was not written would be lost and the panel would be
lying". Measured:

- **Moving a row writes immediately.** No Save press needed — one `set_pacing` call rewrote
  `NR_LAYER_ASYNC=1` and `NR_LAYER_LIVE=3` while `nr_settings.json` stayed byte-identical.
- **Save leaves `work/nr_settings.json`** and writes nothing to `nr-env.txt`: the pacing rows are
  already in their own file.
- **Reset leaves both alone.** It returns the knobs to defaults on screen and in the preview and
  deliberately does not write the saved file; `nr-env.txt` is not its business, so a pacing change
  stands through a Reset. Coherent — they are next-launch settings, not previews — but it does
  mean Reset is not an undo for them.
- **The next start really applies them.** The proxy reads `nr-env.txt` and calls
  `SetEnvironmentVariableW` for every line; the layer reads `NR_LAYER_ASYNC` / `NR_LAYER_LIVE` /
  `NR_GPU_IMAGE` once before the first present. Verified on the real installed game: its
  `nr-env.txt` holds `NR_LAYER_ASYNC=1` and `NR_LAYER_LIVE=4`, which is the panel's "on" and
  "every 4th frame".

**What was wrong: the three rows could vanish for a reason unconnected to them.** `read_pacing(b)`
was called from inside `load_settings`'s `if (nr_settings_parse(...))` branch. The pacing values
are in `nr-env.txt`; the settings file is the daemon's and unrelated. So a settings file that was
absent, empty, or half-written left `pacing_ok = 0`, `nr_menu_bridge_pacing_available` returned
false, and the panel drew **no pacing rows at all** while `set_pacing` refused to write — the three
settings that decide how the network is fed became unreachable because an unrelated JSON file was
missing. Install always writes that file, so a normal installation never showed it; a release whose
`work/` had been cleared, or a settings file a tool truncated, did.

`read_pacing` now runs first in `load_settings`, unconditionally, before the settings file is even
opened, and `pacing_ok` depends only on whether `nr-env.txt` was located and read — the only thing
it ever meant. `test_menu_pacing` gained three checks (a missing settings file leaves the rows
readable and writable; an empty one likewise), **mutation-tested** by putting the call back inside
the parse branch, which reports all three failing with `available=0`.

## Model swap took the GPU transport down with it (2026-10-20, branch `menu-on-e4m3`)

The owner reported NR failing with the GPU image path on. The log named the failure exactly:

```
frame rejected/failed; game keeps original: GPU frame has no matching imported session
[nr_layer] GPU transport fallback: frame exchange failed; original frame retained
[nr_layer] GPU-only: CPU pixel path disabled (shared processing unavailable); original GPU image retained
```

The image engine wraps a `ResidentBackend`, so a new model means a new engine — and
`close_gpu_images` closed the **whole transport** with it. But the transport's `contexts` are the
**game's** imports: buffers, semaphores and an authenticated process lifetime established by a
HELLO the layer sent. Closing them leaves the layer holding handles to objects the daemon has
already given back, so its next frame has no matching session, `gpu_blocked` latches for the
session (it is cleared only when the reply said "unsupported"), and NR never processes again. In a
release built before the strict-mode fix, the player then sees the game's own frame — which is the
combination this machine was running. **The panel's network chooser was the trigger**: it is the
only thing that changes the model mid-game.

Two halves, because either alone is wrong (`1b26be3`):

- `close_gpu_images(keep_sessions=True)` drops the engine, leaves the contexts. Shutdown still
  takes the transport — nobody sends another frame after it.
- `rebind_backend` follows the model: the engine is rebuilt lazily from `self.backend`, so
  leaving it on the replaced model would rebuild the pipeline on the model it just replaced.

**Both are mutation-tested, and the call site needed a test of its own.** The transport's own
tests cover `rebind_backend`, and they caught nothing when `keep_sessions=True` was removed from
the daemon — because the daemon was not under test. `test_daemon.model_swap_gpu_tests` drives
`refresh_model` itself with the real weights on disk, which is what catches it; removing either
half now goes red. The first version of that test called `close_gpu_images` directly and passed
with the regression reintroduced, which is the third time in this work a check passed for the
wrong reason.

**The game folder was stale, and that is its own finding.** `nr_layer.dll` beside the game was
from 10-18 while `work` held the 10-19 build, so the panel was running code that predated both
the strict-mode fix and the pacing-rows fix. Updated through the installer (`install` action with
a request built from `work/windows-profile.json`), not by hand: the installer backs up the game's
files and owns the copy. `nr_layer32.dll` is still the older build — that is correct, not an
oversight: `_copy_runtime` installs one layer, chosen by `pe_architecture(game_exe)`, and this
game is x64.

**A second, separate bug found on the way, still open.** The window's `set_pacing` rewrites the
PACING lines of `nr-env.txt` from `layer_environment(profile)`, so it overwrites whatever the
panel wrote for the same keys. The two writers disagree about `nr-env.txt` with no ordering
between them: a panel change followed by any window pacing save silently reverts it. The profile
on this machine says `gpu_image_path: false`, `nr_async: true`, `nr_every: 4` — so the panel's
GPU row and the window's checkbox are currently saying different things. Not fixed here because
it needs a decision about which of the two owns the file, and that is the owner's call.

**The GPU path does work on this hardware**, and it is worth recording since the owner asked:
the same log has `[nr_layer] GPU transport ready: 800x600 format 43 family 0 daemon 6252
sync=semaphore; pixels remain on GPU`, followed by 1110 consecutive frames on
`transport=gpu-shared` with `image prep` and `compose` populated and `change ~0.021` — the
picture really was being rewritten, not passed through. The failure above is a mid-session
model swap, not the path being unavailable.

**Also still open, from the same session:** turning the GPU row on while `detail_strength` /
`colour_strength` are not 1 makes the daemon decline with `GPU composition requires detail and
colour strength 1`, and the panel says nothing about why. The owner confirmed those two knobs
behave normally with the GPU path off, so the constraint is specific to GPU composition — but a
row that silently does nothing is the same failure shape as issue #17.

**The speed test measures the network it is asked for** (2026-10-18, after the review). The test
only ever ran network A: `windows_wizard_core.speed_test` took no network and `speed_test.py`
defaulted to A, so a tester with B installed measured A and could not tell. The two networks are
measurably different here — B is 0.46-0.53 of A's network time — which is exactly what a tester
would want to compare on their own GPU, so the window now has a chooser beside the Speed test
button: A always, B only where `get_settings()` reports its weights are present.

The choice is the **test's** own, and deliberately not the `weights` knob's. The knob asks which
network a game runs; the test asks what to measure. They are separate because comparing the two
networks would otherwise mean changing what the game runs, and because a run records which
network it measured (`work/speed-test.json`, `preset`) so a report cannot be read as the other.
`get_settings` now returns `presets: {A: bool, B: bool}`, read through the same
`_weights_state_file` check everything else uses — a second copy of "is this a whole model" is how
the window and the backend would come to disagree about whether B can be measured at all.

On the bridge, `preset` in a speed-test request names the network to measure and is removed
before the profile is parsed, so one word cannot mean two things in one request; absent, the
profile's own network is measured, which is what the window did before the chooser existed. A
value that is not a network is refused before anything runs; a network whose weights are absent is
the backend's own refusal ("Network B's weights are not in this release: install them first, or
measure A"). `test_the_speed_test_measures_the_network_it_is_asked_for_or_says_which_is_here` and
`test_the_window_chooses_which_network_the_speed_test_measures` cover both directions and the
A-only release; the window's self-test covers the combo's contents, its labels in four languages,
and that the selection reaches the button's request. B in a release without B is **absent from the
list**, not greyed: a choice that cannot run is worse than no choice. These two tests also found
that `speed_test(preset=[...])` raised `TypeError` out of a dict lookup instead of returning a
refusal; that is fixed.

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

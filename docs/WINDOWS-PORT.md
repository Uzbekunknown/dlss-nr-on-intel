# Windows Port — Complete History, Findings and Verification

*Every discovery, change, test and lesson from porting DLSS-NR on Intel to Windows, in
the order it happened. Written for whoever picks this up next.*

## What this branch adds to master

1. **The Vulkan layer builds and runs on Windows** (MSVC), with a named-pipe transport,
   the loader exports the loader needs, and per-platform locking.
2. **The layer spawns the daemon itself** (pseudo single-DLL), opt-in and recursion-safe,
   on both platforms.
3. **The daemon listens on a named pipe on Windows** (`nr_pipe.py`).
4. **`libxmx.dll`, `libnr_image.dll` and all shaders build on Windows**
   (`tools/build_win.bat`), with the host passes byte-identical to NumPy.
5. **Deploy and release setup scripts** that carry the runtime, rename the layer, write
   the manifest, and extract weights from the user's own DLL.
6. **A start-up probe** that refuses to run when the driver's float16 conversion is
   broken, with the evidence it was built from.
7. Two MSVC-port bug fixes in master's own files (`nr_image.c`, `nr_frame.py` encoding).

## The transport decision, and why it changed twice

- **TCP loopback (first attempt)**: chosen because CPython has no `AF_UNIX` on Windows
  and a named pipe seemed to need an auth handshake. Wrong on the second point —
  `CreateNamedPipeW` in byte mode with a `NULL` security descriptor is anonymous, and
  `nr_pipe.py` proves it.
- **Named pipe (final)**: `nr_transport.h` keeps the transport in one place — `AF_UNIX`
  on Linux unchanged, a byte-mode pipe on Windows read with `PeekNamedPipe` and a
  deadline because pipes have no `SO_RCVTIMEO`. `nr_pipe.py` is the daemon's half, with
  the same `accept`/`recv`/`sendall` shape a socket has, so `serve()` does not care.

**Lesson**: measure the platform claim before designing around it. The "pipes need an
auth handshake" belief cost a parallel implementation.

## Bugs found by actually running it (each one invisible to compile+link)

| # | Symptom | Root cause | Fix |
|---|---------|-----------|-----|
| 1 | daemon dies before listening | `xmx.py` hardcoded `libxmx.so` | per-platform name, `nr_build.library()` preferred |
| 2 | daemon dies on the 2nd connection | `nr_pipe.accept()` raises `ERROR_PIPE_BUSY` when the backlog is momentarily full | retry, carry empty handle |
| 3 | tools never see a running daemon | `nr_paths.alive()` used `socket.AF_UNIX` | open-the-pipe probe |
| 4 | `frame_profile.py` crashes on Windows | shaders read without encoding | `encoding="utf-8"` |
| 5 | **whole picture NaN through the daemon** | **my own probe**: a second `xmxres.Runtime()` in the daemon's process reset libxmx.c's process-global Vulkan state | probe moved to a child process |
| 6 | batched GEMM path dead: `cannot open spv` | `build_win.bat` compiled "every .comp to a same-named .spv", missing the three aliases (gemm_batched ← gemm_coopmat_batched, gemm_f16acc ← gemm_coopmat_f16acc, gemm_tiled ← gemm_resident `-DRM=2 -DRN=2`) | the Makefile's alias table transplanted |
| 7 | contract test `16x32x16 batch 3 B^T` FAIL | same root as 6 — the staged/batched routing was broken | same fix |
| 8 | layer DLL loaded but silent | first build exported `nr_GetInstanceProcAddr`; the loader asks for **`vkGetInstanceProcAddr`** | `nr_layer.def` with the real names |

**Lesson**: a green compile + a green `dumpbin /EXPORTS` proves nothing about which
*names* are exported or whether the pipeline computes. Every gate here was passed by a
build that did not work.

## The `packHalf2x16` story — a diagnosis I got wrong, then right

- The B580's Windows driver fails the **round trip** `unpackHalf2x16(packHalf2x16(x))`
  against numpy on 90109 of 90368 values, across two driver versions (101.8993 →
  101.9033), identically.
- I first concluded "the instruction is broken, replace it everywhere" and hand-spelled
  the packing in the four attention shaders. **The picture went black.**
- Why: the softmax's exponential in those shaders is a **bit trick on the packed word**
  (`(packHalf2x16(affine) << 5) + 0x7ff88000`, read back as bits). The trick needs a
  *deterministic* packing whose bits land where the bias expects — and this driver's
  packing does that. It never reads a float back to compare with numpy. My hand-spelled
  pack produced numpy-matching bits that landed somewhere else in the bias's exponent
  space, so my "fix" moved a working instruction.
- **Resolution**: attention shaders keep the hardware `packHalf2x16` exactly as master
  has them; `half_round` (which does read a value back, and which Mesa folds when
  spelled `float(float16_t(x))`) keeps the compile-time switch, Windows build defining
  `HALF_ROUND_FLOAT16`.
- This was caught because the user remembered **two passing runs** from a batch of
  A/B tests I had dismissed. Re-running their exact conditions reproduced the pass and
  falsified my conclusion.

**Lessons**: (a) a probe that reports a defect while running inside a state you do not
fully understand is reporting on that state too; (b) when a batch of A/B results
contradicts your conclusion, the contradiction is the data; (c) a bit trick's contract
is *bits*, not values.

## MSVC porting notes (each cost real time)

- `_Static_assert` needs `/std:c11` — the errors point at the asserts, not the flag.
- MSVC exports nothing from a DLL. Export lists are **generated from the source** at
  build time, so a symbol added later cannot silently go missing (a missing one only
  shows as `function 'xmx_...' not found` from ctypes, far from the cause).
- `nr_image.c` uses `_Float16` (GCC/Clang). On MSVC a half is carried as its sixteen
  bits through the file's own `half_bits`/`half_from_bits` arithmetic — verified against
  numpy over **all 65536 half values**. The first hand-rolled widening had a wrong
  subnormal exponent bias (0x8f-shift instead of 113-shift); caught by the file's own
  test suite, which is 197/197 byte-identical.
- MSVC `/openmp` refuses `size_t` loop variables (C3015); those loops run serially on
  Windows. The GPU does the network; these are the passes around it.
- `restrict` → `__restrict`; `__attribute__((always_inline))` → `__forceinline`.
- `<windows.h>` defines `interface` as `struct` — a Vulkan entry point's parameter named
  `interface` breaks the build 1300 lines later.
- Redirection inside a cmd `if (...)` block is **not performed**, and a variable set
  inside a block is invisible to `if defined` later in the same block. Both fail
  silently. Every batch script here uses top-level `goto` for anything with a
  redirection or a set-then-test pair.
- A `bat` file needs CRLF; a UTF-8 BOM makes cmd try to run `锘緻echo`. `git` treats a
  CRLF rewrite of a LF file as a full-file change — normalize, don't churn.

## cmd batch scripts: the recurring traps

- `%~dp0` changes after `shift` — capture `TOOLSDIR` before any argument loop.
- `for %%I in ("path with spaces\..") do set "X=%%~dpI"` truncates at the space.
  Resolve with `pushd` + `%CD%`, or keep `..` unresolved and let the filesystem do it.
- `xcopy` inside a `for (...)` block does not run. Plain lines.

## Verification performed (Windows, on this machine)

- `build_win.bat` from a plain prompt: `libxmx.dll` (with `/std:c11`, export list
  generated from source), `nr_layer.dll`, `libnr_image.dll`, **20 shaders** — 0 errors.
- Loader: `vulkaninfo --summary` lists `VK_LAYER_dlssnr_intel`;
  `test_layer_loader.c` creates an instance through the layer (exit 0).
- Transport: `test_nr_link_win.c` ↔ `nr_pipe` round trip, bytes matching.
- Unit: `test_native_image.py` **197/197 byte-identical**; `test_gemm_contract.py`
  **all checks pass** (after #6); `test_resident.py` **711 passes / 0 fail** — the whole
  graph tracks the host reference on the B580; `test_input_fp16.py` exact across
  schedules.
- **End to end**: Aperture Desk Job (Steam, Source 2, `-vulkan`) → layer → named pipe →
  B580 inference → 4K frame loop. Best runs: **200 frames, median output ~90 KB, no
  bands**, row-difference std 1.76 (banded runs were 4+).
- Frame times: 4K at scale 0.5 ≈ 2.3–2.9 s to the answer with `gpu 12+300+14 ms`; 640x360
  scale 1.0 ≈ 0.07–0.08 s with `gpu 1+20+1 ms`. With `libnr_image.dll` built, the host
  passes around the network stop falling back to NumPy (they were the dominant cost at
  4K).

## Not fixed, and where the boundary is

- **master's `libxmx.c` has an intermittent `fence wait (-4)` on Windows** (first
  submit after init, B580, current driver). It reproduced during one debugging session
  and not during others on the same day; master's own `test_gemm_contract.py` passes
  19/19 now. Untested on Linux from here — that is where master develops this file, and
  the README calls the Windows runtime "separate changes".
- The reduced `packHalf2x16` reproducer for an Intel bug report is not trimmed yet; the
  probe is the starting point.
- Linux is code-reviewed only from this machine: the socket path is byte-for-byte
  master's, plus the spawn function which is POSIX-native (`posix_spawn`, env copy,
  `dladdr`, `realpath`).

## Compliance

The repository never carries NVIDIA's DLL or any weights derived from one. The extractor
(`scripts/get_weights.py`) drives MLX-DLSS against a DLL the user supplies, writes into
git-ignored `work/`, and verifies 649 tensors. Deploy scripts point the daemon at the
checkout (`NR_ROOT`) instead of copying 278 MB of weights into a game folder. The release
setup (`dist-tools/setup.bat`/`.sh`) offers three ways to find the user's own DLL:
beside the script, a chosen path, or the DLSS 5 AIO pack downloaded for them.

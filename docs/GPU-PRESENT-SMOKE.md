# Windows GPU present smoke

`src/layer/test_gpu_present.c` presents real SDR images through the NR Vulkan layer.
Its hidden owned window presents 12 frames at 128×64, recreates the swapchain at
160×96, then presents another 12 by default. Bounded `--extent` and
`--resize-extent` arguments also exercise 1080p/1440p. Ordinary runs do not read
pixels back to the host.
`src/layer/test_gpu_present.py` supplies a private daemon, endpoint, manifest,
settings, trigger and result folder. It never installs a game, changes the registry,
replaces a runtime DLL or stops an existing process.

Coordinate the GPU test slot before running. Use a normal desktop user token
outside the agent's AppContainer sandbox. The sandbox aborts at
`vkCreateSwapchainKHR` with `0xC0000409` even with NR disabled. The same hidden
window baseline succeeds outside the sandbox. Administrator elevation is not
required.

Build the fixture separately after setting the UCRT toolchain path:

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
& 'C:\msys64\ucrt64\bin\gcc.exe' -std=c11 -Wall -Wextra -Werror -O2 `
  -I "$env:VULKAN_SDK\Include" src\layer\test_gpu_present.c `
  "$env:VULKAN_SDK\Lib\vulkan-1.lib" -luser32 -o work\test_gpu_present.exe
```

The harness defaults to a dry plan. The layer, runtime, image shaders, private
Python dependencies and locally extracted weights must already be available.
Supply the fresh layer explicitly; the harness does not build any DLL.

```powershell
.\work\python\Scripts\python.exe src\layer\test_gpu_present.py `
  --run --sync-validation --exercise-controls --layer work\nr_layer.dll
```

This requests synchronization validation, disables NR after phase-one frame 4,
re-enables it after frame 6, and changes the owned render scale from 0.4 to 1.0
and back during phase two. It requires 24 successful presents and exactly 22
shared GPU inference frames, both swapchain extents, an acknowledged scale
change and no daemon/frame failure. Validation errors also make the fixture
fail. Initial graph preparation is included in its logs; this is a correctness
smoke rather than a performance benchmark or a game compatibility test.

Useful comparisons:

```powershell
# Same WSI surface without NR or a daemon.
.\work\python\Scripts\python.exe src\layer\test_gpu_present.py `
  --run --baseline --sync-validation --trace --layer work\nr_layer.dll

# Vulkan 1.1 application; the primary case defaults to Vulkan 1.3.
.\work\python\Scripts\python.exe src\layer\test_gpu_present.py `
  --run --api11 --sync-validation --exercise-controls --layer work\nr_layer.dll

# Force memory-only protocol v2, even when semaphore exports work.
.\work\python\Scripts\python.exe src\layer\test_gpu_present.py `
  --run --fenced --sync-validation --exercise-controls --layer work\nr_layer.dll
```

`--fenced` sets `NR_GPU_TRANSPORT_FENCE=1` only in the exporting fixture's
environment. The daemon still validates the incoming packet. Protocol v1 uses
exported ready/done semaphores; v2 requires zero semaphore handles and orders
GPU ownership through private fences and control replies. Pixels remain GPU
resident in both cases. Results record `selected_protocols`, `sync_modes` and
each frame's transport label. A forced v2 run rejects frames labeled only
`gpu-shared` instead of `gpu-shared-fence`.

Each new `work/gpu-present-smoke-*` folder retains `summary.json`, daemon and
fixture logs, the test settings and opt-in layer timings. Child startup/run
deadlines are bounded; cleanup stops and waits for only the owned native
processes. The base Python executable uses its venv-launcher hint to preserve
dependencies without leaving a venv launcher parent behind.

Validated on Arc 140V with Intel 101.9150, outside the sandbox:

| Application | Requested transport | Selected protocol | Shared GPU frames |
|---|---|---|---:|
| Vulkan 1.1 | Automatic | v1, semaphore | 22 |
| Vulkan 1.3 | Automatic | v1, semaphore | 22 |
| Vulkan 1.3 | Forced fenced | v2, private fences | 22 |

All three passed the full image path, both swapchain extents, controls, resize
and teardown with synchronization validation and zero errors. The forced run's
frames were all labeled `gpu-shared-fence`; its result is saved in
`work/gpu-present-nr-fenced-9150-normal/summary.json`. Standalone buffer interop
and these full-image cases retain their individual protocol and execution-mode
records.

## Owned daemon outage check

The separate `--restart-daemon` case steps the fixture through the test rather
than relying on pause timing. After two confirmed shared frames it stops only
its native daemon, presents frame 3 with that endpoint absent, then restarts its
own daemon at the same endpoint. Later frames must use the existing safe
`gpu-image-host` fallback unless `--gpu-only` is requested. Strict runs must keep
showing original frames without host pixel processing after the failure; the
test does not change the device's recovery policy.

```powershell
.\work\python\Scripts\python.exe src\layer\test_gpu_present.py `
  --run --restart-daemon --frames 8 --fenced --sync-validation `
  --layer work\nr_layer.dll
```

This diagnostic samples four bytes from the reacquired outage image before
overwriting it, checking that its center pixel retained the game's clear color.
The readback is confined to this optional test. The ordinary GPU-path smoke
still maps no pixels. Results retain both owned daemon PIDs/exit codes, the
original-pixel check, fallback frames and synchronization-validation messages.
The absent endpoint can consume the RPC deadlines, so allow the bounded run
timeout to finish. Coordinate this case through the same GPU test slot.

The validated normal-user result is
`work/gpu-present-restart-9150-final/summary.json`: two initial
`gpu-shared-fence` frames, the absent-daemon frame's original pixel retained,
and 13 subsequent `gpu-image-host` frames across both extents. All 16 presents
completed, both owned daemons were stopped, and synchronization validation
reported zero errors. The current recovery policy intentionally keeps shared
transport disabled for that device after failure. Restarting the daemon resumes
the host transport; restarting the game or recreating its Vulkan device is
required to enable shared transport again.

## Strict GPU pixel policy and high resolutions

`--gpu-only` sets `NR_GPU_ONLY=1` in both owned processes. The layer forbids legacy
host capture, cached-image writeback, async pixel exchange and photo/dump paths.
The daemon rejects legacy headers before reading their pixel payload. A shared
transport failure or unsupported mode leaves the original GPU image on screen.
The experimental GPU option in the setup window also selects this strict policy.

The CPU still loads weights at startup, records Vulkan commands, handles settings,
ownership and reduced statistics. Strict mode forbids CPU access to full frame,
feature, neural-head and image-history pixels; it does not promise zero CPU work.

```powershell
.\work\python\Scripts\python.exe src\layer\test_gpu_present.py `
  --run --gpu-only --sync-validation --layer work\nr_layer.dll `
  --extent 1920x1080 --resize-extent 2560x1440 `
  --frames 55 --warmup 25 --render-scale 0.4 --run-timeout 300
```

`present_timings` records 30 warm samples per geometry in this example. Its scope
is the complete `vkQueuePresentKHR` call including capture, daemon processing,
return and downstream WSI. Rendering and acquisition outside that call are not
timed, and this owned synthetic fixture is not a game FPS benchmark. Summaries
include the driver and binary/shader hashes. Repeat with `--render-scale 1.0` for
the full network scale or `--fenced` to validate private-fence synchronization.

Separate strict failure cases use `--gpu-only --restart-daemon --frames 8` or
`--gpu-only --unsupported async --frames 8` / `--unsupported meter` (the daemon's
`--meter`, which the GPU path leaves to the CPU; until 2026-11-21 this case was
`--unsupported settings`, a detail strength the GPU path composes now). These
diagnostics alone sample one original pixel (four bytes), verifying exact
retention. Strict restart also checks a fresh frame after the daemon returns,
so that case samples two four-byte pixels. No `gpu-image-host`/CPU or unlabeled
successful transport is allowed in their daemon logs; CPU requests in the
layer timing records also fail the test.

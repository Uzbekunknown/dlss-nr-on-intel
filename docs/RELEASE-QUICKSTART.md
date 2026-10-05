# Testing a prebuilt DLSS-NR release

This folder contains the Intel runtime, the Vulkan layer, shaders and Python
modules. It does not contain NVIDIA's DLL or model weights. Windows releases
currently use the **64-bit layer**; use the source guide for 32-bit games.

You need a supported Intel Xe2 GPU, a working Vulkan graphics driver, **64-bit
Python 3** with `numpy` and `safetensors`, and your own compatible
`nvngx_dlssnr.dll`. You do not need a compiler or the Vulkan SDK. Arc B570/B580
and supported Lunar Lake Xe2 iGPUs are the current targets.

For an MSVC Windows build, install the
[Microsoft Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
if its runtime libraries are not already present.

Install the Python packages into the interpreter the daemon will use:

```text
python -m pip install numpy safetensors
```

On Windows, `py` can replace `python`; on Linux use `python3`. If you use a
virtual environment, activate it and set `NR_PYTHON` to its interpreter before
setup and game launch.

Extract this release into a permanent folder, separate from the game. On Windows,
use an ASCII path such as `C:\dlss-nr-release` and run these commands in Command
Prompt (`cmd.exe`):

```bat
setup.bat --game "C:\Games\MyGame" --dll "D:\MyFiles\nvngx_dlssnr.dll"
```

On Linux:

```sh
./setup.sh --game "/path/to/MyGame" --dll "/path/to/nvngx_dlssnr.dll"
```

Setup extracts the weights locally, installs the layer and runtime into the
game's `dlss-nr` subfolder, and writes `launch-nr.bat` or `launch-nr.sh` there.
Keep the release folder: the launcher points the daemon at its weights and
runtime. If setup reports an error, keep its output and stop before launching.

Open the generated launcher and set or check the game executable. For an existing
weight extraction, `--skip-weights` reuses it; setup refuses this option when the
weights are missing. `--dry-run` skips installation into the game, but can still
extract weights into the release folder.

Start with a Vulkan game in a **640x360 to 800x450** window. On Windows, a DirectX
game needs an appropriate translation path such as matching x64 DXVK DLLs for
D3D9–11; setup does not install DXVK or provide a universal D3D12 route. Back up
game-local DLLs before replacing them. If Steam restarts the game, launch Steam
from the same configured environment so it inherits the layer settings.

If a Windows Steam game starts the daemon but never logs processed frames after
you create the trigger, try disabling Steam's Vulkan shader-cache recording layer
for that launch. This was required for DOOM (2016) on Arc 140V with driver
32.0.101.9033. Set this in the same configured game/Steam launch environment as
the NR variables, before the game starts:

```bat
set "DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1"
```

This disables Fossilize pipeline recording for that process and its children;
it can affect shader-cache warmup. A normal launch without the variable restores
recording. The tested workaround did not require disabling Steam's Vulkan
overlay layer.

The effect starts **off**, with render scale **0.4** configured for the first test.
Keep the game running and use a second terminal to turn it on. Replace the release
path below with yours:

```bat
type nul > "C:\dlss-nr-release\work\nr_trigger"
```

On Linux, use `touch "/path/to/release/work/nr_trigger"`. Delete that file to turn
the effect off. Read `work/nr_daemon.log` in the release folder to confirm processed
frames. Keep the release path ASCII on Windows, and stop the previous daemon when
switching releases.

Compare the same scene with the effect off and on. This adds work per frame; an
optimization reduces its cost rather than promising more FPS than the game
without the effect.

Report the release/commit, GPU/CPU, OS/driver, game/API, resolution, render scale,
`min_extent`, FPS off/on and daemon output in the project's
[Issues](https://github.com/Uzbekunknown/dlss-nr-on-intel/issues).

Maintainers: `tools/deploy.* --release <folder>` assembles a new or empty folder
from a complete build. Linux's normal release build recompiles the host passes
without `-march=native`; with `--skip-build`, supply binaries compatible with the
target CPU and OS. This folder is platform-specific. User-extracted weights and
the user-supplied DLL are local files and should not be included in a release.

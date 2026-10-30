# Testing a prebuilt DLSS-NR release

This folder contains the Intel runtime, the Vulkan layer, shaders and Python
modules. It does not contain NVIDIA's DLL or model weights. Windows releases carry
the layer for **64-bit and 32-bit games**, and DXVK for DirectX 8–11 games.

You need a supported Intel Xe2 GPU, a working Vulkan graphics driver, **64-bit
Python 3** with `numpy` and `safetensors`, and your own compatible
`nvngx_dlssnr.dll`. You do not need a compiler or the Vulkan SDK. Arc B570/B580
and supported Lunar Lake Xe2 iGPUs are the current targets.

For an MSVC Windows build, install the
[Microsoft Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
if its runtime libraries are not already present.

## Windows setup window

Extract the Windows ZIP into a permanent folder and double-click **`NR-Setup.exe`**
(`NR-Setup.cmd` is an alternative).
The window uses Windows PowerShell/WPF, so it opens without Python or a compiler.
Select your own `nvngx_dlssnr.dll` and the **game's executable**, 64-bit or 32-bit.
Python is detected automatically; you can choose another native Windows x64 interpreter.
Use **Check**, then **Install NR**. If only the Python packages are missing,
installation prepares a private Python environment inside this release. The
separate **Prepare Python** button also creates that local environment; it does
not install packages globally. A missing Python installation still needs to be
installed from [python.org](https://www.python.org/downloads/windows/).

Choose **Vulkan** for a game with its own Vulkan renderer, and **DirectX 8–11** for the
rest. Installation puts a `vulkan-1.dll` beside every game, and for DirectX 8–11 DXVK
3.1.1 as well (in `dxvk\`, with its zlib licence), for the game's architecture. A game
file of the same name is set aside. **Remove NR** deletes the game's `dlss-nr` folder and
these files and puts the set-aside ones back; a file that changed since, for example in a
game update, is left as it is and named.
DirectX 12 games do not work on Windows yet: DXVK covers DirectX 8–11, and VKD3D-Proton,
tried with Mortal Kombat 1, stopped the game at startup.

For a Vulkan game, choose the executable that draws with Vulkan: NR works only in the one
chosen. Steam starts DOOM (2016)'s `DOOMx64.exe`, which starts `DOOMx64vk.exe` when the
game's Graphics API is Vulkan, so choose `DOOMx64vk.exe` and set the game to Vulkan.
**Check** says when the chosen executable does not load the Vulkan loader itself, and
names the one beside it that does.

**Start the game the way you usually do.** That `vulkan-1.dll` is a small proxy: loaded
into the game, it gives the game's own process NR's settings and passes every Vulkan call
on to Windows' Vulkan loader, or to the game's own copy if it brought one. So Steam's Play
button, a desktop shortcut, Epic, GOG or any other launcher start the game with NR, and
setup changes none of them. **Launch game** starts it from setup. If an earlier setup set
Steam's launch options for NR, **Install NR** and **Remove NR** return them, and Steam
restarts once. A game with anti-cheat may refuse a DLL beside it, or count it as
tampering: test single-player games, offline.

**Not as administrator.** In a program that runs as administrator, Windows' Vulkan loader
ignores where NR's layer is, so NR cannot reach the game. Start the game, and Steam or the
launcher that starts it, without administrator rights. With UAC off, Windows runs every
program as administrator, and NR needs UAC on. The status says when a game runs as
administrator, and **Check** when this window does.

**Speed test.** After the first installation the window measures NR alone on this PC, with no
game: four window sizes, 640×360 to 1920×1080, at the render scale a release starts at (0.4)
and at full scale (1.0), in about half a minute. It shows what the network itself takes for a
frame at each, and the frame rate that time allows. Capturing each frame from the game and
giving it back takes time as well, and the game draws on the same GPU, so in a game the frame
rate is lower than these. **Speed test** runs it again, with no game running. **Save report**
carries the result, the whole round trip too, with the GPU, driver, CPU, memory and power, so
reports from testers say what each GPU does.

Set the game itself to an 800×450 window for the first comparison. NR starts off;
**Enable/disable NR** changes it while the game runs. If the game was already running
when NR was installed, restart it. The status distinguishes a model that is loaded from
fresh processed game frames; processed-frame throughput is not a game FPS measurement.

Open **NR controls** for the same ten live settings as Linux `nr-panel`: render
scale, minimum network side, profile, intensity, detail strength, colour strength,
temporal history, hold, release and scene-cut threshold. Numeric settings have
sliders and editable values; a slider you have clicked also takes the arrow keys.
Changes save automatically after adjusting, and the daemon reads them between frames
without a game restart. Changing scale can rebuild the network's working buffers, so
a drag is saved after release. The readout shows actual network dimensions separately
from requested scale; minimum padding may keep the same size at several scales.

**Frame pacing**, below them, is read by NR when the game starts. **Asynchronous** lets the
network work on one frame while the game draws the next: more frames a second, and what is on
screen is one processed frame behind. **Network on** every 2nd, 3rd or 4th frame repeats the
last result on the frames between: the game runs faster, and the picture changes only on the
network's frames. A change is saved at once, into the installed game as well, and takes effect
when the game starts next. On the Arc 140V neither raised the frame rate much (Mortal Kombat 11
at 1280×720, render scale 0.4: 19.3 processed frames a second in step, 19.7 asynchronous):
there the network's own time is most of every frame. Asynchronous should gain more where the
game's frame and the network's take similar time, as on a discrete card; that is not measured
yet.

**Network.** `nvngx_dlssnr.dll` 310.9.16 carries two networks: **A**, the one NR has always
run, and **B**, a smaller one NVIDIA added. With that DLL, **Install NR** takes B out of it
beside A, and **NR controls** offers both under **Network**; the choice is saved and read when
the game starts next, as frame pacing is, and the speed test measures the one chosen. On the
Arc 140V B's network takes about half of A's time: at 1280×720 and render scale 0.4, 16 ms
against 32–34, frames sent back to back. With time between frames, as a game leaves, the GPU
clocks down, and the further for B: with 25 ms between frames B took 37–39 ms a round trip
there against 22–23 back to back, and A 42–45 against 39–42. B has not been tried in a game on
Windows yet.

The button beside a control returns it to its default; **Reset all** writes the
daemon's defaults, including render scale **1.0**. The release starts at **0.4** for
a lighter first test. Reinstallation preserves chosen settings. Existing advanced
values beyond a slider's normal range remain visible in the editable value and are
preserved. The button beside the language switches the light and dark themes.

Under **Launch options**, the optional Fossilize workaround turns off Steam's Fossilize
layer in this game while NR is installed; it does not disable Steam Overlay or modify the
global shader-cache setting. The game arguments there are for **Launch game**, which also
passes the switches the game's own Steam launch options carry — a game that chooses Vulkan
with one, such as `-VULKAN`, needs it there as well.

**Save report** exports settings, version/driver information, what **Check** says, the speed
test, the game's DXVK logs and diagnostic log data without NVIDIA DLLs or weights. The release's private profile and extracted
weights stay on this PC and must not be added to a distributable.
The window supports English, Russian and Spanish, chooses its initial language from
Windows, and has a language selector at the top. The manual setup below remains available.

## Manual setup

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
game needs a translation path such as matching DXVK DLLs for D3D8–11; this manual
`setup.bat` does not install DXVK (the setup window does). Back up game-local DLLs
before replacing them. If Steam restarts the game, launch Steam
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
from a complete build. Linux's release build compiles the host passes for
`x86-64-v3` (AVX2 and F16C), the floor every build uses; with `--skip-build`, supply
binaries compatible with the target CPU and OS. This folder is platform-specific.
User-extracted weights and the user-supplied DLL are local files and should not be
included in a release.

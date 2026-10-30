@echo off
setlocal
rem ===========================================================================
rem  build_win.bat  -  build the Windows runtime, layer and shaders into work/
rem
rem  All with MSVC:
rem    work\libxmx.dll      the resident Vulkan runtime the daemon drives
rem    work\nr_layer.dll    the Vulkan layer a 64-bit game loads
rem    work\nr_layer32.dll  the same layer for a 32-bit game, if the x86 tools are there
rem    work\libnr_image.dll the host passes around the network
rem    work\libnr_alloc.dll the daemon's NumPy allocator, which keeps a frame's large blocks
rem    work\*.spv           the compute shaders
rem    work\test_exchange.exe   the layer's own exchange(), as a fixture
rem    work\test_menu_panel.exe the panel's own reporting, headless
rem    work\test_menu_side.exe  the menu's C side against the same panel, headless
rem    work\test_menu_keys.exe  the panel's hotkeys, by structure
rem    work\test_menu_language.exe  the selector matches the rows
rem    work\test_menu_actions.exe   Reset, Revert and the effect checkbox
rem    work\test_menu_pacing.exe    async, the GPU path and the cadence
rem
rem  work\mlx-dlss is NOT built here and must NOT be deleted with the artifacts. It is the
rem  extracted MLX-DLSS runtime - `features.py`, `composition.py`, the weights - and
rem  src/ref/nr_frame.py imports it at run time. `rmdir /s work` as a "clean build" therefore
rem  breaks the daemon: it starts, cannot import the network's pure-numpy half, and the layer
rem  reports a daemon that never answers. Delete the .dll/.spv/obj files instead.
rem
rem  Two things here are not obvious and both cost an afternoon if missed:
rem
rem    * libxmx.c needs /std:c11. It uses _Static_assert, which MSVC's default C
rem      dialect does not accept - the build fails with a page of syntax errors
rem      pointing at the asserts rather than at the flag.
rem    * MSVC exports nothing from a DLL unless told to. libxmx's export list is
rem      generated from the source (every non-static xmx_* definition) so a symbol
rem      added later cannot silently go missing from the DLL, which is a failure
rem      the Python side only reports as "function 'xmx_...' not found".
rem
rem  Usage:  build_win.bat            build everything
rem          build_win.bat --layer    only the layer (fast; what deploy needs)
rem          build_win.bat --shaders  only the shaders
rem ===========================================================================

set "ONLY="
if /i "%~1"=="--layer"   set "ONLY=layer"
if /i "%~1"=="--shaders" set "ONLY=shaders"

set "REPO=%~dp0.."
pushd "%REPO%" >nul 2>&1
set "REPO=%CD%"
popd
set "WORK=%REPO%\work"
set "GPU=%REPO%\src\gpu"

if not defined VULKAN_SDK (
  echo ERROR: VULKAN_SDK is not set. Install the Vulkan SDK and reopen the prompt.
  exit /b 1
)
if not exist "%WORK%" mkdir "%WORK%"

rem ---- locate MSVC ----
rem Stays OUTSIDE any ( ) block: a variable set inside a block is not visible to a
rem test later in the same block, and redirection inside a block is not performed at
rem all. Both fail silently and leave cl missing.
set "VSROOT="
where cl >nul 2>nul
if not errorlevel 1 goto :have_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\nr_vsroot.txt" 2>nul
if exist "%TEMP%\nr_vsroot.txt" set /p VSROOT=< "%TEMP%\nr_vsroot.txt"
del /Q "%TEMP%\nr_vsroot.txt" >nul 2>&1
if not defined VSROOT (
  echo ERROR: no MSVC found. Run from a "x64 Native Tools Command Prompt for VS".
  exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
:have_msvc

rem ---- the shader and runtime stages, flat: no ( ) block around a set/if pair ----
if "%ONLY%"=="layer" goto :layer

rem ---- shaders ----
echo [1/6] compiling shaders ...
set "GLSL=%VULKAN_SDK%\Bin\glslangValidator.exe"
if not exist "%GLSL%" (
  echo ERROR: glslangValidator not found in the Vulkan SDK
  exit /b 1
)
rem The Makefile's shader table, transplanted. Compiling "every .comp to its own .spv"
rem missed the three aliases the daemon asks for by a name no source file carries
rem (gemm_batched.spv from gemm_coopmat_batched.comp, gemm_f16acc.spv from
rem gemm_coopmat_f16acc.comp, gemm_tiled.spv from gemm_resident.comp), and a missing one
rem fails at run time with "cannot open spv", which names the file but not the list.
rem Same includes as the Makefile's GEMM_GLSL. Nothing is defined per driver here: libxmx
rem chooses half_round's spelling when it builds each pipeline (publish.glsl's constant 1).
for %%C in ("%GPU%\*.comp") do call :shader %%~nC
call :shader gemm_coopmat_batched
call :shader gemm_coopmat_f16acc
call :shader_def gemm_batched gemm_coopmat_batched ""
call :shader_def gemm_f16acc  gemm_coopmat_f16acc  ""
call :shader_def gemm_tiled   gemm_resident        "-DRM=2 -DRN=2"
call :shader_def gemm_staged32      gemm_staged "-DSTAGED_BM=32"
call :shader_def gemm_staged32_deep gemm_staged "-DSTAGED_BM=32" "-DSTAGED_BK=64"
rem The menu, twice: a storage image's declared format has to match the image's, so the HDR
rem surface Aperture Desk Job presents (A2B10G10R10) cannot be written with the rgba8 build. The
rem loop above already produced menu.spv without the define; this is the 10-bit one. A game on a
rem BGRA8 surface gets the first, and `nr_menu_create` picks by the swapchain's format.
call :shader_def menu_rgb10a2 menu "-DNR_MENU_TARGET_RGB10A2"
call :shader_def attention_rows     attention   "-DROW_LANES=256"
call :shader_def attention_ab       attention   "-DSOFTMAX_AB"
rem The daemon's start-up probe, whose source is a bench tool rather than a graph shader.
"%GLSL%" --target-env vulkan1.3 -I"%GPU%" -o "%WORK%\half_probe.spv" "%REPO%\src\bench\half_probe.comp" >nul 2>&1
if exist "%WORK%\half_probe.spv" (echo   half_probe.spv) else (echo   half_probe.spv FAILED)

rem ---- libxmx.dll ----
echo [2/6] building libxmx.dll ...
rem Export list from the source: every non-static xmx_* definition, so a symbol added
rem later cannot silently go missing from the DLL.
powershell -NoProfile -Command "$s = Get-Content '%GPU%\libxmx.c' -Raw; $m = [regex]::Matches($s, '(?m)^(?!static)[^\n]*?\b(xmx_\w+)\s*\([^;]*\)\s*\{') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique; @('LIBRARY libxmx','EXPORTS') + ($m | ForEach-Object { '    ' + $_ }) | Set-Content '%WORK%\libxmx.def'"
rem /std:c11 is required: libxmx.c uses _Static_assert, and MSVC's default C dialect
rem rejects it with syntax errors pointing at the asserts, not at the missing flag.
cl /nologo /O2 /std:c11 /TC /D_WIN32 /I"%VULKAN_SDK%\Include" /I"%GPU%" /Fo:"%WORK%\libxmx.obj" /Fe:"%WORK%\libxmx.dll" /LD "%GPU%\libxmx.c" /link /DEF:"%WORK%\libxmx.def" "%VULKAN_SDK%\Lib\vulkan-1.lib"
if errorlevel 1 ( echo ERROR: libxmx.dll failed & exit /b 1 )
echo   libxmx.dll

:layer
if "%ONLY%"=="shaders" goto :done
rem The layer calls no Vulkan function by name: the loader hands it every pointer, so it
rem links no vulkan-1.lib, and its exports are its own functions (nr_layer.def).
rem
rem The menu sources come with it. /utf-8 because their strings are Chinese, Russian and Spanish as
rem well as English, and MSVC otherwise reads the source in the system codepage: on a GBK machine
rem that is "newline in constant" at compile time and mojibake in the literal at run time, which
rem looks like a font bug long after it stopped being one. user32 is GetAsyncKeyState, gdi32 is
rem GetGlyphOutlineW - the two the menu needs and the layer did not.
rem
rem No /Fo with more than one source: D8036, "cannot specify multiple source files with this
rem option". The objects land beside their sources and the .def names what is exported.
rem ImGui (third_party\imgui, MIT) is C++ and this layer is C, so the UI lives in one C++
rem translation unit behind a plain-C interface (nr_menu_ui.h) and is compiled /TP into the same
rem DLL. It links no Vulkan import library either: the backend's function pointers all come from
rem the vkGetDeviceProcAddr the layer already carries. The .cpp files are compiled to objects
rem first, because /TP on a mixed-source command line would force the C files through the C++
rem front end.
rem
rem The two menu tests go through the same /TP pass: they are C++ (they drive ImGui), they link the
rem REAL nr_menu_ui.cpp and the REAL menu sources, and they need no Vulkan device - which is what
rem makes the panel's reporting and the bridge's handoff checkable on any machine that builds this
rem file. `test_exchange.c` is the opposite shape and stays in the /TC block below: it is C, and it
rem #includes nr_layer.c so its symbols are already inside its own object.
echo [3/6] building nr_layer.dll ...
cl /nologo /O2 /TP /utf-8 /EHsc /DNDEBUG /DVK_NO_PROTOTYPES /D_WIN32 /D_IMGUI_DEFINE_MATH_OPERATORS /I"%VULKAN_SDK%\Include" /I"%REPO%\third_party\imgui" /I"%REPO%\src\layer" /Fd"%REPO%\imgui.pdb" /c "%REPO%\third_party\imgui\imgui.cpp" "%REPO%\third_party\imgui\imgui_draw.cpp" "%REPO%\third_party\imgui\imgui_tables.cpp" "%REPO%\third_party\imgui\imgui_widgets.cpp" "%REPO%\third_party\imgui\imgui_impl_vulkan.cpp" "%REPO%\src\layer\nr_menu_ui.cpp" "%REPO%\src\layer\test_menu_panel.cpp" "%REPO%\src\layer\test_menu_side.cpp" "%REPO%\src\layer\test_menu_keys.cpp" "%REPO%\src\layer\test_menu_language.cpp" "%REPO%\src\layer\test_menu_actions.cpp" "%REPO%\src\layer\test_menu_pacing.cpp" "%REPO%\src\layer\test_menu_choices.cpp"
if errorlevel 1 ( echo ERROR: imgui objects failed & exit /b 1 )
rem The objects landed in the current directory (cl /c with no /Fo). Move each by name and fail
rem loudly if any is missing: a stale object from an earlier build, linked in place of a fresh one,
rem cost this menu a full day of symptoms that matched no source change.
for %%F in (imgui.obj imgui_draw.obj imgui_tables.obj imgui_widgets.obj imgui_impl_vulkan.obj nr_menu_ui.obj test_menu_panel.obj test_menu_side.obj test_menu_keys.obj test_menu_language.obj test_menu_actions.obj test_menu_pacing.obj test_menu_choices.obj) do (
  if exist "%%F" ( move /Y "%%F" "%REPO%\" >nul ) else if exist "%REPO%\third_party\imgui\%%F" (
    move /Y "%REPO%\third_party\imgui\%%F" "%REPO%\" >nul
  ) else if exist "%REPO%\src\layer\%%F" (
    move /Y "%REPO%\src\layer\%%F" "%REPO%\" >nul
  ) else (
    echo ERROR: %%F was not produced by the compile & exit /b 1
  )
)
rem /we4013: an implicit declaration is an ERROR, not a level-3 warning.
rem
rem Two rounds running, the same defect got through because MSVC is silent by default: a call to a
rem function with no prototype, C assuming it returns `int`, and a 64-bit pointer coming back with
rem its top half gone. The first one crashed a game inside a frame; the second stopped MinGW and
rem Linux from compiling the layer at all. GCC treats it as an error already (14 made it one), so
rem this is only bringing MSVC up to the standard the other two compilers hold.
cl /nologo /O2 /TC /utf-8 /we4013 /D_WIN32 /I"%VULKAN_SDK%\Include" /I"%REPO%\third_party\imgui" /I"%REPO%\src\layer" /c "%REPO%\src\layer\nr_layer.c" "%REPO%\src\layer\nr_menu_layer.c" "%REPO%\src\layer\nr_menu.c" "%REPO%\src\layer\nr_menu_font.c" "%REPO%\src\layer\nr_menu_strings.c" "%REPO%\src\layer\nr_menu_strings_lookup.c" "%REPO%\src\layer\nr_menu_bridge.c" "%REPO%\src\layer\nr_file.c" "%REPO%\src\layer\nr_focus.c" "%REPO%\src\layer\nr_raw_mouse.c" "%REPO%\src\layer\test_exchange.c"
if errorlevel 1 ( echo ERROR: layer C objects failed & exit /b 1 )
rem link.exe, not cl, for the mixed C/C++ link: cl with /TC recompiles .obj inputs it is handed
rem on the same command line (C2018, reading the binary as source), which is not a link at all.
link /nologo /DLL /OUT:"%WORK%\nr_layer.dll" /DEF:"%REPO%\src\layer\nr_layer.def" "%REPO%\nr_layer.obj" "%REPO%\nr_menu_layer.obj" "%REPO%\nr_menu.obj" "%REPO%\nr_menu_font.obj" "%REPO%\nr_menu_strings.obj" "%REPO%\nr_menu_strings_lookup.obj" "%REPO%\nr_menu_bridge.obj" "%REPO%\nr_file.obj" "%REPO%\nr_focus.obj" "%REPO%\nr_raw_mouse.obj" "%REPO%\imgui.obj" "%REPO%\imgui_draw.obj" "%REPO%\imgui_tables.obj" "%REPO%\imgui_widgets.obj" "%REPO%\imgui_impl_vulkan.obj" "%REPO%\nr_menu_ui.obj" user32.lib gdi32.lib
if errorlevel 1 ( echo ERROR: nr_layer.dll failed & exit /b 1 )
rem The layer's own exchange(), as a fixture: `test_exchange.c` INCLUDES nr_layer.c, so nr_layer.obj
rem is left out of THIS link - its symbols are already in test_exchange.obj. The masked case in
rem src/layer/test_daemon.py exists nowhere else: the request carries a mask plane the answer does
rem not, so send and receive have different sizes. CMake builds this on the Linux line and nothing
rem built it on Windows, which is why the daemon's exchange tests could not run here at all.
link /nologo /OUT:"%WORK%\test_exchange.exe" "%REPO%\test_exchange.obj" "%REPO%\nr_menu_layer.obj" "%REPO%\nr_menu.obj" "%REPO%\nr_menu_font.obj" "%REPO%\nr_menu_strings.obj" "%REPO%\nr_menu_strings_lookup.obj" "%REPO%\nr_menu_bridge.obj" "%REPO%\nr_file.obj" "%REPO%\nr_focus.obj" "%REPO%\nr_raw_mouse.obj" "%REPO%\imgui.obj" "%REPO%\imgui_draw.obj" "%REPO%\imgui_tables.obj" "%REPO%\imgui_widgets.obj" "%REPO%\imgui_impl_vulkan.obj" "%REPO%\nr_menu_ui.obj" user32.lib gdi32.lib
if errorlevel 1 ( echo ERROR: test_exchange.exe failed & exit /b 1 )
echo   test_exchange.exe
rem The menu's own two fixtures, headless. Same object set as the DLL's link, with nr_layer.obj left
rem out: neither test touches the layer, they drive `nrex_*` and `nr_menu_bridge_*` directly, and
rem imgui_impl_vulkan.obj is in the line anyway because nr_menu_ui.cpp includes its header. No Vulkan
rem device is created, so these run on any machine this file builds on.
link /nologo /OUT:"%WORK%\test_menu_panel.exe" "%REPO%\test_menu_panel.obj" "%REPO%\nr_menu.obj" "%REPO%\nr_menu_font.obj" "%REPO%\nr_menu_strings.obj" "%REPO%\nr_menu_strings_lookup.obj" "%REPO%\nr_menu_bridge.obj" "%REPO%\nr_file.obj" "%REPO%\nr_focus.obj" "%REPO%\nr_raw_mouse.obj" "%REPO%\imgui.obj" "%REPO%\imgui_draw.obj" "%REPO%\imgui_tables.obj" "%REPO%\imgui_widgets.obj" "%REPO%\imgui_impl_vulkan.obj" "%REPO%\nr_menu_ui.obj" user32.lib gdi32.lib
if errorlevel 1 ( echo ERROR: test_menu_panel.exe failed & exit /b 1 )
echo   test_menu_panel.exe
link /nologo /OUT:"%WORK%\test_menu_side.exe" "%REPO%\test_menu_side.obj" "%REPO%\nr_menu.obj" "%REPO%\nr_menu_font.obj" "%REPO%\nr_menu_strings.obj" "%REPO%\nr_menu_strings_lookup.obj" "%REPO%\nr_menu_bridge.obj" "%REPO%\nr_file.obj" "%REPO%\nr_focus.obj" "%REPO%\nr_raw_mouse.obj" "%REPO%\imgui.obj" "%REPO%\imgui_draw.obj" "%REPO%\imgui_tables.obj" "%REPO%\imgui_widgets.obj" "%REPO%\imgui_impl_vulkan.obj" "%REPO%\nr_menu_ui.obj" user32.lib gdi32.lib
if errorlevel 1 ( echo ERROR: test_menu_side.exe failed & exit /b 1 )
echo   test_menu_side.exe
rem The hotkeys are checked by structure, not by pressing keys: S, R and the effect toggle were
rem dead because their reads sat inside `if (m->open && m->read_keys)` and the ImGui build turns
rem `read_keys` off. A test that drove the bridge instead passed with the bug in place, and a
rem real `nr_menu` needs a Vulkan device, so this one asserts where the reads live in the source.
rem It links only itself and reads the sources from the repository root, so it runs from there.
link /nologo /OUT:"%WORK%\test_menu_keys.exe" "%REPO%\test_menu_keys.obj"
if errorlevel 1 ( echo ERROR: test_menu_keys.exe failed & exit /b 1 )
echo   test_menu_keys.exe
rem Does the panel's language select match the language its rows are drawn in? A settings file
rem saying `menu_language: 1` gave Chinese rows beside an "English" selector: the bridge held the
rem value and the menu held a second copy nothing updated. It reads the sources too - the push
rem it checks is a call site, and a real bridge needs a menu to hand it to.
link /nologo /OUT:"%WORK%\test_menu_language.exe" "%REPO%\test_menu_language.obj" "%REPO%\nr_menu.obj" "%REPO%\nr_menu_font.obj" "%REPO%\nr_menu_strings.obj" "%REPO%\nr_menu_strings_lookup.obj" "%REPO%\nr_menu_bridge.obj" "%REPO%\nr_file.obj" "%REPO%\nr_focus.obj" "%REPO%\nr_raw_mouse.obj" user32.lib gdi32.lib
if errorlevel 1 ( echo ERROR: test_menu_language.exe failed & exit /b 1 )
echo   test_menu_language.exe
rem Reset, Revert and the effect checkbox answer three different questions; this checks that
rem each still answers its own, and that Reset cannot reach the saved file.
link /nologo /OUT:"%WORK%\test_menu_actions.exe" "%REPO%\test_menu_actions.obj" "%REPO%\nr_menu.obj" "%REPO%\nr_menu_font.obj" "%REPO%\nr_menu_strings.obj" "%REPO%\nr_menu_strings_lookup.obj" "%REPO%\nr_menu_bridge.obj" "%REPO%\nr_file.obj" "%REPO%\nr_focus.obj" "%REPO%\nr_raw_mouse.obj" user32.lib gdi32.lib
if errorlevel 1 ( echo ERROR: test_menu_actions.exe failed & exit /b 1 )
echo   test_menu_actions.exe
rem The layer's own pacing - asynchronous mode, the GPU path and the cadence - and the conflicts
rem the setup window states: the path requires the other two off, so each of the three clears the
rem others. Also checks that rewriting the proxy environment file leaves every line it does not
rem own byte for byte, because those lines are the installation.
link /nologo /OUT:"%WORK%\test_menu_pacing.exe" "%REPO%\test_menu_pacing.obj" "%REPO%\nr_menu.obj" "%REPO%\nr_menu_font.obj" "%REPO%\nr_menu_strings.obj" "%REPO%\nr_menu_strings_lookup.obj" "%REPO%\nr_menu_bridge.obj" "%REPO%\nr_file.obj" "%REPO%\nr_focus.obj" user32.lib gdi32.lib
if errorlevel 1 ( echo ERROR: test_menu_pacing.exe failed & exit /b 1 )
echo   test_menu_pacing.exe
rem A choice list is only as long as its count: the cadence row declared four options with a
rem store of three names, and the fourth entry the count still counted drew `*Unknown item*` in
rem the game - the same shape that had cost an access violation in the profiles combo. This walks
rem the layer's cadence row and every choice knob the bridge produces.
link /nologo /OUT:"%WORK%\test_menu_choices.exe" "%REPO%\test_menu_choices.obj" "%REPO%\nr_menu.obj" "%REPO%\nr_menu_font.obj" "%REPO%\nr_menu_strings.obj" "%REPO%\nr_menu_strings_lookup.obj" "%REPO%\nr_menu_bridge.obj" "%REPO%\nr_file.obj" "%REPO%\nr_focus.obj" user32.lib gdi32.lib
if errorlevel 1 ( echo ERROR: test_menu_choices.exe failed & exit /b 1 )
echo   test_menu_choices.exe
rem Which window in front means "the user is here". The panel reads the system-wide cursor and key
rem state, so without this it is driven by whatever the user is doing in ANY window - the reported
rem bug, where a drag in another application moved a slider. The Makefile has built this for years;
rem the Windows build did not, which is the one fixture covering the focus rule the panel's input
rem gating depends on. It needs a real window and a focus change, so it self-skips those cases when
rem the OS refuses to move focus.
cl /nologo /O2 /TC /D_WIN32 /I"%VULKAN_SDK%\Include" /I"%REPO%\src\layer" /Fo:"%WORK%\test_focus_gate.obj" /c "%REPO%\src\layer\test_focus_gate.c"
if errorlevel 1 ( echo ERROR: test_focus_gate compilation failed & exit /b 1 )
cl /nologo /O2 /TC /D_WIN32 /I"%VULKAN_SDK%\Include" /I"%REPO%\src\layer" /Fo:"%WORK%\nr_focus_for_gate.obj" /c "%REPO%\src\layer\nr_focus.c"
if errorlevel 1 ( echo ERROR: nr_focus compilation failed & exit /b 1 )
link /nologo /OUT:"%WORK%\test_focus_gate.exe" "%WORK%\test_focus_gate.obj" "%WORK%\nr_focus_for_gate.obj" user32.lib
if errorlevel 1 ( echo ERROR: test_focus_gate.exe failed & exit /b 1 )
echo   test_focus_gate.exe
rem Whether the game's own raw-input registration survives ours going in and coming out. The
rem `cbSize` bug this covers passed on 32-bit and failed on every 64-bit process, so the platform
rem that was working was the platform that was broken.
cl /nologo /O2 /TC /D_WIN32 /I"%VULKAN_SDK%\Include" /I"%REPO%\src\layer" /Fo:"%WORK%\test_raw_mouse.obj" /c "%REPO%\src\layer\test_raw_mouse.c"
if errorlevel 1 ( echo ERROR: test_raw_mouse compilation failed & exit /b 1 )
cl /nologo /O2 /TC /D_WIN32 /I"%VULKAN_SDK%\Include" /I"%REPO%\src\layer" /Fo:"%WORK%\nr_raw_mouse_for_probe.obj" /c "%REPO%\src\layer\nr_raw_mouse.c"
if errorlevel 1 ( echo ERROR: nr_raw_mouse compilation failed & exit /b 1 )
link /nologo /OUT:"%WORK%\test_raw_mouse.exe" "%WORK%\test_raw_mouse.obj" "%WORK%\nr_raw_mouse_for_probe.obj" user32.lib
if errorlevel 1 ( echo ERROR: test_raw_mouse.exe failed & exit /b 1 )
echo   test_raw_mouse.exe
del /Q "%REPO%\*.obj" >nul 2>&1
echo   nr_layer.dll
echo   nr_layer.dll  (with the INS settings menu: user32, gdi32)
rem What setup puts beside a game as vulkan-1.dll: it sets the layer's environment in the
rem game's own process, so NR is there however the game is started (nr_vulkan_proxy.c).
cl /nologo /O2 /TC /D_WIN32 /I"%VULKAN_SDK%\Include" /I"%REPO%\src\layer" /Fo:"%WORK%\nr_vulkan_proxy.obj" /Fe:"%WORK%\nr_vulkan_proxy.dll" /LD "%REPO%\src\layer\nr_vulkan_proxy.c" /link /DEF:"%REPO%\src\layer\nr_vulkan_proxy.def"
if errorlevel 1 ( echo ERROR: nr_vulkan_proxy.dll failed & exit /b 1 )
echo   nr_vulkan_proxy.dll

rem A 32-bit game - Dead or Alive 5, through DXVK's 32-bit d3d9.dll - loads a 32-bit layer;
rem the daemon it talks to stays 64-bit, on the same pipe. The x86 cross tools come with the
rem C++ workload; they run in a shell of their own so the x86 environment ends with them.
echo [4/6] building nr_layer32.dll ...
set "VCX86=%VCINSTALLDIR%Auxiliary\Build\vcvarsamd64_x86.bat"
rem No parenthesised block here. `%VCX86%` holds the Visual Studio path, and on a machine
rem where that path is "C:\Program Files (x86)\..." the value contains a `)`. cmd substitutes
rem the variable into the block before parsing it, so that bracket closed the `if` early and
rem the rest of the path was read as a command: "\Microsoft was unexpected at this time."
rem The build stopped there, leaving the host-pass DLLs from the previous run in place - a
rem stale libnr_image.dll with no error against it. Two labels instead of a block.
if exist "%VCX86%" goto :have_x86
echo   skipped: no x86 tools
echo     %VCX86%
goto :image
:have_x86
rem The same sources as the 64-bit step, /utf-8 included. No /Fo with more than one source
rem (D8036), so the objects land beside their sources and the .def names what is exported.
rem
rem The UI is C++ and the layer is C, so the x86 build needs its own ImGui objects: an amd64
rem .obj cannot be linked into an x86 DLL (LNK1112), and the C++ objects are not
rem interchangeable the way the C ones are. Compiled first, with the same flags and the same
rem /TP, into a scratch directory of their own - an x86 imgui.obj in the repo root would be
rem picked up by the 64-bit link on the next run and fail there instead.
set "X86OBJ=%WORK%\obj32"
if not exist "%X86OBJ%" mkdir "%X86OBJ%"
cmd /c ""%VCX86%" >nul && cl /nologo /O2 /TP /utf-8 /EHsc /DNDEBUG /DVK_NO_PROTOTYPES /D_WIN32 /D_IMGUI_DEFINE_MATH_OPERATORS /I"%VULKAN_SDK%\Include" /I"%REPO%\third_party\imgui" /I"%REPO%\src\layer" /Fo:"%X86OBJ%\\" /c "%REPO%\third_party\imgui\imgui.cpp" "%REPO%\third_party\imgui\imgui_draw.cpp" "%REPO%\third_party\imgui\imgui_tables.cpp" "%REPO%\third_party\imgui\imgui_widgets.cpp" "%REPO%\third_party\imgui\imgui_impl_vulkan.cpp" "%REPO%\src\layer\nr_menu_ui.cpp""
if errorlevel 1 ( echo ERROR: imgui x86 objects failed & exit /b 1 )
cmd /c ""%VCX86%" >nul && cl /nologo /O2 /TC /utf-8 /D_WIN32 /I"%VULKAN_SDK%\Include" /I"%REPO%\src\layer" /Fo:"%X86OBJ%\\" /c "%REPO%\src\layer\nr_layer.c" "%REPO%\src\layer\nr_menu_layer.c" "%REPO%\src\layer\nr_menu.c" "%REPO%\src\layer\nr_menu_font.c" "%REPO%\src\layer\nr_menu_strings.c" "%REPO%\src\layer\nr_menu_strings_lookup.c" "%REPO%\src\layer\nr_menu_bridge.c" "%REPO%\src\layer\nr_file.c" "%REPO%\src\layer\nr_focus.c" "%REPO%\src\layer\nr_raw_mouse.c""
if errorlevel 1 ( echo ERROR: layer x86 C objects failed & exit /b 1 )
cmd /c ""%VCX86%" >nul && link /nologo /DLL /MACHINE:X86 /OUT:"%WORK%\nr_layer32.dll" /DEF:"%REPO%\src\layer\nr_layer.def" "%X86OBJ%\nr_layer.obj" "%X86OBJ%\nr_menu_layer.obj" "%X86OBJ%\nr_menu.obj" "%X86OBJ%\nr_menu_font.obj" "%X86OBJ%\nr_menu_strings.obj" "%X86OBJ%\nr_menu_strings_lookup.obj" "%X86OBJ%\nr_menu_bridge.obj" "%X86OBJ%\nr_file.obj" "%X86OBJ%\nr_focus.obj" "%X86OBJ%\nr_raw_mouse.obj" "%X86OBJ%\imgui.obj" "%X86OBJ%\imgui_draw.obj" "%X86OBJ%\imgui_tables.obj" "%X86OBJ%\imgui_widgets.obj" "%X86OBJ%\imgui_impl_vulkan.obj" "%X86OBJ%\nr_menu_ui.obj" user32.lib gdi32.lib"
if errorlevel 1 ( echo ERROR: nr_layer32.dll failed & exit /b 1 )
echo   nr_layer32.dll
echo   nr_layer32.dll  (with the INS settings menu)
cmd /c ""%VCX86%" >nul && cl /nologo /O2 /TC /D_WIN32 /I"%VULKAN_SDK%\Include" /I"%REPO%\src\layer" /Fo:"%WORK%\nr_vulkan_proxy32.obj" /Fe:"%WORK%\nr_vulkan_proxy32.dll" /LD "%REPO%\src\layer\nr_vulkan_proxy.c" /link /DEF:"%REPO%\src\layer\nr_vulkan_proxy.def" /MACHINE:X86"
if errorlevel 1 ( echo ERROR: nr_vulkan_proxy32.dll failed & exit /b 1 )
echo   nr_vulkan_proxy32.dll

:image

rem The host-side passes around the network. Without it those passes fall back to NumPy,
rem which at 4K is where most of a frame goes. /fp:precise matters: the file is required
rem to be byte-identical to the NumPy it transcribes, and fast maths moves the rounding
rem points. The export list is generated for the same reason as libxmx's.
rem
rem /TP and /openmp together, and both are needed: MSVC's /openmp rejects a #pragma omp
rem parallel for in C mode outright (C3015 on the canonical sample, in /TC), and compiles
rem it in C++ mode. So this one file goes through the C++ front end. The only C-isms that
rem costs are explicit casts from malloc, added for it. Without OpenMP every host pass runs
rem on one core: 212-216 ms of replayed graph at 720p against 191-195 with gcc's OpenMP.
rem
rem NR_F16C: MSVC has no _Float16, and with this nr_image.c converts each half with F16C's
rem one instruction instead of the arithmetic, the same bits for every float. F16C is part of
rem the x86-64-v3 floor the Makefile and CMake build for, every CPU since Haswell and Zen 1.
rem On the Arc 140V the features pass goes from 1.0-2.9 ms to 0.4-0.8 at 720p and 1080p.
rem Not /arch:AVX2: MSVC takes the intrinsics without it, and under it the composition,
rem which MSVC does not vectorise either way, was 0.1-0.4 ms slower.
echo [5/6] building libnr_image.dll ...
powershell -NoProfile -Command "$s = Get-Content '%REPO%\src\ref\nr_image.c' -Raw; $m = [regex]::Matches($s, '(?m)^(?!static)[^\n]*?\b(nr_\w+)\s*\([^;]*\)\s*\{') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique; @('LIBRARY libnr_image','EXPORTS') + ($m | ForEach-Object { '    ' + $_ }) | Set-Content '%WORK%\libnr_image.def'"
cl /nologo /O2 /TP /D_WIN32 /D_CRT_SECURE_NO_WARNINGS /fp:precise /openmp /DNR_F16C /Fo:"%WORK%\nr_image.obj" /Fe:"%WORK%\libnr_image.dll" /LD "%REPO%\src\ref\nr_image.c" /link /DEF:"%WORK%\libnr_image.def"
if errorlevel 1 ( echo ERROR: libnr_image.dll failed & exit /b 1 )
echo   libnr_image.dll

rem The daemon's NumPy allocator. Windows' heap returns a frame's large arrays to the system
rem as they are freed, and the next frame pays a page fault for every 4 KB of them again;
rem this keeps them from one frame to the next (src/layer/nr_alloc.py).
echo [6/6] building libnr_alloc.dll ...
cl /nologo /O2 /TC /Fo:"%WORK%\nr_alloc.obj" /Fe:"%WORK%\libnr_alloc.dll" /LD "%REPO%\src\layer\nr_alloc.c"
if errorlevel 1 ( echo ERROR: libnr_alloc.dll failed & exit /b 1 )
echo   libnr_alloc.dll

rem ---- the native GPU fixtures ----
rem
rem The two suites CMake runs on the Linux line and nothing ran on Windows:
rem   test_gpu_present.exe  a hidden Win32 window presenting through the real layer
rem   test_gpu_interop.exe  a same-device shared-resource bridge, in and out
rem Their Python harnesses (src/layer/test_gpu_present.py, test_gpu_interop.py) start the daemon
rem and set the layer's environment themselves, so the fixtures need the Vulkan SDK and nothing
rem from this build beyond it.
echo building the native GPU fixtures ...
cl /nologo /O2 /TC /utf-8 /I"%VULKAN_SDK%\Include" /I"%REPO%\src\layer" /Fo:"%WORK%\\" /Fe:"%WORK%\test_gpu_interop.exe" "%REPO%\src\layer\test_gpu_interop.c" /link "%VULKAN_SDK%\Lib\vulkan-1.lib" user32.lib
if errorlevel 1 ( echo ERROR: test_gpu_interop.exe failed & exit /b 1 )
echo   test_gpu_interop.exe
cl /nologo /O2 /TC /utf-8 /I"%VULKAN_SDK%\Include" /I"%REPO%\src\layer" /Fo:"%WORK%\\" /Fe:"%WORK%\test_gpu_present.exe" "%REPO%\src\layer\test_gpu_present.c" /link "%VULKAN_SDK%\Lib\vulkan-1.lib" user32.lib gdi32.lib
if errorlevel 1 ( echo ERROR: test_gpu_present.exe failed & exit /b 1 )
echo   test_gpu_present.exe

:done

rem A small native Windows host opens the WPF setup window without a console.
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO%\tools\build_wizard.ps1" -Root "%REPO%"
if errorlevel 1 ( echo ERROR: NR-Setup.exe failed & exit /b 1 )

echo.
echo Built into %WORK%
echo   libxmx.dll    the runtime the daemon loads
echo   nr_layer.dll  the layer a 64-bit game loads
echo   nr_layer32.dll the layer a 32-bit game loads, with a manifest whose library_arch is 32
echo   libnr_image.dll the host passes around the network
echo   libnr_alloc.dll the daemon's NumPy allocator
echo   *.spv         the compute shaders
echo   NR-Setup.exe  the Windows setup window
echo   test_exchange.exe, test_menu_panel.exe, test_menu_side.exe, test_menu_keys.exe, test_menu_language.exe, test_menu_actions.exe, test_menu_pacing.exe, test_menu_choices.exe  the native fixtures
echo.
echo Next: tools\deploy.bat --game ^<game dir^> installs the layer and points the
echo daemon at this checkout, so nothing has to be copied into the game folder.
endlocal
exit /b 0

rem ---- :shader <name> - one .comp -> .spv, from src/gpu or src/bench ----
rem
rem No per-driver defines. Intel's Windows compiler folds half_round's packHalf2x16 round
rem trip to x (90109 of 90368 values on the B580's 101.8993, the same on the Arc 140V's
rem 101.9033), so libxmx gives every pipeline the float16_t cast there instead, from the
rem driver it finds, through specialisation constant 1. The attention shaders' softmax
rem uses packHalf2x16 only as a bit trick on the packed word, which the fold does not touch.
rem
rem Two sources, not one: the runtime's shaders live in src/gpu and the probe in src/bench,
rem and the Makefile builds both. A missing source is an error here rather than a quiet
rem skip - looking only in src/gpu once made `call :shader half_probe` fall through in
rem silence, and a daemon spawned by the layer then died on the next thing that assumed it.
:shader
set "SRC=%GPU%\%1.comp"
if not exist "%SRC%" set "SRC=%REPO%\src\bench\%1.comp"
if not exist "%SRC%" (echo ERROR: no source for %1.comp & exit /b 1)
"%GLSL%" --target-env vulkan1.3 -I"%GPU%" -o "%WORK%\%1.spv" "%SRC%" >nul 2>&1
if exist "%WORK%\%1.spv" (echo   %1.spv) else (echo   %1.spv FAILED)
goto :eof

rem ---- :shader_def <out-name> <source-name> <defines...> ----
rem For the aliases and variants the Makefile builds from one source with -D. The
rem defines argument may be empty.
:shader_def
if not exist "%GPU%\%2.comp" goto :eof
set "DEFARGS="
if not "%~3"=="" set "DEFARGS=%~3"
if not "%~4"=="" set "DEFARGS=%DEFARGS% %~4"
"%GLSL%" --target-env vulkan1.3 %DEFARGS% -I"%GPU%" -o "%WORK%\%1.spv" "%GPU%\%2.comp" >nul 2>&1
if exist "%WORK%\%1.spv" (echo   %1.spv) else (echo   %1.spv FAILED)
goto :eof

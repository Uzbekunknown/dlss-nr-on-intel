@echo off
rem build_xess.bat - the XeSS proxy (libxess.dll), its panel's probe, and their tests, with MSVC.
rem   src\xess\build_xess.bat [out-dir]       default: work\xess
setlocal
set "HERE=%~dp0"
set "LAYER=%HERE%..\layer\"
set "IMGUI=%HERE%..\..\third_party\imgui\"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%HERE%..\..\work\xess"
if not exist "%OUT%" mkdir "%OUT%"
rem The same search as tools\build_win.bat, outside any ( ) block for the same reasons.
where cl >nul 2>nul
if not errorlevel 1 goto :have_msvc
set "VSROOT="
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
rem The panel's menu state is the Vulkan layer's nr_menu.c, which declares Vulkan types and links
rem no Vulkan: the SDK's headers are all it needs, as in tools\build_win.bat.
if not defined VULKAN_SDK (
  echo ERROR: VULKAN_SDK is not set; the panel's nr_menu.c needs the Vulkan SDK's headers.
  exit /b 1
)
rem cs_5_1 carries the root signature written in the shader, so the C side needs no
rem serialiser; fxc is on PATH once vcvars has run. One source, nine passes.
for %%P in (clear claim track match reduce apply encode motion compose) do call :pass %%P
if errorlevel 1 exit /b 1
rem The panel's two passes, each vertex shader with its pass's root signature (nr_menu_ui_d3d12.hlsl).
for %%P in (ui_vs ui_ps over_vs over_ps) do call :panel_pass %%P
if errorlevel 1 exit /b 1
ml64 /nologo /c /Fo"%OUT%\nr_xess_thunks.obj" "%HERE%nr_xess_thunks.asm" >nul
if errorlevel 1 ( echo ERROR: nr_xess_thunks.asm failed & exit /b 1 )
rem The command-list cut is C++, after DLSSNR-AMD's; the rest is C and calls it through nr_xess_split.h.
cl /nologo /O2 /W4 /std:c++17 /EHsc /c /Fo:"%OUT%\nr_xess_split.obj" "%HERE%nr_xess_split.cpp"
if errorlevel 1 ( echo ERROR: nr_xess_split.cpp failed & exit /b 1 )
rem The panel (nr_xess_panel.cpp): DXGI's Present hooked, the Vulkan layer's panel logic and ImGui
rem panel drawn by a Direct3D 12 renderer. The layer's sources build with the layer's own flags, as
rem tools\build_win.bat builds them; this tree's new C++ with /W4.
cl /nologo /O2 /TP /utf-8 /EHsc /DNDEBUG /D_WIN32 /DIMGUI_DEFINE_MATH_OPERATORS /I"%IMGUI%." /I"%LAYER%." /Fo"%OUT%\\" /c "%IMGUI%imgui.cpp" "%IMGUI%imgui_draw.cpp" "%IMGUI%imgui_tables.cpp" "%IMGUI%imgui_widgets.cpp" "%LAYER%nr_menu_ui.cpp"
if errorlevel 1 ( echo ERROR: the panel's ImGui objects failed & exit /b 1 )
cl /nologo /O2 /W4 /std:c++17 /utf-8 /EHsc /DNDEBUG /DIMGUI_DEFINE_MATH_OPERATORS /I"%IMGUI%." /I"%LAYER%." /I"%OUT%" /Fo"%OUT%\\" /c "%LAYER%nr_menu_ui_d3d12.cpp" "%HERE%nr_xess_panel.cpp"
if errorlevel 1 ( echo ERROR: the panel's Direct3D 12 objects failed & exit /b 1 )
cl /nologo /O2 /TC /utf-8 /we4013 /D_WIN32 /I"%VULKAN_SDK%\Include" /I"%LAYER%." /Fo"%OUT%\\" /c "%LAYER%nr_panel.c" "%LAYER%nr_menu.c" "%LAYER%nr_menu_font.c" "%LAYER%nr_menu_strings.c" "%LAYER%nr_menu_strings_lookup.c" "%LAYER%nr_menu_bridge.c" "%LAYER%nr_file.c" "%LAYER%nr_focus.c" "%LAYER%nr_raw_mouse.c" "%LAYER%nr_input_guard.c"
if errorlevel 1 ( echo ERROR: the panel's C objects failed & exit /b 1 )
set PANEL=
for %%O in (nr_xess_panel nr_menu_ui_d3d12 nr_menu_ui imgui imgui_draw imgui_tables imgui_widgets nr_panel nr_menu nr_menu_font nr_menu_strings nr_menu_strings_lookup nr_menu_bridge nr_file nr_focus nr_raw_mouse nr_input_guard) do call :panel_object %%O
cl /nologo /O2 /W4 /std:c11 /TC /I"%OUT%" /Fo:"%OUT%\nr_xess.obj" /Fe:"%OUT%\libxess.dll" /LD "%HERE%nr_xess.c" /link /DEF:"%HERE%nr_xess.def" "%OUT%\nr_xess_thunks.obj" "%OUT%\nr_xess_split.obj" %PANEL% d3d12.lib dxguid.lib user32.lib gdi32.lib
if errorlevel 1 ( echo ERROR: libxess.dll failed & exit /b 1 )
rem The probe that finds DXGI's Present in a process of its own; a release carries it beside nr_xess.dll.
cl /nologo /O2 /W4 /std:c11 /TC /Fo:"%OUT%\nr_present_probe.obj" /Fe:"%OUT%\nr_present_probe.exe" "%HERE%nr_present_probe.c" /link d3d12.lib dxgi.lib dxguid.lib user32.lib
if errorlevel 1 ( echo ERROR: nr_present_probe.exe failed & exit /b 1 )
cl /nologo /O2 /W4 /std:c11 /TC /I"%OUT%" /Fo:"%OUT%\test_nr_xess.obj" /Fe:"%OUT%\test_nr_xess.exe" "%HERE%test_nr_xess.c" /link "%OUT%\nr_xess_split.obj" d3d12.lib dxguid.lib
if errorlevel 1 ( echo ERROR: test_nr_xess.exe failed & exit /b 1 )
cl /nologo /O2 /W4 /std:c11 /TC /I"%OUT%" /Fo:"%OUT%\replay_nr_xess.obj" /Fe:"%OUT%\replay_nr_xess.exe" "%HERE%replay_nr_xess.c" /link "%OUT%\nr_xess_split.obj" d3d12.lib dxguid.lib
if errorlevel 1 ( echo ERROR: replay_nr_xess.exe failed & exit /b 1 )
rem The panel over a swap chain of the test's own, through the probe and the hook, read back.
cl /nologo /O2 /W4 /std:c11 /TC /Fo:"%OUT%\test_xess_panel.obj" /Fe:"%OUT%\test_xess_panel.exe" "%HERE%test_xess_panel.c" /link "%OUT%\nr_xess_split.obj" %PANEL% d3d12.lib dxgi.lib dxguid.lib user32.lib gdi32.lib
if errorlevel 1 ( echo ERROR: test_xess_panel.exe failed & exit /b 1 )
echo built %OUT%\libxess.dll, nr_present_probe.exe, test_nr_xess.exe, replay_nr_xess.exe and test_xess_panel.exe
exit /b 0

rem One pass: nr_xess.hlsl with the pass's name defined, upper-cased, to nr_xess_<pass>.h.
:pass
set "NAME=%~1"
set "UPPER="
for %%U in (CLEAR CLAIM TRACK MATCH REDUCE APPLY ENCODE MOTION COMPOSE) do if /i "%%U"=="%NAME%" set "UPPER=%%U"
if not defined UPPER ( echo ERROR: no define for the pass %NAME% & exit /b 1 )
fxc /nologo /T cs_5_1 /E main /O3 /D %UPPER% /Fh "%OUT%\nr_xess_%NAME%.h" /Vn nr_xess_%NAME% "%HERE%nr_xess.hlsl" >nul
if errorlevel 1 ( echo ERROR: nr_xess.hlsl's %UPPER% pass failed & exit /b 1 )
exit /b 0

rem One of the panel's objects onto the list both links take, each quoted.
:panel_object
set PANEL=%PANEL% "%OUT%\%~1.obj"
exit /b 0

rem One of the panel's shaders: nr_menu_ui_d3d12.hlsl with its name defined, upper-cased, to
rem nr_menu_ui_d3d12_<name>.h. The list is fixed, as for the passes above: a name missing from it
rem must stop the build rather than compile another shader under its name.
:panel_pass
set "NAME=%~1"
set "UPPER="
set "PROFILE="
for %%U in (UI_VS UI_PS OVER_VS OVER_PS) do if /i "%%U"=="%NAME%" set "UPPER=%%U"
if not defined UPPER ( echo ERROR: no define for the panel's shader %NAME% & exit /b 1 )
if /i "%NAME:~-2%"=="vs" set "PROFILE=vs_5_1"
if /i "%NAME:~-2%"=="ps" set "PROFILE=ps_5_1"
fxc /nologo /T %PROFILE% /E main /O3 /D %UPPER% /Fh "%OUT%\nr_menu_ui_d3d12_%NAME%.h" /Vn nr_menu_ui_d3d12_%NAME% "%LAYER%nr_menu_ui_d3d12.hlsl" >nul
if errorlevel 1 ( echo ERROR: nr_menu_ui_d3d12.hlsl's %UPPER% failed & exit /b 1 )
exit /b 0

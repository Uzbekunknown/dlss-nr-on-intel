@echo off
setlocal
rem ===========================================================================
rem  build_win.bat  -  build the Windows runtime, layer and shaders into work/
rem
rem  Three artefacts, all with MSVC:
rem    work\libxmx.dll      the resident Vulkan runtime the daemon drives
rem    work\nr_layer.dll    the Vulkan layer the game loads
rem    work\*.spv           the compute shaders
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
echo [1/3] compiling shaders ...
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
rem Same includes as the Makefile's GEMM_GLSL; -DHALF_ROUND_FLOAT16 per the note in
rem :shader below.
for %%C in ("%GPU%\*.comp") do call :shader %%~nC
call :shader gemm_coopmat_batched
call :shader gemm_coopmat_f16acc
call :shader_def gemm_batched gemm_coopmat_batched ""
call :shader_def gemm_f16acc  gemm_coopmat_f16acc  ""
call :shader_def gemm_tiled   gemm_resident        "-DRM=2 -DRN=2"
call :shader_def gemm_staged32      gemm_staged "-DSTAGED_BM=32"
call :shader_def gemm_staged32_deep gemm_staged "-DSTAGED_BM=32" "-DSTAGED_BK=64"
call :shader_def attention_rows     attention   "-DROW_LANES=256"
call :shader_def attention_ab       attention   "-DSOFTMAX_AB"
call :shader half_probe

rem ---- libxmx.dll ----
echo [2/3] building libxmx.dll ...
rem Export list from the source: every non-static xmx_* definition, so a symbol added
rem later cannot silently go missing from the DLL.
powershell -NoProfile -Command "$s = Get-Content '%GPU%\libxmx.c' -Raw; $m = [regex]::Matches($s, '(?m)^(?!static)[^\n]*?\b(xmx_\w+)\s*\([^;]*\)\s*\{') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique; @('LIBRARY libxmx','EXPORTS') + ($m | ForEach-Object { '    ' + $_ }) | Set-Content '%WORK%\libxmx.def'"
rem /std:c11 is required: libxmx.c uses _Static_assert, and MSVC's default C dialect
rem rejects it with syntax errors pointing at the asserts, not at the missing flag.
cl /nologo /O2 /std:c11 /TC /D_WIN32 /I"%VULKAN_SDK%\Include" /I"%GPU%" /Fe:"%WORK%\libxmx.dll" /LD "%GPU%\libxmx.c" /link /DEF:"%WORK%\libxmx.def" "%VULKAN_SDK%\Lib\vulkan-1.lib"
if errorlevel 1 ( echo ERROR: libxmx.dll failed & exit /b 1 )
echo   libxmx.dll

:layer
if "%ONLY%"=="shaders" goto :done
echo [3/3] building nr_layer.dll ...
cl /nologo /O2 /TC /D_WIN32 /I"%VULKAN_SDK%\Include" /I"%REPO%\src\layer" /Fe:"%WORK%\nr_layer.dll" /LD "%REPO%\src\layer\nr_layer.c" /link /DEF:"%REPO%\src\layer\nr_layer.def" "%VULKAN_SDK%\Lib\vulkan-1.lib"
if errorlevel 1 ( echo ERROR: nr_layer.dll failed & exit /b 1 )
echo   nr_layer.dll

rem The host-side passes around the network. Without it those passes fall back to NumPy,
rem which at 4K is where most of a frame goes. /fp:precise matters: the file is required
rem to be byte-identical to the NumPy it transcribes, and fast maths moves the rounding
rem points. The export list is generated for the same reason as libxmx's.
echo [4/4] building libnr_image.dll ...
powershell -NoProfile -Command "$s = Get-Content '%REPO%\src\ref\nr_image.c' -Raw; $m = [regex]::Matches($s, '(?m)^(?!static)[^\n]*?\b(nr_\w+)\s*\([^;]*\)\s*\{') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique; @('LIBRARY libnr_image','EXPORTS') + ($m | ForEach-Object { '    ' + $_ }) | Set-Content '%WORK%\libnr_image.def'"
cl /nologo /O2 /TC /D_WIN32 /D_CRT_SECURE_NO_WARNINGS /fp:precise /Fe:"%WORK%\libnr_image.dll" /LD "%REPO%\src\ref\nr_image.c" /link /DEF:"%WORK%\libnr_image.def"
if errorlevel 1 ( echo ERROR: libnr_image.dll failed & exit /b 1 )
echo   libnr_image.dll

:done

echo.
echo Built into %WORK%
echo   libxmx.dll    the runtime the daemon loads
echo   nr_layer.dll  the layer the game loads
echo   *.spv         the compute shaders
echo.
echo Next: tools\deploy.bat --game ^<game dir^> installs the layer and points the
echo daemon at this checkout, so nothing has to be copied into the game folder.
endlocal
exit /b 0

rem ---- :shader <name> - one .comp -> .spv, skipped when the source is absent ----
rem
rem Two defines, and neither is optional on this platform. The B580's Windows driver
rem (101.8993) gets float16 wrong in both spellings the graph uses:
rem
rem   -DHALF_ROUND_FLOAT16  `float(float16_t(x))` instead of the
rem                         `packHalf2x16`/`unpackHalf2x16` pair. Measured by the
rem                         daemon's start-up probe: packHalf2x16 disagrees with float16
rem                         on 90109 of 90368 values on this driver, while float16_t
rem                         agrees on all of them.
rem   -DPACK_HALF_BITS      the softmax's exponential is a bit trick on the packed word
rem                         in all four attention shaders, so a wrong pack breaks every
rem                         attention path at once: the graph's output comes back NaN.
rem                         This spells the packing and unpacking by hand instead.
rem
rem Both are compile-time because both are about one driver. Mesa keeps the hardware
rem spellings and is where they were measured to be correct; build for it with
rem NR_PACKHALF2X16=1.
:shader
if not exist "%GPU%\%1.comp" goto :eof
set "HALF_FLAG=-DHALF_ROUND_FLOAT16"
if defined NR_PACKHALF2X16 set "HALF_FLAG="
"%GLSL%" --target-env vulkan1.3 %HALF_FLAG% -I"%GPU%" -o "%WORK%\%1.spv" "%GPU%\%1.comp" >nul 2>&1
if exist "%WORK%\%1.spv" (echo   %1.spv) else (echo   %1.spv FAILED)
goto :eof

rem ---- :shader_def <out-name> <source-name> <defines...> ----
rem For the aliases and variants the Makefile builds from one source with -D. The
rem defines argument may be empty, in which case only the half-rounding flag applies.
:shader_def
if not exist "%GPU%\%2.comp" goto :eof
set "HALF_FLAG=-DHALF_ROUND_FLOAT16"
if defined NR_PACKHALF2X16 set "HALF_FLAG="
set "DEFARGS="
if not "%~3"=="" set "DEFARGS=%~3"
if not "%~4"=="" set "DEFARGS=%DEFARGS% %~4"
"%GLSL%" --target-env vulkan1.3 %HALF_FLAG% %DEFARGS% -I"%GPU%" -o "%WORK%\%1.spv" "%GPU%\%2.comp" >nul 2>&1
if exist "%WORK%\%1.spv" (echo   %1.spv) else (echo   %1.spv FAILED)
goto :eof

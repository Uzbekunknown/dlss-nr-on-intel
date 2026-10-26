@echo off
rem build_xess.bat - the XeSS proxy (libxess.dll) and its test, with MSVC.
rem   src\xess\build_xess.bat [out-dir]       default: work\xess
setlocal
set "HERE=%~dp0"
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
rem cs_5_1 carries the root signature written in the shader, so the C side needs no
rem serialiser; fxc is on PATH once vcvars has run. One source, eight passes.
for %%P in (clear claim track match reduce apply encode compose) do call :pass %%P
if errorlevel 1 exit /b 1
ml64 /nologo /c /Fo"%OUT%\nr_xess_thunks.obj" "%HERE%nr_xess_thunks.asm" >nul
if errorlevel 1 ( echo ERROR: nr_xess_thunks.asm failed & exit /b 1 )
rem The command-list cut is C++, after DLSSNR-AMD's; the rest is C and calls it through nr_xess_split.h.
cl /nologo /O2 /W4 /std:c++17 /EHsc /c /Fo:"%OUT%\nr_xess_split.obj" "%HERE%nr_xess_split.cpp"
if errorlevel 1 ( echo ERROR: nr_xess_split.cpp failed & exit /b 1 )
cl /nologo /O2 /W4 /std:c11 /TC /I"%OUT%" /Fo:"%OUT%\nr_xess.obj" /Fe:"%OUT%\libxess.dll" /LD "%HERE%nr_xess.c" /link /DEF:"%HERE%nr_xess.def" "%OUT%\nr_xess_thunks.obj" "%OUT%\nr_xess_split.obj" d3d12.lib dxguid.lib
if errorlevel 1 ( echo ERROR: libxess.dll failed & exit /b 1 )
cl /nologo /O2 /W4 /std:c11 /TC /I"%OUT%" /Fo:"%OUT%\test_nr_xess.obj" /Fe:"%OUT%\test_nr_xess.exe" "%HERE%test_nr_xess.c" /link "%OUT%\nr_xess_split.obj" d3d12.lib dxguid.lib
if errorlevel 1 ( echo ERROR: test_nr_xess.exe failed & exit /b 1 )
cl /nologo /O2 /W4 /std:c11 /TC /I"%OUT%" /Fo:"%OUT%\replay_nr_xess.obj" /Fe:"%OUT%\replay_nr_xess.exe" "%HERE%replay_nr_xess.c" /link "%OUT%\nr_xess_split.obj" d3d12.lib dxguid.lib
if errorlevel 1 ( echo ERROR: replay_nr_xess.exe failed & exit /b 1 )
echo built %OUT%\libxess.dll, test_nr_xess.exe and replay_nr_xess.exe
exit /b 0

rem One pass: nr_xess.hlsl with the pass's name defined, upper-cased, to nr_xess_<pass>.h.
:pass
set "NAME=%~1"
for %%U in (CLEAR CLAIM TRACK MATCH REDUCE APPLY ENCODE COMPOSE) do if /i "%%U"=="%NAME%" set "UPPER=%%U"
fxc /nologo /T cs_5_1 /E main /O3 /D %UPPER% /Fh "%OUT%\nr_xess_%NAME%.h" /Vn nr_xess_%NAME% "%HERE%nr_xess.hlsl" >nul
if errorlevel 1 ( echo ERROR: nr_xess.hlsl's %UPPER% pass failed & exit /b 1 )
exit /b 0

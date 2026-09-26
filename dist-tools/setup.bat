@echo off
setlocal
rem ===========================================================================
rem  setup.bat  -  configure a DLSS-NR release to run a game (Windows)
rem
rem  This ships INSIDE the release folder, next to the built layer and the runtime.
rem  It is the last step a user runs; nothing here compiles anything.
rem
rem    1. Find nvngx_dlssnr.dll. Three ways, in the order offered:
rem         a) one sitting next to this script  (drag the pack into a game folder
rem            and drop the DLL beside it - the common case)
rem         b) a folder you pick, searched for the file
rem         c) the DLSS 5 AIO pack, downloaded for you into aio\ if you do not
rem            already have it
rem       Nothing is redistributed: the DLL is NVIDIA's and the sizes below are the
rem       only thing checked about it.
rem    2. Extract the logical weights from that DLL (needs Python 3 with NumPy and
rem       safetensors; the script says so and stops if they are missing).
rem    3. Install the layer: copy it under a name you choose, write the manifest so
rem       its library_path is that file, and write a launcher that points the daemon
rem       at this folder.
rem
rem  Usage:
rem    setup.bat                          configure a game folder chosen interactively
rem    setup.bat --game "C:\Games\MyGame"
rem    setup.bat --game ... --dll X:\path\nvngx_dlssnr.dll
rem    setup.bat --game ... --name nr_layer.dll
rem    setup.bat --game ... --skip-weights
rem ===========================================================================

set "GAME="
set "DLL="
set "NAME=nr_layer.dll"
set "SKIP_WEIGHTS=0"
set "AIODIR="
rem The script directory is the release root, and it must be captured before any
rem shift moves %0.
set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

:parse
if "%~1"=="" goto :done_parse
if /i "%~1"=="--game"         ( set "GAME=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--dll"          ( set "DLL=%~2"  & shift & shift & goto :parse )
if /i "%~1"=="--name"         ( set "NAME=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--aio"          ( set "AIODIR=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--skip-weights" ( set "SKIP_WEIGHTS=1" & shift & goto :parse )
echo Unknown argument: %~1
exit /b 2
:done_parse

echo ============================================================
echo  DLSS-NR on Intel - setup
echo ============================================================
echo.

rem ---- what does this folder actually contain? ----
if not exist "%HERE%\nr_layer.dll" (
  echo ERROR: nr_layer.dll is not in %HERE%
  echo        Run this from inside the release folder it came in.
  exit /b 1
)
if not exist "%HERE%\work" (
  echo ERROR: the work\ folder is missing next to nr_layer.dll
  echo        The release is incomplete; re-extract it.
  exit /b 1
)
echo Release folder: %HERE%
echo.

rem ---- MLX-DLSS: the runtime modules the daemon imports, and the extractor ----
rem Checked before the weights, not with them: the modules are needed to run a game
rem whether or not this machine has extracted weights yet.
set "MLXDEST=%HERE%\work\mlx-dlss\python\mlxdlss"
set "MLXTOOLS=%MLXDEST%\tools"
if not exist "%MLXDEST%\features.py" goto :fetch_mlx
if not exist "%MLXTOOLS%\extract_dlssnr_weights.py" goto :fetch_mlx
goto :have_tools

:fetch_mlx
echo == fetching MLX-DLSS ^(runtime modules and extractor^)
where /q git || ( echo ERROR: git is needed to fetch it. & exit /b 3 )
set "MLXTMP=%TEMP%\nr_mlx"
if exist "%MLXTMP%" rmdir /S /Q "%MLXTMP%" 2>nul
git clone --quiet https://github.com/iamwavecut/MLX-DLSS.git "%MLXTMP%"
if errorlevel 1 (
  echo ERROR: could not clone MLX-DLSS. Check your network.
  exit /b 3
)
git -C "%MLXTMP%" checkout --quiet 06a3e11a8b68817127406ace5c764463543f699b
if errorlevel 1 (
  echo ERROR: could not pin MLX-DLSS.
  exit /b 3
)
if not exist "%MLXDEST%" mkdir "%MLXDEST%" >nul 2>&1
xcopy /E /I /Y /Q "%MLXTMP%\python\mlxdlss" "%MLXDEST%" >nul
rmdir /S /Q "%MLXTMP%" 2>nul
if not exist "%MLXTOOLS%\extract_dlssnr_weights.py" (
  echo ERROR: the extractor did not arrive under %MLXTOOLS%
  exit /b 3
)
if not exist "%MLXDEST%\features.py" (
  echo ERROR: the runtime modules did not arrive under %MLXDEST%
  exit /b 3
)
:have_tools

rem ---- 1. the NVIDIA DLL ----
if "%SKIP_WEIGHTS%"=="1" goto :after_dll

if exist "%HERE%\work\mlxw\dlssnr-logical.safetensors" (
  echo == weights already extracted
  goto :after_dll
)

if not "%DLL%"=="" (
  if exist "%DLL%" (
    echo == DLL: from the command line
    goto :find_python
  )
  echo WARNING: %DLL% does not exist; looking for one instead.
  set "DLL="
)

rem (a) next to this script
if exist "%HERE%\nvngx_dlssnr.dll" (
  set "DLL=%HERE%\nvngx_dlssnr.dll"
  echo == DLL: found next to this script
  goto :find_python
)

rem (b) a folder the user picks
echo == DLL: none beside this script
echo.
echo       Where is yours? You can:
echo         * drag the folder that holds it onto this window and press Enter,
echo         * paste a full path to the DLL or to its folder,
echo         * or press Enter to skip and fetch the DLSS 5 AIO pack instead.
echo.
set "ANSWER="
set /p "ANSWER=Path: "
if not defined ANSWER goto :fetch_aio
set "ANSWER=%ANSWER:"=%"
if exist "%ANSWER%\nvngx_dlssnr.dll" (
  set "DLL=%ANSWER%\nvngx_dlssnr.dll"
) else if exist "%ANSWER%" (
  set "DLL=%ANSWER%"
) else (
  echo   That path does not exist.
  goto :fetch_aio
)
echo   using %DLL%
goto :find_python

:fetch_aio
rem (c) the DLSS 5 AIO pack
echo.
echo   Fetching the DLSS 5 AIO pack into "%HERE%\aio" ...
if not exist "%HERE%\aio" mkdir "%HERE%\aio"
where /q curl || ( echo ERROR: curl is not available to download the pack. & exit /b 3 )
if "%AIODIR%"=="" set "AIODIR=%HERE%\aio"
rem The pack is published as a split 7z; only the part holding the DLL is needed, but
rem the archive has to be whole to extract, so all parts are fetched.
set "AIO_URL=https://github.com/Paimonshen/dllss5-aio/releases/latest/download"
echo   from %AIO_URL%
for %%P in (001 002 003) do (
  if not exist "%AIODIR%\DLSS5-AIO-v1.2.5.7z.%%P" (
    echo   DLSS5-AIO-v1.2.5.7z.%%P
    curl -L --fail -o "%AIODIR%\DLSS5-AIO-v1.2.5.7z.%%P" "%AIO_URL%/DLSS5-AIO-v1.2.5.7z.%%P" || (
      echo ERROR: the download failed. Get the pack yourself and re-run with
      echo        --aio ^<folder^> once it is unpacked.
      exit /b 3
    )
  )
)
echo.
echo   The pack is downloaded but not unpacked: extracting a 7z needs 7-Zip, and this
echo   script will not install one. Unpack DLSS5-AIO-v1.2.5.7z.001 into
echo     %AIODIR%
echo   and run this again; it will find nvngx_dlssnr.dll under
echo     02-DLSS5-Neural-Rendering\
echo.
where /q 7z && (
  echo   7z found - attempting the unpack.
  pushd "%AIODIR%"
  7z x -y "DLSS5-AIO-v1.2.5.7z.001" >nul 2>&1
  popd
)
for /d %%D in ("%AIODIR%\*") do (
  if exist "%%D\02-DLSS5-Neural-Rendering\nvngx_dlssnr.dll" (
    set "DLL=%%D\02-DLSS5-Neural-Rendering\nvngx_dlssnr.dll"
  )
)
if defined DLL (
  echo   found %DLL%
  goto :find_python
)
exit /b 3

:find_python
rem ---- 2. Python and the two packages the extractor needs ----
set "PY="
where /q py && set "PY=py"
if not defined PY ( where /q python3 && set "PY=python3" )
if not defined PY ( where /q python && set "PY=python" )
if not defined PY (
  echo ERROR: Python 3 was not found on PATH.
  echo        Install it from python.org, then run this again.
  exit /b 3
)
"%PY%" -c "import numpy, safetensors" >nul 2>&1
if errorlevel 1 (
  echo ERROR: Python is missing the packages the extractor needs.
  echo        Run:  %PY% -m pip install numpy safetensors
  exit /b 3
)

echo == extracting weights from %DLL%
echo       (a few minutes; the DLL is read, nothing is written back to it)
"%PY%" "%HERE%\scripts\get_weights.py" "%DLL%" --work-dir "%HERE%\work"
if errorlevel 1 (
  echo ERROR: extraction failed - see the message above.
  exit /b 3
)
if not exist "%HERE%\work\mlxw\dlssnr-logical.safetensors" (
  echo ERROR: the extractor reported success but the file is not there.
  exit /b 3
)
echo.

:after_dll
rem ---- 3. where is the game ----
if not "%GAME%"=="" goto :have_game
echo == Which game? Drag its folder here (the one with the .exe) and press Enter.
set "ANSWER="
set /p "ANSWER=Game folder: "
if not defined ANSWER (
  echo ERROR: no game folder given.
  exit /b 2
)
set "GAME=%ANSWER:"=%"
:have_game
if not exist "%GAME%" (
  echo ERROR: game folder not found: %GAME%
  exit /b 2
)

rem ---- 4. install ----
echo == installing into %GAME%
set "DEST=%GAME%\dlss-nr"
if not exist "%DEST%" mkdir "%DEST%"
copy /Y "%HERE%\nr_layer.dll" "%DEST%\%NAME%" >nul || exit /b 3
echo   layer:      %DEST%\%NAME%

set "MANIFEST=%HERE%\VkLayer_dlss_nr.json"
if not exist "%MANIFEST%" set "MANIFEST=%HERE%\src\layer\VkLayer_dlss_nr.json"
if not exist "%MANIFEST%" (
  echo ERROR: no VkLayer_dlss_nr.json in the release.
  exit /b 3
)
rem An absolute library_path: a bare name makes the loader fail with error 87.
powershell -NoProfile -Command "$lib = (Join-Path '%DEST%' '%NAME%'); (Get-Content '%MANIFEST%') -replace 'LIBRARY_PATH_PLACEHOLDER', $lib.Replace('\','\\') | Set-Content '%DEST%\VkLayer_dlss_nr.json'"
echo   manifest:   %DEST%\VkLayer_dlss_nr.json

rem The runtime the daemon needs, and the sources it imports. The weights stay here in
rem the release folder - the launcher points the daemon at it rather than copying 278 MB
rem into a game directory where it could be passed on by accident.
for %%D in (layer ref gpu bench) do (
  if exist "%HERE%\src\%%D" xcopy /E /I /Y /Q "%HERE%\src\%%D" "%DEST%\src\%%D" >nul
)
if exist "%HERE%\work\mlx-dlss" xcopy /E /I /Y /Q "%HERE%\work\mlx-dlss" "%DEST%\work\mlx-dlss" >nul
copy /Y "%HERE%\work\*.spv" "%DEST%\work\" >nul 2>&1
copy /Y "%HERE%\work\*.dll" "%DEST%\work\" >nul 2>&1
copy /Y "%HERE%\work\*.so"  "%DEST%\work\" >nul 2>&1
echo   runtime:    shaders and libraries

for /d /r "%DEST%\src" %%J in (__pycache__) do @if exist "%%J" rmdir /S /Q "%%J" 2>nul

set "LAUNCH=%DEST%\launch-nr.bat"
(
  echo @echo off
  echo setlocal
  echo.
  echo rem The loader finds the layer through VK_LAYER_PATH plus the manifest.
  echo set "VK_LAYER_PATH=%DEST%"
  echo set "VK_INSTANCE_LAYERS=VK_LAYER_dlssnr_intel"
  echo set "ENABLE_NR_LAYER=1"
  echo set "NR_LAYER_SPAWN=1"
  echo set "NR_LAYER_LIVE=1"
  echo rem Windows talks to the daemon over a named pipe.
  echo set "NR_LAYER_SOCKET=\\.\pipe\nr_dlssnr_intel"
  echo rem The weights stay in the release folder.
  echo set "NR_ROOT=%HERE%"
  echo.
  echo set "GAME_EXE="
  echo for %%%%F in ^("%GAME%\*.exe"^) do ^( if not defined GAME_EXE set "GAME_EXE=%%%%F" ^)
  echo if not defined GAME_EXE ^( echo No .exe in %GAME% ^& exit /b 1 ^)
  echo start "" "%%GAME_EXE%%" %%*
) > "%LAUNCH%"
echo   launcher:   %LAUNCH%

echo.
echo Done.
echo   Run the game through %LAUNCH%
echo   Weights stayed in %HERE%\work\mlxw; the launcher points NR_ROOT there.
echo.
echo COMPLIANCE: nothing NVIDIA's is redistributed by this script. The DLL it read is
echo yours, the weights came from it, and both stay on this machine.
endlocal
exit /b 0

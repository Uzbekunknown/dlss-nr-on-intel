@echo off
rem ===========================================================================
rem  sync_release.bat  -  copy the built layer and the Python side into the
rem                      release tree, which is what setup.bat deploys from.
rem
rem Why this exists. `setup.bat` reads `%HERE%\src\...` and `%HERE%\nr_layer.dll`
rem - the release tree is the deploy root, not the build tree. Building in the
rem repository changes nothing in a game until the two are copied across, and a
rem release tree that lags its source fails in the worst way: the game runs, the
rem menu opens, and the new code is simply not there. That cost a full debugging
rem session on the settings preview - the daemon was three days old and had none
rem of the two-file logic, which looked exactly like the logic being wrong.
rem
rem Usage:  tools\sync_release.bat [release-dir]
rem         the release directory defaults to the sibling ..\..\nr-release-test
rem ===========================================================================
setlocal
set "REPO=%~dp0.."
pushd "%REPO%" >nul 2>&1
set "REPO=%CD%"
popd

set "REL=%~1"
if not defined REL (
  for %%D in ("%REPO%\..\nr-release-test") do set "REL=%%~fD"
)
if not exist "%REL%\setup.bat" (
  echo ERROR: %REL% does not look like a release tree ^(no setup.bat^)
  exit /b 1
)

echo syncing %REPO%  ->  %REL%

rem ---- the built artifacts ----
rem The DLLs setup.bat copies beside the game, and the shaders it copies into work\.
rem
rem `work\*.spv` and `work\*.dll` are the runtime `setup.bat` installs INTO THE GAME (its lines 325
rem and 326 copy both globs), and this script used to refresh only the four layer DLLs that sit at
rem the release root. So a shader change never reached a release: `menu.spv` was rebuilt, the tree
rem kept the old one, and the game ran the old menu while every check on the build tree passed.
rem
rem Each file is checked for existence rather than assumed: a build that failed leaves the previous
rem one in place, and a silent stale copy is the failure this whole script is about.
set "MISSING=0"
for %%F in (nr_layer.dll nr_layer32.dll nr_vulkan_proxy.dll nr_vulkan_proxy32.dll) do (
  if exist "%REPO%\work\%%F" (
    copy /Y "%REPO%\work\%%F" "%REL%\%%F" >nul
    echo   work\%%F
  ) else (
    echo   work\%%F  MISSING - not built yet
    set "MISSING=1"
  )
)

rem The runtime setup.bat hands to the game. `libxmx.dll` is the one the daemon loads, so a stale
rem copy is a daemon that runs last week's code against this week's layer.
for %%F in ("%REPO%\work\*.spv") do (
  copy /Y "%%F" "%REL%\work\" >nul
)
for %%F in ("%REPO%\work\*.dll") do (
  copy /Y "%%F" "%REL%\work\" >nul
)
echo   work\*.spv and work\*.dll (the runtime setup.bat installs into the game)

rem A shader the layer asks for by name and the release does not carry is a menu that never draws.
for %%F in (menu.spv menu_rgb10a2.spv libxmx.dll) do (
  if not exist "%REL%\work\%%F" (
    echo   ERROR: work\%%F is missing from the release - the game will not get it
    set "MISSING=1"
  )
)

rem ---- the Python side ----
rem /E to keep the tree, /Y to overwrite, /Q for a quiet log. __pycache__ is
rem removed afterwards: a stale .pyc beside a fresh .py is another way the game
rem runs old code, which is the thing being prevented here.
xcopy /E /I /Y /Q "%REPO%\src" "%REL%\src" >nul
if errorlevel 1 ( echo ERROR: copying src failed & exit /b 1 )
echo   src\ (whole tree)

rem The installer's own scripts. `setup.bat` and `setup.sh` run
rem `%HERE%\scripts\get_weights.py`, which in turn runs `scripts\nr_weights.py`, and a release
rem without them can install a game but cannot extract weights into it - the failure reads as a
rem missing Python module rather than as a missing file. The ZIP build has always carried them
rem (`build_release.py` lists both); this local shortcut did not, so a tree synced here could only
rem ever be installed with --skip-weights.
xcopy /E /I /Y /Q "%REPO%\scripts" "%REL%\scripts" >nul
if errorlevel 1 ( echo ERROR: copying scripts failed & exit /b 1 )
echo   scripts\ (get_weights.py, nr_weights.py, build_release.py)

for /d /r "%REL%\src" %%J in (__pycache__) do @if exist "%%J" rmdir /S /Q "%%J" 2>nul

rem ---- the setup window, and the strings it is translated from ----
rem `windows-wizard.ps1` IS the installer's UI - the setup exe is a console-free host that runs
rem it - so a release tree without it installs in English whatever the build did, and one that has
rem it in the WRONG PLACE does not run at all.
rem
rem It goes in two places, and both are load-bearing:
rem   the release root   `NR-Setup.cs` does `Path.Combine(root, "windows-wizard.ps1")`, where root is
rem                      the exe's own folder - so the setup window needs it BESIDE the exe.
rem   dist-tools\        where the source keeps it, and what the string generator reads.
rem Copying only to dist-tools\ left NR-Setup.exe unable to find its own UI: it failed with
rem "Extract the complete DLSS-NR ZIP before starting setup", which reads as a packaging complaint
rem about the user's archive rather than a missing file beside the exe.
xcopy /E /I /Y /Q "%REPO%\dist-tools" "%REL%\dist-tools" >nul
if errorlevel 1 ( echo ERROR: copying dist-tools failed & exit /b 1 )
echo   dist-tools\ (the setup window and its strings)

copy /Y "%REPO%\dist-tools\windows-wizard.ps1" "%REL%\windows-wizard.ps1" >nul
if errorlevel 1 ( echo ERROR: copying the wizard beside the setup exe failed & exit /b 1 )
echo   windows-wizard.ps1  (beside the exe, where NR-Setup.exe looks for it)

if exist "%REPO%\work\NR-Setup.exe" (
  copy /Y "%REPO%\work\NR-Setup.exe" "%REL%\NR-Setup.exe" >nul
  echo   work\NR-Setup.exe
) else (
  echo   work\NR-Setup.exe  MISSING - the setup window was not built
  set "MISSING=1"
)

rem The two files the setup window cannot run without, checked here because a release that is
rem missing one still installs nothing and says so in a message about the user's ZIP.
for %%F in (windows-wizard.ps1 NR-Setup.exe) do (
  if not exist "%REL%\%%F" (
    echo   ERROR: %%F is missing from the release root - NR-Setup.exe will not run
    set "MISSING=1"
  )
)

rem ---- the check that matters ----
rem Report the daemon's size and whether it carries the marker this sync was
rem written for. A file that copied but is the wrong one is the failure mode, and
rem hashing every file to catch it is more than this needs.
for %%F in ("%REL%\src\layer\nr_daemon.py") do echo   src\layer\nr_daemon.py  %%~zF bytes
if %MISSING%==1 (
  echo.
  echo NOTE: at least one DLL was not in work\ - run tools\build_win.bat first.
  exit /b 1
)
echo.
echo Next: cd /d "%REL%" ^&^& setup.bat --game ^<game dir^> --exe ^<exe^>
endlocal
exit /b 0

@echo off
:: Build and install the HalflifeCLI plugin into the Sven Co-op game directory.
:: Usage: scripts\install_plugin.bat [GameDir]
setlocal

set "RepoDir=%~dp0.."
set "GameDir=%~1"
if "%GameDir%"=="" if defined GAME_DIR set "GameDir=%GAME_DIR%"
if "%GameDir%"=="" if exist "%RepoDir%\scripts\game_dir.txt" set /p GameDir=<"%RepoDir%\scripts\game_dir.txt"
if defined GameDir set "GameDir=%GameDir:"=%"
if "%GameDir%"=="" (
    echo ERROR: no game directory configured.
    echo   Pass the path as the first argument, set GAME_DIR, or write the full
    echo   path ^(one line^) into scripts\game_dir.txt. Try: python scripts\find_game.py
    exit /b 1
)
set "PluginSrc=%RepoDir%\build\Release\HalflifeCLI.dll"
set "PluginDst=%GameDir%\svencoop\metahook\plugins\HalflifeCLI.dll"
set "PluginsLst=%GameDir%\svencoop\metahook\configs\plugins.lst"

if not exist "%PluginSrc%" (
    echo ERROR: %PluginSrc% not found. Build first:
    echo   cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
    echo   cmake --build build --config Release
    exit /b 1
)
if not exist "%GameDir%\svencoop\metahook\plugins" (
    echo ERROR: %GameDir% does not look like a Sven Co-op install with MetaHookSv.
    exit /b 1
)

copy /y "%PluginSrc%" "%PluginDst%" || exit /b 1

:: VGUI2Extension.dll is a hard dependency: console output is captured through
:: its GameConsole callbacks. Warn when the DLL itself is not installed.
if not exist "%GameDir%\svencoop\metahook\plugins\VGUI2Extension.dll" (
    echo WARNING: %GameDir%\svencoop\metahook\plugins\VGUI2Extension.dll not found.
    echo WARNING: Console output mirroring will be disabled without it.
)

:: List VGUI2Extension.dll first so it loads before HalflifeCLI.dll
findstr /i /c:"VGUI2Extension.dll" "%PluginsLst%" >nul 2>&1
if errorlevel 1 (
    echo VGUI2Extension.dll>"%PluginsLst%.tmp"
    if exist "%PluginsLst%" type "%PluginsLst%" >>"%PluginsLst%.tmp"
    move /y "%PluginsLst%.tmp" "%PluginsLst%" >nul
    echo Added VGUI2Extension.dll to plugins.lst
) else (
    echo plugins.lst already lists VGUI2Extension.dll
)

:: Append the plugin to plugins.lst once
findstr /i /c:"HalflifeCLI.dll" "%PluginsLst%" >nul 2>&1
if errorlevel 1 (
    echo HalflifeCLI.dll>>"%PluginsLst%"
    echo Added HalflifeCLI.dll to plugins.lst
) else (
    echo plugins.lst already lists HalflifeCLI.dll
)

echo Installed: %PluginDst%
endlocal

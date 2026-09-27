@echo off
:: Build and install the HalflifeCLI plugin into the Sven Co-op game directory.
:: Usage: scripts\install_plugin.bat [GameDir]
setlocal

set "RepoDir=%~dp0.."
set "GameDir=%~1"
if "%GameDir%"=="" set "GameDir=D:\SteamLibrary\steamapps\common\Sven Co-op"
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

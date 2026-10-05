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
    echo   path ^(one line^) into scripts\game_dir.txt. Try: python mcp\find_game.py
    exit /b 1
)
set "PluginSrc=%RepoDir%\build\Release\HalflifeCLI.dll"
set "PluginDst=%GameDir%\svencoop\metahook\plugins\HalflifeCLI.dll"
set "PluginsLst=%GameDir%\svencoop\metahook\configs\plugins.lst"
set "PluginDataDir=%GameDir%\svencoop\metahook\configs\halflifecli"
set "LegacyConfigDir=%GameDir%\svencoop\metahook\configs"
set "SchemaSrc=%RepoDir%\configs\usermsgs"
set "SchemaDst=%PluginDataDir%\usermsgs"
set "GameDataSrc=%RepoDir%\build\metahook\gamedata\halflifecli"
set "GameDataDst=%GameDir%\svencoop\metahook\gamedata\halflifecli"

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

:: Do not install a DLL whose required catalog is absent or incomplete.
python "%RepoDir%\scripts\validate-gamedata.py" "%GameDataSrc%" --manifest "%RepoDir%\scripts\manifests\halflifecli.json" || exit /b 1
if not exist "%GameDataDst%" mkdir "%GameDataDst%"
copy /y "%GameDataSrc%\*.json" "%GameDataDst%\" >nul || exit /b 1
copy /y "%PluginSrc%" "%PluginDst%" || exit /b 1

:: All plugin data (halflifecli.toml, the port file, usermsg schemas) lives in
:: this subfolder of MetaHook's configs dir.
if not exist "%PluginDataDir%" mkdir "%PluginDataDir%"

:: Migrate an install from the pre-halflifecli layout (top-level files in
:: metahook\configs). The port file is rewritten at every plugin start.
if exist "%LegacyConfigDir%\halflifecli.toml" if not exist "%PluginDataDir%\halflifecli.toml" (
    move /y "%LegacyConfigDir%\halflifecli.toml" "%PluginDataDir%\halflifecli.toml" >nul
    echo Migrated halflifecli.toml into %PluginDataDir%
)
if exist "%LegacyConfigDir%\halflifecli.port" move /y "%LegacyConfigDir%\halflifecli.port" "%PluginDataDir%\" >nul

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

:: Append the plugin to plugins.lst once. Write a line break first: when the
:: file does not end with a newline, the entry would otherwise be glued onto
:: the last line (e.g. "VGUI2Extension.dllHalflifeCLI.dll"), breaking both
:: plugins. A resulting blank line is harmless, the MetaHook loader skips it.
findstr /i /c:"HalflifeCLI.dll" "%PluginsLst%" >nul 2>&1
if errorlevel 1 (
    >>"%PluginsLst%" echo(
    >>"%PluginsLst%" echo HalflifeCLI.dll
    echo Added HalflifeCLI.dll to plugins.lst
) else (
    echo plugins.lst already lists HalflifeCLI.dll
)

:: UserMsg schema definitions (per-game TOML) inside the plugin's config folder.
if not exist "%SchemaSrc%\valve.toml" (
    echo WARNING: %SchemaSrc%\valve.toml not found, usermsg schemas not installed.
) else (
    if not exist "%SchemaDst%" mkdir "%SchemaDst%"
    copy /y "%SchemaSrc%\*.toml" "%SchemaDst%" >nul
    echo Installed usermsg schemas into %SchemaDst%
)

echo Installed: %PluginDst%
endlocal

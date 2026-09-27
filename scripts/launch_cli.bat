@echo off
:: Launch Sven Co-op in CLI automation mode.
:: -windowed -novid are ALWAYS enforced: the plugin hides the render window
:: (off-screen), which requires windowed mode, and -novid skips the intro movie.
:: Usage: scripts\launch_cli.bat [extra launch args...]
:: Override the install location with: set GAME_DIR=D:\path\to\Sven Co-op
setlocal

set "GameDir=%GAME_DIR%"
if "%GameDir%"=="" set "GameDir=D:\SteamLibrary\steamapps\common\Sven Co-op"
if not exist "%GameDir%\svencoop.exe" (
    echo ERROR: svencoop.exe not found under "%GameDir%"
    exit /b 1
)

set "ARGS=-windowed -novid"
if not "%~1"=="" set "ARGS=%ARGS% %~1"

echo Launching: "%GameDir%\svencoop.exe" %ARGS%
start "" "%GameDir%\svencoop.exe" %ARGS%
endlocal

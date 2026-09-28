@echo off
:: Launch Sven Co-op in CLI automation mode.
:: -windowed -novid are ALWAYS enforced: the plugin hides the render window
:: (off-screen), which requires windowed mode, and -novid skips the intro movie.
:: Usage: scripts\launch_cli.bat [extra launch args...]
:: Game dir comes from GAME_DIR or mcp\game_dir.txt (see mcp\find_game.py).
setlocal

set "GameDir=%GAME_DIR%"
if "%GameDir%"=="" if exist "%~dp0..\mcp\game_dir.txt" set /p GameDir=<"%~dp0..\mcp\game_dir.txt"
if defined GameDir set "GameDir=%GameDir:"=%"
if "%GameDir%"=="" (
    echo ERROR: no game directory configured.
    echo   set GAME_DIR or write the full path ^(one line^) into mcp\game_dir.txt.
    echo   Try: python mcp\find_game.py
    exit /b 1
)
if not exist "%GameDir%\svencoop.exe" (
    echo ERROR: svencoop.exe not found under "%GameDir%"
    exit /b 1
)

set "ARGS=-windowed -novid"
if not "%~1"=="" set "ARGS=%ARGS% %~1"

echo Launching: "%GameDir%\svencoop.exe" %ARGS%
start "" "%GameDir%\svencoop.exe" %ARGS%
endlocal

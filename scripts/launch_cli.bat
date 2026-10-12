@echo off
:: Launch Sven Co-op in CLI automation mode.
:: -windowed -novid -redirect_fatalerror_to_log are ALWAYS enforced: the plugin
:: hides the render window (off-screen), which requires windowed mode; -novid
:: skips the intro movie; -redirect_fatalerror_to_log makes MetaHookSv write a
:: fatalerror-<datetime>-<pid>.log beside the launcher instead of a dialog.
:: Usage: scripts\launch_cli.bat [extra launch args...]
:: Game dir comes from GAME_DIR or scripts\game_dir.txt (the Python tooling in
:: mcp\ keeps its own mcp\game_dir.txt).
setlocal

set "GameDir=%GAME_DIR%"
if "%GameDir%"=="" if exist "%~dp0game_dir.txt" set /p GameDir=<"%~dp0game_dir.txt"
if defined GameDir set "GameDir=%GameDir:"=%"
if "%GameDir%"=="" (
    echo ERROR: no game directory configured.
    echo   set GAME_DIR or write the full path ^(one line^) into scripts\game_dir.txt.
    echo   Try: python mcp\find_game.py
    exit /b 1
)
if not exist "%GameDir%\svencoop.exe" (
    echo ERROR: svencoop.exe not found under "%GameDir%"
    exit /b 1
)

set "ARGS=-windowed -novid -redirect_fatalerror_to_log"
if not "%~1"=="" set "ARGS=%ARGS% %~1"

echo Launching: "%GameDir%\svencoop.exe" %ARGS%
start "" "%GameDir%\svencoop.exe" %ARGS%
endlocal

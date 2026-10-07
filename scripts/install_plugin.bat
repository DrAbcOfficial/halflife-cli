@echo off
:: Build and install the HalflifeCLI plugin into the Sven Co-op game directory.
:: Delegates to the CMake LaunchGame workflow so there is a single deployment
:: implementation (see cmake/DeployGame.cmake): the DeployGame target stages the
:: install rules, deploys them with MetahookInstallerCLI and registers the
:: plugin in plugins.lst.
:: Usage: scripts\install_plugin.bat [GameDir [AppId [ModDir]]]
:: Sven Co-op (app 225840, mod svencoop) is the default. Install into another
:: GoldSrc app by naming its Steam app id and, when it is not the app default,
:: its mod directory: scripts\install_plugin.bat D:\CS3266 10 cstrike
setlocal

set "RepoDir=%~dp0.."
set "GameDir=%~1"
set "AppId=%~2"
set "GameMod=%~3"
if "%AppId%"=="" set "AppId=225840"
if "%GameDir%"=="" if defined GAME_DIR set "GameDir=%GAME_DIR%"
if "%GameDir%"=="" if exist "%RepoDir%\scripts\game_dir.txt" set /p GameDir=<"%RepoDir%\scripts\game_dir.txt"
if defined GameDir set "GameDir=%GameDir:"=%"
if "%GameDir%"=="" (
    echo ERROR: no game directory configured.
    echo   Pass the path as the first argument, set GAME_DIR, or write the full
    echo   path ^(one line^) into scripts\game_dir.txt. Try: python mcp\find_game.py
    exit /b 1
)
:: A trailing backslash would escape the closing quote in -D...="%GameDir%".
if "%GameDir:~-1%"=="\" set "GameDir=%GameDir:~0,-1%"

:: Configure. -A Win32 is mandatory (the game process is 32-bit). No -G is given
:: so an existing build cache keeps its generator and a fresh tree picks the
:: default; reconfiguring is cheap and idempotent.
:: The app id and mod are always passed, so a previous run for another app
:: cannot leave a stale mod in the cache and deploy into the wrong folder.
cmake -S "%RepoDir%" -B "%RepoDir%\build" -A Win32 ^
    -DHALFLIFECLI_ENABLE_LAUNCH_GAME=ON ^
    -DHALFLIFECLI_GAME_DIRECTORY="%GameDir%" ^
    -DHALFLIFECLI_GAME_APPID=%AppId% ^
    -DHALFLIFECLI_GAME_MOD=%GameMod%
if errorlevel 1 exit /b 1

:: Build + deploy. gamedata synchronization and validation run as part of the
:: HalflifeCLI dependency chain, so no separate validate step is needed.
cmake --build "%RepoDir%\build" --config Release --target DeployGame
if errorlevel 1 exit /b 1

echo Installed HalflifeCLI into %GameDir%
endlocal

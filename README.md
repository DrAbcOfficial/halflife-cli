# halflife-cli

A [MetaHookSv](https://github.com/hzqst/MetaHookSv) plugin that turns
Half-Life / Sven Co-op into a CLI-driven program for automated testing or agent operation:
the game window is hidden (but alive, so `snapshot` keeps working), a CLI
console is exposed, commands come in through stdin, and a Source RCON server
runs on a random localhost port.

## Build

Requirements: Visual Studio 2019+ with C++ x86/x64 toolset, CMake 3.21+, git.

```bat
git submodule update --init --recursive --depth 1
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release
```


## Install
```bat
scripts\install_plugin.bat            [optional: path to "Sven Co-op"]
scripts\launch_cli.bat                [optional: extra launch args]
```

`launch_cli.bat` always enforces `-windowed -novid` (off-screen hiding needs
windowed mode).

No game path is hardcoded: scripts resolve the game directory from (in order)
their command-line argument, the `GAME_DIR` environment variable, and
`scripts\game_dir.txt` (one line, machine-local, gitignored). Run
`python scripts\find_game.py` to locate the install automatically (Steam
registry, `libraryfolders.vdf`, common layouts) or to validate a candidate
with `--dir <path>`; when nothing is found, configure the path as above.

## Using it
- **stdin**: every line is executed as a game console command (`status`,
  `map osprey`, `snapshot`, `quit`, ...). Plugin commands: `cli.help`,
  `cli.rconinfo`, `cli.window <0|1|2>` (0=show, 1=off-screen default,
  2=SW_HIDE; note SW_HIDE may pause rendering on some engines).
- **stdout mirror**: all captured console output is echoed live, including
  `Con_DPrintf` output (captured even when `developer` is 0; the plugin also
  sets `developer 1` by default so the in-game console shows it too).
- **RCON**: `scripts/rcon_client.py <host> <port> <password> "cmd" ...` or any
  standard Source RCON client. The bound port is written to
  `svencoop\metahook\configs\halflifecli.port` for discovery.

## Configuration

`svencoop\metahook\configs\halflifecli.ini` (all optional):

```ini
[rcon]
port=0                 ; 0 = random available port
bind=127.0.0.1         ; default allows localhost connections only
password=              ; empty = accept any auth (localhost default); set to require
allowed_ips=           ; comma separated whitelist; empty = bind address only

[cli]
hide_window=1          ; 0=off 1=off-screen (default) 2=SW_HIDE
developer=1            ; set the developer cvar at startup
```

## CI

[`.github/workflows/build.yml`](.github/workflows/build.yml) builds the plugin
on every push (`windows-latest`, Win32) and uploads the DLL as a workflow
artifact. Pushing a `v*` tag additionally publishes a GitHub Release with
`HalflifeCLI-<tag>.zip`:

```bat
git tag v0.1.0
git push origin v0.1.0
```
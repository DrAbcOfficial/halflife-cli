# halflife-cli

A [MetaHookSv](https://github.com/hzqst/MetaHookSv) plugin that turns
Half-Life / Sven Co-op into a CLI-driven program for automated testing:
the game window is hidden (but alive, so `snapshot` keeps working), a CLI
console is exposed, commands come in through stdin, and a Source RCON server
runs on a random localhost port.

Windows only, C++20, CMake (Win32/x86 — the game process is 32-bit).

## Build

Requirements: Visual Studio 2019+ with C++ x86/x64 toolset, CMake 3.21+, git.

```bat
git submodule update --init --recursive --depth 1
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release
```

The MetaHookSv submodule supplies the plugin ABI headers
(`include/metahook.h`, `include/Interface/IPlugins.h`, HLSDK headers) and the
`CreateInterface` glue (`include/HLSDK/common/interface.cpp`).

## Install

Prerequisite: a working MetaHookSv install (latest `svencoop.exe` launcher,
`svencoop\metahook\gamedata`, and a `plugins.lst`). The output-capture feature
resolves `Con_Printf` / `Con_DPrintf` / `Con_Warning` through MetaHookSv
gamedata symbols; without gamedata commands still run but output mirroring is
disabled.

```bat
scripts\install_plugin.bat            [optional: path to "Sven Co-op"]
scripts\launch_cli.bat                [optional: extra launch args]
```

`launch_cli.bat` always enforces `-windowed -novid` (off-screen hiding needs
windowed mode). Set `GAME_DIR` to override the install path.

## Using it

On startup the plugin prints a banner, either into an inherited/piped console
(automation) or into an allocated console window:

```
halflife-cli 2026-09-27T... loaded (engine: SVENGINE)
halflife-cli: RCON listening on 127.0.0.1:53219 (password: none)
```

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

## How it works

- `IPluginsV4` lifecycle: `LoadEngine` installs the `Con_*` inline hooks;
  `LoadClient` overrides `HUD_Init` (register `cli.*` commands) and
  `HUD_Frame` (per-frame pump).
- Commands are queued from stdin/RCON threads and executed on the engine
  thread via `Cbuf_AddText` (falls back to `pfnClientCmd`), so they behave
  exactly like typed console input.
- RCON responses pair a captured-output sequence range `[begin, next frame]`
  with each command; timeout is 5s.
- Protocol layer and command layer are decoupled; recv loops read exactly the
  announced sizes; packets are capped at 4096 body bytes; auth failure sends
  `id = -1` and disconnects; connection cap 4.

## Notes

- The MetaHookSv submodule is pinned to an upstream commit; its build system
  remains MSBuild-based and is used only when (re)building MetaHookSv itself
  (`scripts/build-MetaHook.bat` inside the submodule; pass
  `-p:PlatformToolset=<latest>` on VS2026 to keep toolsets consistent).
- `.zcode/`, `build/` and other local artifacts are git-ignored.

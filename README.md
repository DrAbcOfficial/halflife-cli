# halflife-cli

A [MetaHookSv](https://github.com/hzqst/MetaHookSv) plugin that turns
Half-Life / Sven Co-op into a CLI-driven program for automated testing or agent operation:
the game window is hidden (but alive, so `screenshot` keeps working), a CLI
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
their command-line argument, the `GAME_DIR` environment variable, and a
machine-local, gitignored `game_dir.txt` — `scripts\game_dir.txt` for the
`.bat` launchers, `mcp\game_dir.txt` for the Python tooling. Run
`python mcp\find_game.py` to locate the install automatically (Steam
registry, `libraryfolders.vdf`, common layouts) or to validate a candidate
with `--dir <path>`; when nothing is found, configure the path as above.

## MCP server

`mcp/halflife_mcp.py` is a Model Context Protocol server (stdio transport)
that turns the game into callable tools for MCP clients. It launches the game
itself, so the game survives the client's lifetime; a game started elsewhere
is attached to by reading the plugin's port file.

Tools: `launch_game`, `game_status`, `run_command`, `find_cvar`,
`read_console`, `snapshot` (returns a PNG image, downscaled by default), and
`quit_game`.

Run it with [uv](https://docs.astral.sh/uv/) (dependencies are declared inline,
nothing is installed globally):

```bat
uv run --script mcp\halflife_mcp.py
```

or with a plain Python environment:

```bat
pip install "mcp>=2.2,<3" pillow pywin32
python mcp\halflife_mcp.py
```

Claude Code picks up the committed [`.mcp.json`](.mcp.json) automatically when
the repo is opened (it asks for approval the first time); register it elsewhere
with `claude mcp add`. The first `uv run` downloads the dependencies, which can
take a minute.

## Using it
- **stdin**: every line is executed as a game console command (`status`,
  `map osprey`, `screenshot`, `quit`, ...). Plugin commands: `cli.help`,
  `cli.rconinfo`, `cli.window <0|1|2>` (0=show, 1=off-screen default,
  2=SW_HIDE; note SW_HIDE may pause rendering on some engines), and
  `cli.find <name>` to check whether a cvar/command exists — when it does
  not, up to 10 similar names (substring match or small edit distance) are
  suggested, e.g. `cli.find abc` → `similar names: ab, ac, bc, c`.
- **stdout mirror**: all captured console output is echoed live, including
  `Con_DPrintf` output when the engine routes it to the vgui console (the
  plugin sets `developer 1` by default so it does).
- **UserMsg monitor**: every server user message the game receives is decoded
  per a TOML schema and printed as one `[usermsg] Name size=N field=value ...`
  console line (see below). `cli.usermsg` reports the hook state;
  `cli.usermsg on|off|reload|list|pending|<name>` controls it.
- **RCON**: `mcp/rcon_client.py <host> <port> <password> "cmd" ...` or any
  standard Source RCON client. The bound port is written to
  `svencoop\metahook\configs\halflifecli.port` for discovery.

## UserMsg monitor

The plugin wraps the user-message hooks the client game DLL registers with the
engine (`g_pMetaHookAPI->HookUserMsg`), decodes each payload per a schema
description, prints one line per message, and forwards the message to the game
untouched. Messages the game DLL never hooks are registered display-only, so
server-only traffic (e.g. svencoop `DeathMsg`, `SelAmmo`) is visible too.

Schemas live in `svencoop\metahook\configs\usermsgs\<gamedir>.toml`
(`configs/usermsgs/` in the repo, installed by `install_plugin.bat`). One file
per `-game` folder: `valve.toml` (Half-Life), `cstrike.toml`,
`svencoop.toml`. A file may inherit a base with
`extends = "valve.toml"` and then only define messages whose wire format
differs. Field syntax:

```toml
[primitives]
coord_size = 2            # 2 = short*1/8 (hl/cstrike), 4 = long*1/8 (svencoop)

[[usermsg]]
name = "StatusIcon"
fields = [
    { name = "enable", type = "byte" },
    { name = "icon", type = "string" },
    { name = "rgb", type = "byte", count = 3, when = { field = "enable", ne = 0 } },
]
```

Types: `byte char short word long float coord angle angle16 string vec3
group`; `count` is a number, the name of a previously-read field, or `*`
(until the buffer runs out); `when` makes a field (or group) conditional.
`raw = true` dumps the payload as hex instead of parsing. The definitions are
transcribed from the reverse-engineering notes in `.zcode/networkmessages/`
(gitignored).

`python mcp/usermsg_test.py [--connect HOST:PORT]` runs the acceptance flow:
launches the game, checks the hook report, and captures `[usermsg]` traffic
from a local map or a game server.

## Dependencies

`VGUI2Extension.dll` (from
[MetaHookSv](https://github.com/hzqst/MetaHookSv)) must be installed in
`svencoop\metahook\plugins\` and listed in `plugins.lst`: console output is
captured through its GameConsole interface callbacks and commands are executed
through the engine's client command entry, so no engine code is hooked
directly. `install_plugin.bat` ensures `VGUI2Extension.dll` is listed first in
`plugins.lst` and warns when the DLL itself is missing.

## Configuration

`svencoop\metahook\configs\halflifecli.toml` (all optional):

```toml
[rcon]
port = 0                    # 0 = random available port
bind = "127.0.0.1"          # default allows localhost connections only
password = ""               # empty = accept any auth (localhost default); set to require
allowed_ips = ""            # comma separated whitelist; empty = bind address only

[cli]
hide_window = 1             # 0=off 1=off-screen (default) 2=SW_HIDE
developer = 1               # set the developer cvar at startup
console_topmost = 0         # 1 = keep the CLI console window always on top

[usermsg]
enabled = 1                 # hook + decode server user messages
file = ""                   # schema file; empty = "<gamedir>.toml" in configs/usermsgs/
max_string = 64             # truncate decoded strings longer than this
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
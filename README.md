# halflife-cli

A [MetaHookSv](https://github.com/hzqst/MetaHookSv) plugin that turns
Half-Life / Sven Co-op into a CLI-driven program for automated testing or agent operation:
the game window is hidden (but alive, so `screenshot` keeps working), a CLI
console is exposed and commands come in through stdin. On Windows x86 Sven
Co-op, Half-Life (and the mods sharing its engine) and Cry of Fear, Native
GoldSrc UDP RCON shares the engine's server socket, including at the main menu.
Other engines retain the independent Source TCP backend.

### Trigger the bunker nuke with DeepSeek


https://github.com/user-attachments/assets/f2f113e6-9ad6-45f7-940a-90005684d6b7



## Build

Requirements: Visual Studio 2019+ with C++ x86/x64 toolset, CMake 3.21+, Python 3.11+, git.

```bat
git submodule update --init --depth 1
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release
```

Only tomlplusplus is a submodule. CMake fetches pinned
[MetaHook](https://github.com/MetaHookSv/MetaHook) and
[VGUI2Extension](https://github.com/MetaHookSv/VGUI2Extension) sources on the
first configure (network access required), without configuring their projects
or fetching their submodules. VGUI2Extension supplies interface headers only;
its runtime DLL must still be installed separately.

To use existing source trees instead:

```bat
cmake -S . -B build -A Win32 ^
  -DMETAHOOK_SOURCE_PATH=D:/MetaHookSv/MetaHook ^
  -DVGUI2EXTENSION_SOURCE_PATH=D:/MetaHookSv/Plugins/VGUI2Extension
```

Each path independently falls back to FetchContent when empty. These CMake
cache variables default to the same-named environment variables on the first
configure; explicit `-D` values override them. Relative paths are resolved from
the project root. Invalid explicit paths fail before any dependency downloads.
The configure output prints the resolved source paths.

The build also runs `scripts/sync-gamedata.py` and `scripts/validate-gamedata.py`
with `scripts/manifests/halflifecli.json`, following the MetaHookSv/BetterSpray
consumer workflow. They download identity-bound snapshots directly from
GoldSrc_VibeSignatures, cache verified downloads, and package only the required
symbols in `build/metahook/gamedata/halflifecli/`. A catalog missing required
symbols fails the build and installation. `GOLDSRC_VIBESIGNATURES_INDEX_URL`
can select a mirror; it must satisfy the same manifest and integrity checks.

Native UDP needs MetaHook API **115+** and covers every Windows x86 engine
build the manifest declares (`gameVersions` in
`scripts/manifests/halflifecli.json`, symbols from
[GoldSrc_VibeSignatures #326](https://github.com/HLND2T/GoldSrc_VibeSignatures/issues/326)
and [PR #331](https://github.com/HLND2T/GoldSrc_VibeSignatures/pull/331)):

| Engine | Builds | Address layout | Notes |
| --- | --- | --- | --- |
| SvEngine | 8948, 10257 | 36-byte `netadr` | |
| GoldSrc (HL, CS, CZ, OP4, BS, ...) | 3248, 3266, 3329, 3647 (BLOB), 4554, 6153, 8684, 10210 | 20-byte `netadr_t` | 10210 inlines four helpers, see below |
| Cry of Fear | 5936 | 20-byte `netadr_t` | |

cstrike/czero/czeror snapshots carry no engine module: those mods run on the
Half-Life `hw.dll` of the same build and use its record. The adapter is
instantiated per address layout, and the C++ build list in
`src/rcon/native_udp.cpp` keeps an unverified build away from the catalog even
when another plugin's catalog matches it. Linux has no adapter.

Half-Life 10210 (Windows) keeps `SV_HandleRconPacket`, `SV_CheckRconFailure`,
`SV_BeginRedirect` and `SV_EndRedirect` only inline, so the manifest exempts
them for that build and the plugin supplies the behavior itself: redirects are
driven through the published `sv_redirected`/`sv_redirectto`/`outputbuf`
state; failure accounting is a plugin table with the native semantics
(`sv_rcon_minfailures`/`maxfailures`/`minfailuretime`, ban through `addip`,
cleared by `resetrcon`); and menu packets are tokenized by the engine's own
`Cmd_ExecuteString` through the internal `cli._rconpacket` command, which then
calls the native challenge/rcon paths with the native `Cmd_Argv`. Typed into a
console the command does nothing.

Sven selects Native UDP unconditionally; its initialization failure reports
the missing symbol/category or layout and never silently selects TCP. A
GoldSrc build selects it when the build is listed and the catalog covers its
`hw.dll`; otherwise it keeps Source TCP and the console says why
(`Native UDP unavailable (...); using Source TCP`). Once selected, a failure
never falls back to TCP. See [verification evidence](docs/native-rcon-verification.md)
for the distinction between static coverage and actual game runs.


## Install
```bat
scripts\install_plugin.bat            [optional: GameDir [AppId [ModDir]]]
scripts\launch_cli.bat                [optional: extra launch args]
```

Sven Co-op (app `225840`, mod `svencoop`) is the default. Another GoldSrc app
needs its Steam app id, and its mod directory when that is not the app default:

```bat
scripts\install_plugin.bat D:\CS3266 10 cstrike
```

`launch_cli.bat` always enforces `-windowed -novid` (off-screen hiding needs
windowed mode).

`install_plugin.bat` now configures and drives the CMake `DeployGame` target, so
the batch and the Visual Studio workflow share one deployment implementation.

No game path is hardcoded: scripts resolve the game directory from (in order)
their command-line argument, the `GAME_DIR` environment variable, and a
machine-local, gitignored `game_dir.txt` — `scripts\game_dir.txt` for the
`.bat` launchers, `mcp\game_dir.txt` for the Python tooling. Run
`python mcp\find_game.py` to locate the install automatically or to validate a
candidate with `--dir <path>`; when nothing is found, configure the path as
above.

Sven Co-op (the default) is found without external tools (Steam registry,
`libraryfolders.vdf`, common layouts). Other GoldSrc apps are resolved through
`MetahookInstallerCLI`, which knows Steam's layout and each app's default mod:

```bat
python mcp\find_game.py --appid 70               # Half-Life (valve)
python mcp\find_game.py --appid 10 --mod cstrike # Counter-Strike
python mcp\find_game.py --appid 30               # Day of Defeat
```

`find_game.py` discovers `MetahookInstallerCLI.exe` from
`HALFLIFECLI_INSTALLER_CLI_EXECUTABLE` or the CMake build tree (populate it
once with `-DHALFLIFECLI_ENABLE_LAUNCH_GAME=ON`); it is never downloaded by the
script itself.

### Visual Studio one-click deploy + debug

Instead of `install_plugin.bat` + manual attach, CMake can add a `LaunchGame`
target that builds the plugin, deploys it into the game and starts Sven Co-op
under the debugger (F5):

```bat
cmake -S . -B build -A Win32 -DHALFLIFECLI_ENABLE_LAUNCH_GAME=ON
```

Open `build\halflife-cli.sln`, select `LaunchGame` as the startup project and
press F5. This builds `HalflifeCLI`, runs `DeployGame` (which stages the install
rules and deploys them with `MetahookInstallerCLI`, then registers the plugin in
`plugins.lst`), and launches `svencoop.exe` with `-insecure -game svencoop
-windowed -novid`. The PDB is deployed alongside the DLL, so breakpoints
resolve.

The game directory is resolved by `MetahookInstallerCLI` (Steam layout), not
hardcoded. All settings are cache variables:

| Variable | Default | Purpose |
|---|---|---|
| `HALFLIFECLI_GAME_APPID` | `225840` | Steam app ID (Sven Co-op) |
| `HALFLIFECLI_GAME_DIRECTORY` | *(empty)* | Game root; empty lets the CLI find the Steam install |
| `HALFLIFECLI_GAME_MOD` | *(empty)* | Mod directory; empty uses the app default |
| `HALFLIFECLI_GAME_ARGUMENTS` | *(empty)* | Extra game arguments appended to the defaults |
| `HALFLIFECLI_INSTALLER_CLI_EXECUTABLE` | *(empty)* | Use an existing `MetahookInstallerCLI.exe` instead of downloading one |

Deployment is plugin-only: it requires MetaHook to be installed in the game
already (as `install_plugin.bat` does) and never replaces the launcher or
`plugins.lst`'s existing entries.

## MCP server

`mcp/halflife_mcp.py` is a Model Context Protocol server (stdio transport)
that turns the game into callable tools for MCP clients. It launches the game
itself and stops managed games when the server shuts down. A game started
elsewhere is attached to through validated endpoint metadata and an auth probe;
server shutdown closes that connection without quitting the external game.

`launch_game` targets Sven Co-op by default. Pass `appid`, and `mod` when it is
not the app's default, to drive another app instead: `game_dir`, the mod
directory and the launcher then come from `MetahookInstallerCLI`'s
`-describe-target`, so `launch_game(appid=10, mod="cstrike",
game_dir="D:\\CS3266")` starts `MetaHook_blob.exe -insecure -game cstrike
-windowed -novid`. Screenshots, usermsg schemas and the RCON endpoint of every
later call follow the mod directory of the target the session launched.

Tools: `launch_game`, `game_status`, `run_command`, `find_cvar`,
`send_key` / `send_mouse` (key and mouse-button events through the engine's
SDL2 event queue or the original `CGame::WindowProc`), `read_console`, `snapshot`
(returns a PNG image, downscaled by default), `quit_game`, and the UserMsg
monitor tools `usermsg_status` (hook health),
`usermsg_messages` (merged schema layouts), `usermsg_events` (decoded traffic,
paged by `since_seq`), `usermsg_set_display`, and `usermsg_reload_schema`.

`run_command(command, max_lines=200, keep="head")` returns at most 200 output
lines by default. Set `max_lines=-1` for unlimited output or `max_lines=0` to
return only the saved-file marker. `keep` selects the first lines (`head`),
last lines (`tail`), or both halves (`both`; an odd limit keeps one extra line
at the start). The marker is an additional line beyond the requested limit.

Only output exceeding the limit is truncated and saved in full as UTF-8 under
the system temporary directory, in `halflife-mcp/commands/`. The marker reports
the omitted line count and log path; search that file for details instead of
raising the limit. Filenames are unique across MCP processes. If saving fails,
the capped output is still returned with the error in the marker. Logs have no
automatic cleanup. Internal Manager calls keep their unlimited default so
`find_cvar`, input injection and UserMsg parsing receive complete responses.

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

### Registering the server

No client config file is committed: the script path is machine-local, so
registering the server is an installation step on the machine that built the
checkout, not something the repository can carry. Register it at **user scope**
so it stays available in every project, with an absolute path to this checkout:

```bat
claude mcp add -s user halflife -- uv run --script D:\halflife-cli\mcp\halflife_mcp.py
codex mcp add halflife -- uv run --script D:\halflife-cli\mcp\halflife_mcp.py
```

Both write that machine's own client config, and `claude mcp remove -s user
halflife` / `codex mcp remove halflife` undo them. The absolute path is
required rather than cosmetic: a relative path is resolved against the
directory the client was started from, so it would only work when that happens
to be the repository root. The script itself is cwd-independent — it finds its
package and the repository root through `__file__`.

Easiest is to let an agent do it — paste this into Claude Code or Codex from
anywhere:

> Register the halflife MCP server at user scope: run `uv run --script <abs
> path to this checkout>\mcp\halflife_mcp.py` through `claude mcp add -s user
> halflife` (or `codex mcp add halflife`), then confirm with `claude mcp list`
> / `codex mcp list` that it is enabled.

The first `uv run` downloads the dependencies, which can take a minute.

## Using it
- **stdin**: every line is executed as a game console command (`status`,
  `map osprey`, `screenshot`, `quit`, ...). Plugin commands: `cli.help`,
  `cli.rconinfo`, `cli.window <0|1|2>` (0=show, 1=off-screen default,
  2=SW_HIDE; note SW_HIDE may pause rendering on some engines),
  `cli.inputlock [on|off]` (lock the mouse cursor so it stops driving the
  view; no argument reports the state), `cli.blockinput [on|off]` (make the
  game ignore the physical keyboard and mouse buttons; no argument reports
  the hook state and blocked-event counters), `cli.trapkey <key> <0|1>` and
  `cli.trapmouse <buttons> <0|1>` (inject a key / mouse-button event through
  the engine's own input path; they pass `block_input`), and
  `cli.find <name>` to check whether a cvar/command exists — when it does
  not, up to 10 similar names (substring match or small edit distance) are
  suggested, e.g. `cli.find abc` → `similar names: ab, ac, bc, c`.
- **stdout mirror**: all captured console output is echoed live, including
  `Con_DPrintf` output when the engine routes it to the vgui console (the
  plugin sets `developer 1` by default so it does).
- **Fatal errors**: the engine's `Sys_Error` is hooked — MetaHook reports its
  own load failures through the same function — so the message is mirrored as
  `[halflife-cli] sys_error: ...` and appended to
  `<mod>\metahook\configs\halflifecli\errors.log` before the game goes down.
  The original is called last, so the dialog and the exit path are unchanged.
  Errors raised before this plugin loads (e.g. MetaHook failing to read its
  own gamedata) stay out of reach, and a `warning: Sys_Error not hooked` line
  reports an engine whose gamedata has no such symbol.
- **UserMsg monitor**: every server user message the game receives is decoded
  per a TOML schema and printed as one `[usermsg] Name size=N field=value ...`
  console line (see below). `cli.usermsg` reports the hook state;
  `cli.usermsg on|off|reload|list|pending|<name>` controls it.
- **RCON**: `python mcp/rcon_client.py --protocol goldsrc-udp <host> <port> <password> "cmd" ...`
  on Native UDP engines; select `--protocol source-tcp` for the TCP backend.
  External clients of a Native UDP engine must speak GoldSrc challenge-based
  UDP, not Source TCP.
  `cli.rconinfo` reports protocol, actual binding, state and password presence.

Native UDP binding is selected by the engine (`ip`, `hostport`, `port`, Sven's
`ip_hostport`, and `-port`), after startup configuration executes. The plugin
does not rebind or own the socket. Old `[rcon].bind` and `.port` keys produce a
deprecation notice and are ignored on Native UDP; they retain their old
semantics on TCP.
The configured password is assigned to `rcon_password` once at startup;
subsequent engine cvar changes take precedence. Empty passwords are accepted
only while CLI RCON is running, from `NA_LOOPBACK` or exactly `127.0.0.1`, and
only if permitted by `allowed_ips`. Other `127/8` addresses are not local.
Nonempty passwords retain native challenges, failure accounting and bans.

Discovery keeps the single-line `halflifecli.port` for compatibility and adds
`halflifecli.endpoint.json` alongside it (version 1: protocol, actual binding,
port, status, PID, Windows process creation FILETIME and publication time).
MCP validates process identity and metadata freshness, maps `0.0.0.0` to
`127.0.0.1`, and probes authentication before reporting ready. A legacy file
without metadata means **Source TCP only**. Invalid or stopped metadata never
falls back to the old file or triggers a protocol guess.

UDP replies are OOB `A2C_PRINT` datagrams with at most 1200 text bytes each.
The client serializes commands and uses a new socket/challenge for every
command to isolate late replies. It aggregates arrival order until 100 ms of
silence (10 s total timeout by default; 1 MiB output cap). UDP cannot guarantee
complete, ordered or duplicate-free output; silence can truncate delayed
replies. A total timeout with partial output raises `PartialResponseError`.
Already-sent commands are never automatically retried, including `map` and
`quit`. Readiness uses the read-only `version` command.

Input blocking uses `SDL_SetEventFilter` on SDL2 engines (including
sdl2-compat), or a gamedata-resolved inline hook on `CGame_WindowProc` on
legacy engines. SDL exports are resolved with `GetProcAddress`; the prior
SDL filter is chained and restored on exit. Injection uses `SDL_PushEvent`
or the original WindowProc trampoline, without generating OS input. There
is no separate VGUI input filter. Missing backends fall back to disabling
the game window.

`cli.mousemove absolute <x> <y>` positions the UI cursor in original screenshot
pixels; `cli.mousemove relative <dx> <dy>` adds a displacement, and
`cli.mousemove` queries the position. Both SDL2 and legacy WindowProc engines
support this. Coordinates are scaled to the client area and clamped to its
bounds. Subsequent `cli.trapmouse` clicks use that position. This is UI motion;
use `+left` / `+right` / `+lookup` / `+lookdown` for FPS view turning.

On legacy engines, engine/VGUI `GetCursorPos` imports read the virtual cursor
in desktop coordinates without moving the real mouse. Their `SetCursorPos`
warps are swallowed while the virtual cursor owns the position, including
recentres on later frames. Client view sampling retains its separate
`input_lock` behavior. A MetaHook DLL-load notification installs cursor hooks
for VGUI DLLs loaded after `LoadClient`, so per-frame UI polling preserves the
injected control's mouse focus. Allowed physical mouse motion takes ownership back;
`cli.blockinput on` keeps physical motion from taking over. A legacy backend
missing the engine's cursor imports reports an error for motion, while its
existing key/button injection and window fallback behavior remain available.

Key names use engine `bind` names; numeric keys without a native keyboard
mapping are rejected. Mouse masks describe all held buttons, and are
translated into individual transitions. `MOUSE1`–`MOUSE5` use the same path
as `cli.trapmouse`, so client focus restrictions also apply to these keys.
Wheel press generates one pulse; release does nothing. sdl2-compat needs the
integer wheel conversion fix from `sdl2-compat-fork` commit `c24acad` (or a
version containing it). Unpatched builds such as 2.32.57 lose the integer
delta on a push/read roundtrip despite preserving the precise delta.

## UserMsg monitor

The plugin wraps the user-message hooks the client game DLL registers with the
engine (`g_pMetaHookAPI->HookUserMsg`), decodes each payload per a schema
description, prints one line per message, and forwards the message to the game
untouched. Messages the game DLL never hooks are registered display-only, so
server-only traffic (e.g. svencoop `DeathMsg`, `SelAmmo`) is visible too.
Every message is also recorded into the plugin's channel-keyed event ring
(512 events per channel, sequence numbers shared across channels). Each
message's channel is its functional group, set by the optional `channel` key
of its `[[usermsg]]` definition: `weapon` (guns/ammo/inventory), `status`
(vitals/equipment), `text` (chat/text), `score` (scoreboard/teams/deaths),
`screen` (fade/shake/camera), `world` (temporary-entity effects), `hud`
(widgets/menus/timers), `meta` (server/session info); definitions without a
channel record under `usermsg`. Which channels are echoed to the console
while recorded is configured with `[usermsg] display_channels` (`all` =
wildcard); `cli.usermsg events` queries one channel or the merged ring:

```text
cli.usermsg                              # schema + hook state report
cli.usermsg on|off                       # live console printing (recording unaffected)
cli.usermsg events [channel C|all] [since N] [limit N] [name X]
                                         # recorded events as "#seq [usermsg] ..." lines
cli.usermsg list|pending|<name>          # per-message state / channel / field layout
cli.usermsg reload                       # re-read the schema TOML
```

If the schema file for the running mod does not exist, the plugin prints a
notice and disables usermsg collection entirely (nothing is hooked or
recorded). Installing the schema and running `cli.usermsg reload` — or just
changing maps, which re-runs the load — re-enables it.

Schemas live in `svencoop\metahook\configs\halflifecli\usermsgs\<gamedir>.toml`
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
through the engine's client command entry. `install_plugin.bat` ensures
`VGUI2Extension.dll` is listed first in
`plugins.lst` and warns when the DLL itself is missing.

## Configuration

`svencoop\metahook\configs\halflifecli\halflifecli.toml` (all optional):

```toml
[rcon]
password = ""               # Native UDP: empty only permits exact localhost, subject to allowed_ips
allowed_ips = ""            # comma-separated IPv4 allowlist; empty = no additional address restriction
# Source TCP only (deprecated/ignored on Native UDP):
# port = 0                  # independent ephemeral TCP port
# bind = "127.0.0.1"

[cli]
hide_window = 1             # 0=off 1=off-screen (default) 2=SW_HIDE
block_input = true          # true = the game ignores the physical keyboard/mouse buttons (cli.blockinput toggles at runtime; cli.trapkey/cli.trapmouse still inject)
input_lock = true           # true = lock the mouse so it stops driving the view (cli.inputlock toggles at runtime)
developer = 1               # set the developer cvar at startup
console_topmost = false     # keep the CLI console window always on top
rcon = true                 # false leaves native engine networking/authentication untouched

[usermsg]
enabled = true              # hook + decode server user messages
file = ""                   # schema file; empty = "<gamedir>.toml" in configs/halflifecli/usermsgs/
max_string = 64             # truncate decoded strings longer than this
display_channels = "all"    # channels echoed to the console while recorded (comma separated; "all" = wildcard, empty = record only)
```

## CI

Both workflows build the plugin on `windows-latest` (Win32) and package a 7z
that mirrors the game directory, using the same `cmake --install` layout the
`DeployGame` target uses:

- [`.github/workflows/livebuild.yml`](.github/workflows/livebuild.yml) — every
  push to `main`/`dev` and every PR; uploads `HalflifeCLI-windows-x86.7z` as a
  workflow artifact.
- [`.github/workflows/msbuild.yml`](.github/workflows/msbuild.yml) — `v*` tags;
  publishes a GitHub Release with the same 7z.

The archive contains the plugin DLL, its PDB, the `halflifecli.toml` config
template, the usermsg schemas and the required gamedata
(`svencoop/metahook/{plugins,configs/halflifecli,gamedata/halflifecli}`), so
extracting it into a Sven Co-op root installs everything:

```bat
git tag v0.1.0
git push origin v0.1.0
```

The gamedata catalog covers every engine version in the manifest (about 11 KB
per pruned snapshot): Native UDP RCON and the focus lock resolve their
`hw.dll` symbols from it. An engine build outside it loads the plugin and uses
the independent Source TCP RCON backend (see `src/rcon/rcon_server.cpp`).

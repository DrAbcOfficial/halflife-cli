---
name: halflife-cli
description: Operate, automate, and debug Sven Co-op / Half-Life through the halflife-cli MetaHookSv plugin — piped-stdin console bridge, native GoldSrc UDP / legacy Source TCP RCON, and in-engine screenshots. Use whenever the task involves launching or driving the game headlessly, sending console commands (map, status, quit, cvars), capturing game screenshots (the `screenshot` command or the MCP `snapshot` tool) for vision-based verification, connecting over RCON, or diagnosing why the plugin, the CLI bridge, or the game misbehaves — even if the user just says "take a screenshot of the game", "run the map", or "test the plugin".
---

# halflife-cli: drive Sven Co-op / Half-Life as a CLI program

halflife-cli turns the game into a controllable process: the render window is
hidden off-screen (still rendering, so screenshots keep working), every stdin
line is executed as a console command, all console output is mirrored to
stdout. Windows x86 Sven 8948/10257, Half-Life 3248-10210 (and the mods on
its engine) and Cry of Fear 5936 use native GoldSrc UDP RCON on the engine's
server socket, including at the menu; this needs MetaHook API 115+ and the
manifest-validated gamedata catalog. Sven never falls back to TCP; a GoldSrc
build the catalog does not cover keeps Source TCP and prints
`Native UDP unavailable (...); using Source TCP`. Once Native UDP is selected,
missing symbols fail startup without falling back.

## Paths you will need

| What | Where |
|---|---|
| Game install | resolved externally — see "Resolve the game directory first"; the default Sven Co-op install contains `svencoop.exe` |
| Mod dir | `<game>\svencoop` for Sven Co-op; another GoldSrc app uses its own mod dir (e.g. `<game>\cstrike`) and starts through `MetaHook_blob.exe` |
| Screenshots | `<game>\<mod>\screenshots\*.tga` (the engine `screenshot` command; format varies by mod) |
| RCON port file | `<game>\<mod>\metahook\configs\halflifecli\halflifecli.port` |
| RCON metadata | `<game>\<mod>\metahook\configs\halflifecli\halflifecli.endpoint.json` |
| Config file | `<game>\<mod>\metahook\configs\halflifecli\halflifecli.toml` |
| Game dir config | `<repo>\mcp\game_dir.txt` for the Python tooling, `<repo>\scripts\game_dir.txt` for the `.bat` launchers (one line each, machine-local, gitignored) |
| RCON client / acceptance test / locator | `mcp\rcon_client.py`, `mcp\acceptance_test.py`, `mcp\find_game.py` in this repo |
| MCP server | `mcp\halflife_mcp.py` (stdio; run with `uv run --script`), registered per machine at user scope (`claude mcp add -s user` / `codex mcp add`) |

## Resolve the game directory first

No install path is hardcoded anywhere in this project. Resolve it at the start
of every task, before launching the game or running any script:

1. If the user already gave the path in this conversation, use it.
2. Otherwise run `python mcp\find_game.py`. It checks `GAME_DIR` and
   `mcp\game_dir.txt` first, then searches (Steam registry →
   `steamapps\libraryfolders.vdf` libraries → common install layouts) and
   prints the first directory containing `svencoop.exe`. Pass `--dir <path>`
   to merely validate a candidate.
3. For any app other than Sven Co-op, pass its Steam app id and, when it is
   not the app default, its mod: `python mcp\find_game.py --appid 10 --mod
   cstrike`. Resolution then goes through `MetahookInstallerCLI`, which knows
   the app's default mod and also reports the launcher; the printed path is
   still the game root. Pass the same appid/mod to `launch_game` (see "MCP
   tools"): a non-Sven target starts its described launcher
   (`MetaHook_blob.exe -insecure -game <mod>`), never `svencoop.exe`.
4. If it exits non-zero, ask the user for the install path — do not guess.
   Then:
   - Verify the answer: a Sven install must contain `svencoop.exe` (and
     `svencoop\metahook\` if the plugin still needs installing); another app
     must contain its mod's `liblist.gam` and the MetaHook launcher.
   - Persist it as a single line in `mcp\game_dir.txt` so later sessions
     never need to ask again:

     ```bat
     echo C:\Program Files (x86)\Steam\steamapps\common\Sven Co-op>"<repo>\mcp\game_dir.txt"
     ```

Every script and snippet in this skill takes the game directory from that
resolution chain — never invent a default location.

## Launch and discover

Automation harnesses must launch `svencoop.exe` directly with piped stdio (the
way `mcp/acceptance_test.py` does), with `game_dir` resolved as above. Do
not use `launch_cli.bat` for piping — it uses `start`, which detaches the
console.

```python
proc = subprocess.Popen(
    [os.path.join(game_dir, "svencoop.exe"), "-windowed", "-novid"],
    cwd=game_dir, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
```

`-windowed` is mandatory (off-screen hiding requires windowed mode) and
`-novid` skips the intro. The plugin enforces nothing on your behalf here —
always pass both.

For any app other than Sven Co-op, let the resolved target build the command
line instead of hardcoding the executable: `build_game_argv` starts the
launcher MetahookInstallerCLI reported, with the mod it named.

```python
from find_game import resolve_target
from game_process import build_game_argv

target, reason = resolve_target(game_dir, appid=10, mod="cstrike")  # appid/mod: non-Sven only
proc = subprocess.Popen(
    build_game_argv(target), cwd=target.directory,
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
```

Then read stdout until you see the banner (allow up to ~120 s for game start):

```
halflife-cli: RCON listening on 0.0.0.0:27015 (goldsrc-udp, password: none|set)
```

Read `halflifecli.endpoint.json` through `halflifecli.plugin_config.read_endpoint_file`.
It validates ready status, protocol, PID and process creation time. Use its
host/port (wildcard bindings map to 127.0.0.1), then perform an authenticated
read-only probe. Never treat the banner or port file alone as ready. Missing
metadata with a legacy port file means Source TCP only; invalid metadata must
not fall back to that port or trigger protocol guessing.

## MCP tools (preferred)

When an MCP server named `halflife` is available, prefer its tools over
shelling out — they launch and supervise the game, so a game started by the
MCP server is cleaned up on server shutdown and its console stream is readable
via `read_console`. Tool ↔ manual mapping:

| MCP tool | Replaces |
|---|---|
| `launch_game` | the `subprocess.Popen` + banner-wait launch loop (pass `appid`/`mod` for a non-Sven app; Sven Co-op is the default) |
| `run_command` | `rcon_client.py` / `run_command` |
| `find_cvar` | `cli.find <name>` over RCON |
| `send_key` / `send_mouse` | `cli.trapkey` / `cli.trapmouse` over RCON (see "Sending keyboard and mouse input") |
| `read_console` | reading the piped stdout mirror (only for MCP-launched games) |
| `snapshot` | the engine `screenshot` command + locating the newest image + converting to PNG (returns a PNG image directly, downscaled by default) |
| `quit_game` | `quit` over RCON, then stdin, then kill |
| `game_status` | manual process/port inspection |
| `usermsg_status` / `usermsg_messages` / `usermsg_events` / `usermsg_set_display` / `usermsg_reload_schema` | `cli.usermsg` over RCON + reading `configs/usermsgs/*.toml` by hand |

Only fall back to the manual channels below when no `halflife` MCP server is
connected, or for games that were not started by the MCP server (attached games
expose no `read_console` stream).

## UserMsg monitor

While connected to a server, the plugin decodes every server user message per
a TOML schema (one file per `-game` folder in
`svencoop\metahook\configs\halflifecli\usermsgs\`, maintained in the repo at
`configs/usermsgs/`) and records the last 512 per channel into a
channel-keyed event ring. A message's channel is its functional group from
the schema's `channel` key (weapon, status, text, score, screen, world, hud,
meta; untagged ones land in "usermsg"). A missing mod schema disables
usermsg collection (notice on the console; install the schema, then
`cli.usermsg reload` or a map change re-enables it). Watch traffic with
`usermsg_events(since_seq=..., name=..., channel=...)` (page forward with
`since_seq=<previous>.newest_seq`) or, without MCP, `cli.usermsg events
[channel C|all] [since N] [limit N] [name X]` over RCON/stdin; `cli.usermsg`
reports hook
health (wrapped = intercepted the game DLL's hook, self-registered =
display-only entry for messages the game DLL never hooks). Messages only flow
while a server connection is up. `python mcp\usermsg_test.py
[--connect HOST:PORT]` runs the plugin-level acceptance flow.

## Control channels

**RCON (preferred for request/response).** Select the protocol explicitly:

```bat
python mcp\rcon_client.py --protocol goldsrc-udp 127.0.0.1 27015 "" "status" "echo hello"
```

In Python use `RconConnection(host, port, password, protocol=endpoint.protocol)`;
`open()` authenticates (`version` for UDP), `command(text)` executes, and
`close()` releases the socket. The low-level `connect`/`run_command` helpers
are TCP-only.

- Sven binding comes from engine `ip`/`ip_hostport`/`hostport`/`port` or `-port`.
  Legacy `[rcon].bind` and `.port` are ignored. Read the actual endpoint.
- The configured password initializes `rcon_password`; later cvar changes win.
  Empty password permits only exact 127.0.0.1 or NA_LOOPBACK while CLI is running
  and the allowlist permits the source. Other 127/8 sources are not local.
- UDP uses native challenges, failure accounting and bans. Replies are native
  redirected output, independent of the stdout capture plugin.
- UDP requests are limited to 510 bytes after the OOB header. Replies use
  <=1200 text bytes per datagram and double NUL termination. The client allows
  1 MiB aggregate output, 100 ms silence and 10 s total time by default.
  UDP has no sequence IDs or completion frame: loss, reordering, duplicates
  and delayed-output truncation remain possible. Partial total timeouts raise
  `PartialResponseError` with `partial_output`.
- Never automatically retry an already-sent UDP command, particularly `map`
  or `quit`. New commands use a new socket/challenge to isolate late replies.
  An empty reply is valid. If quit gets no reply, wait for process exit first.
- The TCP backend uses `--protocol source-tcp`, retaining the 4096-byte
  response limit, up to four connections, localhost default and prior password
  behavior. The banner and `halflifecli.endpoint.json` name the protocol.

**stdin (fire-and-forget).** Every line piped to stdin is queued and executed
on the next frame. There is no per-command response framing on stdin — output
simply appears interleaved on stdout. Use RCON when you need the answer to a
specific command; use stdin for one-way pushes.

**stdout mirror.** All captured console output is echoed live, including
`Con_DPrintf` output when the engine routes it to the vgui console (the
plugin sets `developer 1` by default so it does). Capture rides the
VGUI2Extension plugin's GameConsole interface callbacks — `VGUI2Extension.dll`
must be installed and listed in `plugins.lst` (the installer ensures both).
This is your main observability channel. It also carries
`[halflife-cli] sys_error: ...`: the engine's `Sys_Error` (the path MetaHook
reports its own load failures through) is hooked, so a fatal error's text is on
stdout — and appended to `<mod>\metahook\configs\halflifecli\errors.log` — even
though the game is about to die. Errors raised before this plugin loads are out
of reach.

**Plugin commands** (alongside all normal game commands):
`cli.help`, `cli.rconinfo` (current RCON endpoint), `cli.window <0|1|2>`
(0=show, 1=off-screen default, 2=SW_HIDE), `cli.inputlock [on|off]` (mouse
motion stops driving the view), `cli.blockinput [on|off]` (the game ignores
the physical keyboard and mouse buttons; no argument reports the hook state
and blocked-event counters), `cli.trapkey <key> <0|1>` /
`cli.trapmouse <buttons> <0|1>` (inject input, see "Sending keyboard and
mouse input"), and `cli.find <name>` — check
whether a cvar or console command exists before using it. On a hit it prints
what it is and its value (passwords masked as `**`), e.g.
`cli.find: "sv_cheats" exists (cvar, value "0")`; on a miss it prints up to
10 similar names ranked by similarity (substring match or small edit
distance), e.g. `cli.find: no cvar or command named "abc"` followed by
`cli.find: similar names: ab, ac, bc, c`. Case-insensitive. `cli.help` also
doubles as a liveness probe — if its output comes back, the bridge works.

## Sending keyboard and mouse input

To press keys or mouse buttons in the game, use the MCP tools `send_key` /
`send_mouse`, or without MCP the plugin commands `cli.trapkey` /
`cli.trapmouse` over RCON or stdin. They inject with `SDL_PushEvent` on SDL2
engines, or call the original `CGame::WindowProc` on non-SDL engines.

**Do not simulate input with Win32**: no `SendInput`, `keybd_event`,
`mouse_event`, or posted/sent `WM_KEYDOWN` / `WM_LBUTTONDOWN` messages. The
game window is off-screen and unfocused by default, so OS-level input goes to
whatever window has focus (the user's editor, browser, ...), and
`block_input` drops it anyway. The engine-level API needs no focus, no
visible window, and passes `block_input`.

| Want | MCP | Plugin command |
|---|---|---|
| Tap a key (press, hold, release) | `send_key(key="SPACE")` (`hold_ms`, default 100) | `cli.trapkey SPACE 1`, then `cli.trapkey SPACE 0` |
| Hold a key across other calls (e.g. walk) | `send_key(key="w", action="press")` … `send_key(key="w", action="release")` | `cli.trapkey w 1` … `cli.trapkey w 0` |
| Fire / use a mouse-button bind | `send_key(key="MOUSE1")` | `cli.trapkey MOUSE1 1` / `0` |
| Raw mouse-button state | `send_mouse(buttons=1)` | `cli.trapmouse 1 1`, then `cli.trapmouse 0 0` |

- `key` is a `bind` key name: a single character (`w`, `1`, `e`), `SPACE`,
  `ENTER`, `ESCAPE`, `TAB`, `SHIFT`, `CTRL`, `ALT`, `F1`–`F12`, `UPARROW`,
  `MOUSE1`–`MOUSE5`, `MWHEELUP` / `MWHEELDOWN`, `SEMICOLON`, `KP_*`, … or a
  keynum 0–255. An unknown name answers `cli.trapkey: error: unknown key`.
  Numeric keynums without a native keyboard mapping return an injection error.
- `cli.trapmouse <buttons>` is the held-button mask *after* the event
  (1=left 2=right 4=middle 8=mouse4 16=mouse5), exactly as the engine passes
  it; release with `cli.trapmouse 0 0`. The mask determines individual native
  button transitions. It reaches the client's
  `IN_MouseEvent`, which Sven Co-op ignores while the game window is not
  focused (the normal state for a hidden window), the same as a real click.
  `send_key("MOUSE1")` / `cli.trapkey MOUSE1` uses the same native mouse path
  and has the same focus restriction.
- Native events reach gameplay and VGUI. Key events do not synthesize text
  input events; use console commands for text entry.
- Wheel press is one pulse (the engine generates both key edges); release
  does nothing. sdl2-compat requires the integer wheel conversion fix from
  `sdl2-compat-fork` commit `c24acad` (or a version containing it). Unpatched
  2.32.57 loses the integer delta; patched builds pass both directions.
- There is no mouse *motion* injection. To turn or aim, use commands such as
  `+left` / `+right` / `+lookup` / `+lookdown` (speed from `cl_yawspeed` /
  `cl_pitchspeed`).
- Verify a key did something by binding it to an `echo` first
  (`bind k "echo K_PRESSED"`, then `send_key("k")`, then look for the line).

**`block_input`** (`[cli] block_input = true`, or `cli.blockinput on` at
runtime) makes the game ignore the physical keyboard and mouse buttons,
including in menus and the console, while the window stays movable and
resizable. Injected events still go through. `input_lock` is separate: it
stops physical mouse motion from turning the view. `cli.blockinput` without
arguments reports `engine hooks`, `window fallback`, and per-type counters
of physical events seen and blocked. SDL2 uses `SDL_SetEventFilter` and
chains the prior filter; non-SDL uses a gamedata-resolved inline hook on
`CGame_WindowProc`. Both block before gameplay/VGUI dispatch. Missing
backends fall back to disabling the game window (`window fallback=on`).

## Seeing the game: screenshots for vision-capable agents

If you can read images, capture and view the game like this:

1. Make sure a map is loaded and rendering. From a fresh start send
   `map osprey` (or any small map) and wait for the load — the acceptance test
   allows ~25 s after `map` before shooting. A screenshot at the main menu shows
   the menu, not the game.
2. Send `screenshot` (the engine's local-file screenshot command). In Sven
   Co-op this writes `<mod>\screenshots\<map>-<date>-NNNN.tga`. Do **not** use
   `snapshot` for local files: `SteamScreenshots.dll` hooks it and uploads to
   Steam instead of writing to disk. Locate results by filename, not assumed
   extension (format varies by engine/mod: `.tga`, `.bmp`, ...).
3. Wait 1–2 s, then find the newest image file by modification time:

```python
import glob, os
shots_dir = os.path.join(game_dir, "svencoop", "screenshots")
shots = [p for p in glob.glob(os.path.join(shots_dir, "*"))
         if p.lower().endswith((".tga", ".bmp", ".png", ".jpg", ".jpeg"))]
newest = max(shots, key=os.path.getmtime)
```

4. TGA/BMP are not readable by every harness. Convert to PNG with Python:
   `from PIL import Image; Image.open(shot).convert("RGB").save(png)`.
   (PowerShell's `System.Drawing` reads BMP but not TGA.)
5. Read the PNG with your image tool and reason about the frame.

The window is hidden off-screen (mode 1) yet still rendered by the OS, so
screenshots work with no visible window. Do not switch to `cli.window 2`
(SW_HIDE) for capture — some engines pause rendering when fully hidden and
screenshots stop updating. `cli.window 0` restores the window on screen if you
need to watch it.

## Operating the game

Useful console commands (send via RCON or stdin): `status`, `version`,
`map <name>`, `echo <text>` (pipeline probe), cvars via `<name> <value>`, and
`quit` for clean shutdown. Verify a cvar/command name with `cli.find <name>`
first — the engine silently ignores a typo'd command, and `cli.find` tells
you the correct spelling instead. Quit via RCON `quit` first; fall back to writing
`quit\n` to stdin if the socket path fails, then wait up to ~30 s for exit.

Config lives in `<game>\svencoop\metahook\configs\halflifecli\halflifecli.toml` (all keys
optional):

```toml
[rcon]
password = ""               # Sven: empty permits allowed local sources only
allowed_ips = ""
# Other engines only: port = 0, bind = "127.0.0.1"

[cli]
hide_window = 1             # 0=off 1=off-screen (default) 2=SW_HIDE
block_input = true          # true = the game ignores the physical keyboard/mouse buttons (injected input still passes)
input_lock = true           # true = physical mouse motion stops driving the view
developer = 1
console_topmost = false     # keep the CLI console window always on top
```

Build and install from the repo root:

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release
scripts\install_plugin.bat              ; optional arg: game dir
```

The build must be Win32 (x86) — the game is a 32-bit process; an x64 DLL will
not load. `install_plugin.bat` copies the DLL into
`svencoop\metahook\plugins\`, appends it to `plugins.lst` once, and ensures
`VGUI2Extension.dll` is installed and listed first (console capture depends
on its GameConsole callbacks).

## Debugging: symptoms → causes → fixes

Run the full end-to-end check first, with the game directory resolved:
`python mcp\acceptance_test.py` (pass `--game <path>` to override). It
exercises auth (wrong and right password), echo, version, `map` + `screenshot`
file appearance, and clean quit.

| Symptom | Likely cause | Fix |
|---|---|---|
| `no game directory resolved` | nothing configured and the search found nothing | Ask the user for the path; verify and persist it (see "Resolve the game directory first") |
|---|---|---|
| No RCON banner within ~120 s | Plugin not loaded: missing DLL, x64 build, or not listed in `plugins.lst` | Check stdout for `halflife-cli ... loaded`; rebuild Win32; re-run `install_plugin.bat` |
| Game dies during startup with no visible reason | A fatal error (engine, MetaHook or another plugin) | Read `[halflife-cli] sys_error: ...` on stdout and `<mod>\metahook\configs\halflifecli\errors.log`: the plugin hooks the engine's `Sys_Error` (MetaHook reports its own load failures through it) and mirrors the message before the process goes down |
| `RCON failed to start (bind failed ...)` | Port conflict | Set a fixed `[rcon] port` in the ini, or kill the process holding it |
| `warning: VGUI2Extension.dll missing or incompatible, console output mirroring disabled` | Console capture plugin absent/incompatible | Sven UDP replies still work through native redirect; TCP output capture requires VGUI2Extension.dll. |
| RCON auth fails immediately | Password mismatch | Check the ini; note one-strike disconnect — open a fresh connection |
| Response is `timed out waiting for command output` | Engine busy (loading, paused) | Wait and resend; lengthen the wait after `map` |
| Screenshot file never appears | Used `snapshot` (SteamScreenshots.dll uploads it to Steam, no local file), no map loaded, or window fully hidden (`cli.window 2`) pausing render | Use `screenshot`; load a map and wait; use off-screen mode 1; wait 1–2 s after the command |
| Port file disagrees with banner | Stale file from a previous run | Trust the banner; the file is refreshed at startup |
| Game ignores stdin commands | stdin not actually piped (launched via `start`/bat) | Launch `svencoop.exe` directly with piped stdio, as in the acceptance test |
| Game does not exit after `quit` | Shutdown stalled or command was not delivered | If UDP says the command may have executed, wait before an explicit stdin fallback. Inspect stacks; kill only a task-owned process as a last resort. |

Source map (one src/ folder per responsibility), for code-level debugging:
`src/core/plugins.cpp` (lifecycle, banner, export overrides),
`src/core/cli_commands.cpp` (`cli.*` commands, cli.find suggestions),
`src/console/console_bridge.cpp` (stdin queue, command pump via
`pfnClientCmd`, RCON response assembly — responses complete on the frame
after execution), `src/console/output_capture.cpp` (console capture via
VGUI2Extension GameConsole callbacks; no engine code hooks),
`src/rcon/rcon_server.cpp` (backend selection, metadata, lifecycle),
`src/rcon/native_udp.cpp` (native UDP hooks per address layout, socket
polling, redirect, HL 10210 fallbacks), `src/rcon/rcon_policy.h` (locality,
allowlist, packet dispatch and failure-accounting rules),
`src/rcon/tcp_server.cpp` (legacy Source TCP),
`src/window/window_manager.cpp` (hide modes, window-level input fallback),
`src/input/engine_input.cpp` (`block_input` SDL filter / CGame WindowProc hook,
`cli.trapkey` / `cli.trapmouse` injection), `src/input/input_lock.cpp`
(`input_lock` mouse-view freeze),
`src/config/config.cpp` (TOML config parsing),
`src/util/toml_file.cpp` (engine-filesystem TOML reading shared by config and
schema), `src/usermsg/usermsg_schema.cpp` (UserMsg TOML loader with extends
inheritance), `src/usermsg/usermsg_decoder.cpp` (wire decoder),
`src/usermsg/usermsg_monitor.cpp` (UserMsg hooking via
`g_pMetaHookAPI->HookUserMsg`, 512-event ring, `cli.usermsg` implementation),
`mcp/halflifecli/` (MCP implementation package; `halflife_mcp.py` is the
entry point), `mcp/acceptance_test.py` (reference automation harness).

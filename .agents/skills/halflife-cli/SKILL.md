---
name: halflife-cli
description: Operate, automate, and debug Sven Co-op / Half-Life through the halflife-cli MetaHookSv plugin — piped-stdin console bridge, Source RCON server, and in-engine screenshots. Use whenever the task involves launching or driving the game headlessly, sending console commands (map, status, quit, cvars), capturing game screenshots (the `screenshot` command or the MCP `snapshot` tool) for vision-based verification, connecting over RCON, or diagnosing why the plugin, the CLI bridge, or the game misbehaves — even if the user just says "take a screenshot of the game", "run the map", or "test the plugin".
---

# halflife-cli: drive Sven Co-op / Half-Life as a CLI program

halflife-cli turns the game into a controllable process: the render window is
hidden off-screen (still rendering, so screenshots keep working), every stdin
line is executed as a console command, all console output is mirrored to
stdout, and a Source RCON server listens on a random localhost port.

## Paths you will need

| What | Where |
|---|---|
| Game install | resolved externally — see "Resolve the game directory first"; a valid install contains `svencoop.exe` |
| Mod dir | `<game>\svencoop` |
| Screenshots | `<game>\svencoop\screenshots\*.tga` (the engine `screenshot` command; format varies by mod) |
| RCON port file | `<game>\svencoop\metahook\configs\halflifecli.port` |
| Config file | `<game>\svencoop\metahook\configs\halflifecli.toml` |
| Game dir config | `<repo>\mcp\game_dir.txt` for the Python tooling, `<repo>\scripts\game_dir.txt` for the `.bat` launchers (one line each, machine-local, gitignored) |
| RCON client / acceptance test / locator | `mcp\rcon_client.py`, `mcp\acceptance_test.py`, `mcp\find_game.py` in this repo |
| MCP server | `mcp\halflife_mcp.py` (stdio; run with `uv run --script`), registered via `.mcp.json` |

## Resolve the game directory first

No install path is hardcoded anywhere in this project. Resolve it at the start
of every task, before launching the game or running any script:

1. If the user already gave the path in this conversation, use it.
2. Otherwise run `python mcp\find_game.py`. It checks `GAME_DIR` and
   `mcp\game_dir.txt` first, then searches (Steam registry →
   `steamapps\libraryfolders.vdf` libraries → common install layouts) and
   prints the first directory containing `svencoop.exe`. Pass `--dir <path>`
   to merely validate a candidate.
3. If it exits non-zero, ask the user for the install path — do not guess.
   Then:
   - Verify the answer: the directory must contain `svencoop.exe` (and
     `svencoop\metahook\` if the plugin still needs installing).
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

Then read stdout until you see the banner (allow up to ~120 s for game start):

```
halflife-cli: RCON listening on 127.0.0.1:54321 (password: none|set)
```

The same port is written to `metahook/configs/halflifecli.port` (decimal port +
newline). The port file may be stale from a previous run — prefer the banner,
or re-read the file only after the banner appears.

## MCP tools (preferred)

When an MCP server named `halflife` is available, prefer its tools over
shelling out — they launch and supervise the game, so a game started by the
MCP server is cleaned up on server shutdown and its console stream is readable
via `read_console`. Tool ↔ manual mapping:

| MCP tool | Replaces |
|---|---|
| `launch_game` | the `subprocess.Popen` + banner-wait launch loop |
| `run_command` | `rcon_client.py` / `run_command` |
| `find_cvar` | `cli.find <name>` over RCON |
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
`svencoop\metahook\configs\usermsgs\`, maintained in the repo at
`configs/usermsgs/`) and records the last 512 per channel into a
channel-keyed event ring. A message's channel is its functional group from
the schema's `channel` key (weapon, status, text, score, screen, world, hud,
meta; untagged ones land in "usermsg"). Watch traffic with
`usermsg_events(since_seq=..., name=..., channel=...)` (page forward with
`since_seq=<previous>.newest_seq`) or, without MCP, `cli.usermsg events
[channel C|all] [since N] [limit N] [name X]` over RCON/stdin; `cli.usermsg`
reports hook
health (wrapped = intercepted the game DLL's hook, self-registered =
display-only entry for messages the game DLL never hooks). Messages only flow
while a server connection is up. `python mcp\usermsg_test.py
[--connect HOST:PORT]` runs the plugin-level acceptance flow.

## Control channels

**RCON (preferred for request/response).** Standard Source RCON protocol over
TCP; responses are synchronous, one response per command, body capped at 4096
bytes. From this repo:

```bat
python mcp\rcon_client.py 127.0.0.1 54321 "" "status" "echo hello"
```

Or in Python: `sys.path.insert(0, "<repo>/mcp"); import rcon_client`, then
`rcon_client.connect(sock, host, port, password)` and
`rcon_client.run_command(sock, rid, command)`.

- Empty configured password accepts any auth; a wrong password against a set
  password gets `rid=-1` and the connection is closed on the first strike —
  reconnect fresh rather than reusing the socket.
- A response of `[halflife-cli] timed out waiting for command output` means the
  engine did not produce output within 5 s (usually still loading or paused) —
  retry after waiting.
- An empty response body is normal for commands that print nothing (e.g. a
  successful `map`); it is not an error.
- Up to 4 concurrent connections are accepted; bind is localhost-only by
  default.

**stdin (fire-and-forget).** Every line piped to stdin is queued and executed
on the next frame. There is no per-command response framing on stdin — output
simply appears interleaved on stdout. Use RCON when you need the answer to a
specific command; use stdin for one-way pushes.

**stdout mirror.** All captured console output is echoed live, including
`Con_DPrintf` output when the engine routes it to the vgui console (the
plugin sets `developer 1` by default so it does). Capture rides the
VGUI2Extension plugin's GameConsole interface callbacks — `VGUI2Extension.dll`
must be installed and listed in `plugins.lst` (the installer ensures both).
This is your main observability channel.

**Plugin commands** (alongside all normal game commands):
`cli.help`, `cli.rconinfo` (current RCON endpoint), `cli.window <0|1|2>`
(0=show, 1=off-screen default, 2=SW_HIDE), and `cli.find <name>` — check
whether a cvar or console command exists before using it. On a hit it prints
what it is and its value (passwords masked as `**`), e.g.
`cli.find: "sv_cheats" exists (cvar, value "0")`; on a miss it prints up to
10 similar names ranked by similarity (substring match or small edit
distance), e.g. `cli.find: no cvar or command named "abc"` followed by
`cli.find: similar names: ab, ac, bc, c`. Case-insensitive. `cli.help` also
doubles as a liveness probe — if its output comes back, the bridge works.

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

Config lives in `<game>\svencoop\metahook\configs\halflifecli.toml` (all keys
optional):

```toml
[rcon]
port = 0                    # 0 = random available port
bind = "127.0.0.1"
password = ""               # empty = accept any auth
allowed_ips = ""

[cli]
hide_window = 1             # 0=off 1=off-screen (default) 2=SW_HIDE
developer = 1
console_topmost = 0         # 1 = keep the CLI console window always on top
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
| `RCON failed to start (bind failed ...)` | Port conflict | Set a fixed `[rcon] port` in the ini, or kill the process holding it |
| `warning: VGUI2Extension.dll missing or incompatible, console output mirroring disabled` | VGUI2Extension.dll not installed or not listed in `plugins.lst` (console capture depends on it) | Commands still run; RCON replies come back EMPTY. Install VGUI2Extension.dll into `svencoop\metahook\plugins\` (re-run `install_plugin.bat`, which also adds it to `plugins.lst`) |
| RCON auth fails immediately | Password mismatch | Check the ini; note one-strike disconnect — open a fresh connection |
| Response is `timed out waiting for command output` | Engine busy (loading, paused) | Wait and resend; lengthen the wait after `map` |
| Screenshot file never appears | Used `snapshot` (SteamScreenshots.dll uploads it to Steam, no local file), no map loaded, or window fully hidden (`cli.window 2`) pausing render | Use `screenshot`; load a map and wait; use off-screen mode 1; wait 1–2 s after the command |
| Port file disagrees with banner | Stale file from a previous run | Trust the banner; the file is refreshed at startup |
| Game ignores stdin commands | stdin not actually piped (launched via `start`/bat) | Launch `svencoop.exe` directly with piped stdio, as in the acceptance test |
| Game does not exit after `quit` | RCON path broken | Send `quit\n` on stdin as fallback, wait ~30 s, then kill as last resort |

Source map (one src/ folder per responsibility), for code-level debugging:
`src/core/plugins.cpp` (lifecycle, banner, export overrides),
`src/core/cli_commands.cpp` (`cli.*` commands, cli.find suggestions),
`src/console/console_bridge.cpp` (stdin queue, command pump via
`pfnClientCmd`, RCON response assembly — responses complete on the frame
after execution), `src/console/output_capture.cpp` (console capture via
VGUI2Extension GameConsole callbacks; no engine code hooks),
`src/rcon/rcon_server.cpp` (protocol, auth, limits),
`src/window/window_manager.cpp` (hide modes),
`src/config/config.cpp` (TOML config parsing),
`src/util/toml_file.cpp` (engine-filesystem TOML reading shared by config and
schema), `src/usermsg/usermsg_schema.cpp` (UserMsg TOML loader with extends
inheritance), `src/usermsg/usermsg_decoder.cpp` (wire decoder),
`src/usermsg/usermsg_monitor.cpp` (UserMsg hooking via
`g_pMetaHookAPI->HookUserMsg`, 512-event ring, `cli.usermsg` implementation),
`mcp/halflifecli/` (MCP implementation package; `halflife_mcp.py` is the
entry point), `mcp/acceptance_test.py` (reference automation harness).

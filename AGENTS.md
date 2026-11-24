# AGENTS.md

Agent-facing notes for this repository. They are deliberately short: the
authoritative detail lives in `README.md` and
`.agents/skills/halflife-cli/SKILL.md` (load that skill before driving the
game). Everything below assumes the game directory has already been resolved
(the user's path, `GAME_DIR`, `mcp/game_dir.txt`, or `python mcp/find_game.py`).

## What ships by default: UserMsg schemas

`configs/usermsgs/` contains one TOML per `-game` directory. The plugin picks
`<gamedir>.toml` (falling back to `valve.toml` when the game directory is
unknown); a mod with no schema file gets **UserMsg monitoring disabled**.

| Schema | `-game` dir | Covers |
| --- | --- | --- |
| `valve.toml` | `valve` | Half-Life; base file for every GoldSrc mod (`SelAmmo` is raw) |
| `cstrike.toml` | `cstrike` | Counter-Strike 1.x (CS-specific HUD, VGUI menus, tutor) |
| `czero.toml` | `czero` | Condition Zero — `extends = "cstrike.toml"` (same client/server sources) |
| `czeror.toml` | `czeror` | CZ: Deleted Scenes (own fog/particle/weather HUD) |
| `bshift.toml` | `bshift` | Blue Shift (`HudTextPro` is a bare string) |
| `gearbox.toml` | `gearbox` | Opposing Force (CTF stats, custom/flag icons, 5-byte `ScoreInfo`) |
| `tfc.toml` | `tfc` | Team Fortress Classic (two-short `Battery`, spectator fade) |
| `dod.toml` | `dod` | Day of Defeat (objective/control-point and score messages) |
| `dmc.toml` | `dmc` | Deathmatch Classic (CTF flags/runes, teleporter+fog `InitHUD`) |
| `ricochet.toml` | `ricochet` | Ricochet (Frozen/Powerup/Reward; round bookkeeping is raw) |
| `svencoop.toml` | `svencoop` | Sven Co-op; the only schema with `[primitives] coord_size = 4` |

Inheritance rules: a child file declares `extends = "valve.toml"` (or
`cstrike.toml`) and only overrides/adds messages by name. `czero.toml` has no
entries of its own. `svencoop.toml` overrides `coord_size` because Sven's
usermsg `READ_COORD` is a 32-bit long.

## Where schema definitions live

1. **Repo source of truth** — `configs/usermsgs/<gamedir>.toml`. Edit here.
2. **Installed copy the game actually reads** —
   `<game>/<mod>/metahook/configs/halflifecli/usermsgs/<gamedir>.toml`.
   The CMake install rule copies the whole directory. Do not edit installed
   copies long-term: change the repo, then reinstall
   (`scripts/install_plugin.bat`) or copy the file and run
   `cli.usermsg reload`.
3. **Grammar / loader** — `src/usermsg/usermsg_schema.cpp` (`extends`,
   `[primitives] coord_size`, `[[usermsg]]`, `channel`, `raw`,
   `fields`, types, `count`, `when`) and `src/usermsg/usermsg_schema.h`; the
   renderer is `src/usermsg/usermsg_decoder.cpp`. `group` fields nest;
   `count` may be an integer, `"*"` (read to the end of the buffer) or the name
   of a previously read field; `when = { field = "...", eq|ne = N }` gates a
   field or group.
4. **Runtime selection** — `[usermsg]` in
   `<game>/<mod>/metahook/configs/halflifecli/halflifecli.toml`
   (`enabled`, `file` (empty = `<gamedir>.toml`), `max_string`,
   `display_channels`). Repo defaults: `configs/halflifecli/halflifecli.toml`.
5. **Semantics reference** — when the checkouts are present beside this repo
   (`GoldSrc-SDK/`, `svencoop_sdk/`): the server side is authoritative for the
   wire (`REG_USER_MSG` + `MESSAGE_BEGIN`/`WRITE_*`, including gating), the
   client side for field order (`MsgFunc_*`/`READ_*`). A `count = "*"` tail or a
   `when` gate is how optional wire blocks are expressed.
6. **At runtime** — `cli.usermsg` (hook health), `cli.usermsg list`
   (merged layouts), `cli.usermsg <name>`, `cli.usermsg reload`; MCP
   equivalents are `usermsg_status`, `usermsg_messages`, `usermsg_events`,
   `usermsg_reload_schema`. Decoded traffic is printed as
   `[usermsg] Name size=N field=value ...` (a trailing `tail` means the schema
   under-reads; `truncated` means it over-reads).

## Where to confirm the RCON server

- **Config** — `[rcon]` in
  `<game>/<mod>/metahook/configs/halflifecli/halflifecli.toml`:
  `port`, `bind`, `password`, `allowed_ips`. These keys apply to the
  **Source TCP** backend only. The repo copy of the default config is
  `configs/halflifecli/halflifecli.toml`.
- **Native UDP engines** (Sven, cataloged Half-Life builds, Cry of Fear) use
  the engine's own socket: the endpoint comes from `ip`/`hostport`/`port`/
  `ip_hostport` or `-port`; `[rcon].bind`/`.port` are ignored.
- **Authoritative runtime endpoint** —
  `<game>/<mod>/metahook/configs/halflifecli/halflifecli.endpoint.json`
  (`status`, `protocol`, `bind`, `port`, `pid`, `process_start_filetime`) plus
  `halflifecli.port`. Treat the JSON as ready only when `status == "ready"`
  and its `pid`/process start match the process you launched; never infer
  readiness from the port file or the banner alone.
- **In game** — `cli.rconinfo` prints
  `RCON protocol=... status=... bind=... port=... password=...`; it works over
  stdio even when you do not have the RCON client yet.
- **Helpers** — `mcp/halflifecli/plugin_config.py` (`read_endpoint_file`),
  `mcp/rcon_client.py`, `mcp/game_process.py`, `mcp/acceptance_test.py`, and
  the MCP `game_status` / `attach_game` tools.

## Transport priority: stdio first, RCON second

### 1. Piped stdio — the default when *you* launch the game

Launch the executable with piped streams and keep the default
`[cli] console = true`:

```python
proc = subprocess.Popen(
    [exe, "-windowed", "-novid", "-redirect_fatalerror_to_log",
     "+max_queries_sec", "100", "+max_queries_sec_global", "1000",
     "+max_queries_window", "60"],
    cwd=game_dir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT)
proc.stdin.write(b"status\n"); proc.stdin.flush()
line = proc.stdout.readline()           # console mirror
```

- Every stdin line is executed as a game console command on the next frame;
  stdout mirrors all captured console output (including `developer 1`
  DPrintf). Plugin `cli.*` commands work here too.
- stdin has **no per-command framing**: output is asynchronous and
  interleaved. To attribute output, inject a unique marker command
  (`echo <marker>`) and wait for it, or wait for a known completion line.
- Readiness: wait for `halflife-cli: RCON listening on ...` (the banner names
  the protocol), or send `cli.rconinfo`. stdio keeps working when RCON is
  inactive; `console = false` disables the stdin reader and stdout mirror.
- Helpers: `mcp/game_process.py` (`GameProcess.send_stdin`, `tail`,
  `wait_for_banner`), `mcp/testcommon.py` (`launch_test_game`).

### 2. RCON — fallback when stdio is unavailable

Use RCON when you did not launch the process (attached/external game), have no
usable stdin/stdout pipe, `console = false`, or you need a strict
request/response pair. Resolve the endpoint from
`halflifecli.endpoint.json` (never guess host/port/protocol), then:

```bat
python mcp\rcon_client.py --protocol goldsrc-udp 127.0.0.1 27015 "" "status"
python mcp\rcon_client.py --protocol source-tcp 127.0.0.1 27015 "" "status"
```

- `--protocol` must equal the endpoint's `protocol`; do not probe or guess.
- Never automatically retry an already-sent UDP command (especially `map` or
  `quit`); open a new connection for a new command.
- RCON is still the channel for `send_key`/`snapshot`-style MCP tools when a
  console stream is not available.

### 3. MCP wrappers (when a `halflife` server is connected)

`launch_game` + `send_console`/`read_console` are the stdio path;
`run_command` and `find_cvar` are RCON. `halflife_info` returns the installed
schema list, the plugin config/RCON paths and the validated endpoint, so an MCP
client without the source tree still knows what ships and where to look.
Prefer the stdio path and use the RCON path only when no console stream exists
(attached games expose no `read_console`). `quit_game` tries RCON, then stdin,
then termination.

## Minimal workflow

1. Resolve the game directory (`GAME_DIR` → `mcp/game_dir.txt` →
   `python mcp/find_game.py`); never invent a path.
2. Launch with piped stdio (or `launch_game`), keep `console = true`.
3. Do everything over stdin/stdout first, using markers for correlation.
4. If stdio is unavailable, read `halflifecli.endpoint.json` and use
   `mcp/rcon_client.py` with the reported protocol.
5. For schema work: edit `configs/usermsgs/<gamedir>.toml`, validate the TOML
   (strict parser, e.g. Python `tomllib`), reinstall/copy, then
   `cli.usermsg reload` and inspect with `cli.usermsg list` / live traffic.

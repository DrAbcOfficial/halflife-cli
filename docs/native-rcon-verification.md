# Native UDP RCON verification

Native UDP first shipped for Windows x86 Sven 8948/10257 (2026-10-05, below).
[Engine family extension](#engine-family-extension-2026-10-06) records the
GoldSrc/Cry of Fear builds added afterwards.

## Sven 8948/10257 (2026-10-05)

Only **10257** was run in a real game; 8948 has symbol/ABI and catalog
coverage, not a runtime acceptance result.

### Published gamedata and build

The normal CMake build synchronized the official GoldSrc_VibeSignatures index
and passed the 32-record manifest gate for both snapshots. No locally invented
lifecycle records are needed. Published snapshot time: `2026-10-05T04:10:44Z`.
Pruned catalog SHA-256 values:

| Version | SHA-256 |
| --- | --- |
| 8948 | `04ad181c981799f5f9b492148be1c2c8287a6560de4784d0695f738294140dbd` |
| 10257 | `42db43b8dfcd3edcd722a88f4bd0171e80605af727d323b19277bac20a1a0140` |

`Host_Shutdown` and `NET_Shutdown` were delivered by upstream PR #331. The
10257 listen-server exit path can call `NET_Shutdown` before `Sys_ShutdownGame`;
therefore a late plugin exit notification alone is insufficient.

Commands run from the repository root:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
uv run --script mcp/tests/test_halflife_mcp.py
uv run --with 'mcp>=2.2,<3' --with 'pillow>=10' --with 'pywin32>=311' python mcp/tests/test_udp_rcon.py
```

Release build and both CTest tests passed. Python: 52 tests, 51 passed and the
opt-in game test skipped. Existing TCP test fixtures emit socket ResourceWarnings;
the assertions passed. `git diff --check` passed.

### Real game environment

- Sven engine build 10257; `hw.dll` SHA-256:
  `e3c7f374b70845fb6f45c05906e4b5fe3dc9f394ab37bb653501d3b6a3282596`.
- MetaHook API 115+. The user changed its normal exit contract to 0 (error 1).
  The locally rebuilt Release launcher installed for the final normal-exit test
  has SHA-256 `01ac705024653c6d08ea0c392c1a302f68e5108e87466bffefe7c609b96f3054`.
- VGUI2Extension, SteamScreenshots, SCModelDownloader and HalflifeCLI enabled.
- Renderer temporarily disabled with user approval. Its installed DLL requested
  `g_ChromeOrigin`, absent from its active catalog and current local manifest;
  startup stopped at an SDL fatal-error dialog. No Renderer compatibility claim
  is made. Each test wrapper restores the original plugin list.

`python mcp/acceptance_test.py` passed with this temporary plugin configuration:
wrong/correct password, menu echo/version, 20,171-character `cvarlist`, map
`osprey`, active-server echo, stable endpoint, local screenshot, schema reload,
disconnect, menu RCON and stdin, second map load, RCON quit, **exit code 0**,
stopped endpoint metadata and removal of the port file.

The opt-in MCP test also passed against the real game, including launch/status,
command, cvar lookup, key/mouse release, console reads, screenshot, all UserMsg
tools and quit with exit code 0:

```powershell
$env:HALFLIFE_E2E = '1'
uv run --script mcp/tests/test_halflife_mcp.py TestEndToEnd
```

Additional local wire tests passed: capture disabled, empty password accepted
only from 127.0.0.1, rejection from 127.0.0.2, runtime password change, nonlocal
valid/invalid challenges, allowlist filtering for challenge and command, engine
`-port 27029`, ignored legacy bind/port and matching actual endpoint metadata.
A full cvar list arrived in 29 datagrams, each <=1200 text bytes with OOB
`A2C_PRINT` framing and two trailing NUL bytes. Native invalid-challenge handling
also emits its original rejection packet before the redirected error response.

Native failed-password counting and banning passed using a fixed peer socket;
`listip` confirmed the ban, which was removed after the test. Runtime fault
injection removing `NET_Shutdown` produced failed endpoint metadata, no TCP
fallback and usable stdin; the official catalog bytes were restored afterward.
RCON-disabled and `-noip` controls correctly reported disabled/failed metadata
without a port file. An externally launched game could be attached over UDP;
manager shutdown left it running, and its later normal quit returned 0.

Configuration-dependent hooks now initialize in `LoadClient`, after engine
filesystem paths are available, rather than using defaults prematurely in
`LoadEngine`. Capture-disabled and RCON-disabled tests verified the actual
configured behavior. The final MCP game test was repeated after this correction.

### UserMsg recursion found and repaired

With SteamScreenshots enabled, loading a map previously caused alternating
`SteamScreenshots::__MsgFunc_ServerName` and HalflifeCLI dispatcher calls until
stack overflow. Periodic reinstallation replaced the monitor's saved original
with a plugin which already forwarded to that same monitor, forming a cycle.

The monitor now preserves later plugin wrappers and only repairs an established
entry when the original callback or a client-module callback replaces it.
Schema reload preserves hook state even for display-only messages. A C++ chain
test failed with the old policy and passed after the change. Real map loading,
schema reload and a second map load passed with both plugins enabled.

### Independent restart teardown failure

`_restart` reloads the engine and publishes another ready endpoint, but immediate
quit afterwards can still fail with `0xC0000409`. Equal-wait controls reproduced
this with RCON both disabled and enabled. An earlier apparent difference used
unequal readiness waits and does not establish causation. Ordinary debugger
attachment masked it. Clearing the debugged flag while retaining exception
capture exposed `FAST_FAIL_FATAL_APP_EXIT` (subcode 7) in Steam `crashhandler.dll`.
The preceding assertion stack follows the launcher's normal `ExitProcess(0)`
through `ntdll!LdrShutdownProcess`, Steam client DLL detach, and
`tier0_s!CThread::~CThread`, reporting illegal termination of `SocketThread`.
This is distinct from the repaired UserMsg stack overflow.

Local diagnostic evidence: `build/control-crash.log` and
`build/restart-native-crash.dmp` and `build/socket-assert.log` (ignored,
machine-local). Temporarily enabling ThreadGuard did not resolve the failure.
With HalflifeCLI and Renderer both removed from the plugin list, native engine
RCON still reproduced `osprey -> _restart -> quit -> 0xC0000409`. The original
plugin list was restored after each control. This establishes independence from
HalflifeCLI and its hooks, but does not identify which earlier Steam/engine
lifecycle action leaves the thread alive. The control retained MetaHook,
VGUI2Extension, SteamScreenshots and SCModelDownloader; a completely stock
launcher was not tested. Full restart lifecycle acceptance remains blocked.

Non-Sven TCP has
automated regression coverage, but no real non-Sven game was launched in this
verification. UDP quiet-window completion remains inherently unable to prove
complete, ordered or duplicate-free delivery.

## Engine family extension (2026-10-06)

Issue [#5](https://github.com/MetaHookSv/halflife-cli/issues/5). The manifest
declares all 11 engine versions of the published index (schema 4,
`lastPublishTime 2026-10-06T02:42:37Z`): `cof-5936`, `hl-3248`, `hl-3266`,
`hl-3329`, `hl-3647`, `hl-4554`, `hl-6153`, `hl-8684`, `hl-10210`,
`svencoop-8948`, `svencoop-10257`. cstrike/czero/czeror snapshots have no
engine records; those mods use the Half-Life `hw.dll` of the same build.
Every version publishes all 34 manifest symbols except `hl-10210`, which lacks
`SV_HandleRconPacket`, `SV_CheckRconFailure`, `SV_BeginRedirect` and
`SV_EndRedirect` (inline-only on Windows per GoldSrc_VibeSignatures #326 Part
B) and exempts them. Synchronization and validation passed for the 11 pruned
snapshots (10.8-12.6 KB each).

### ABI evidence

- Helper ABI and layouts come from the upstream #326 survey of each exact
  input: x86 cdecl helpers, 20-byte `netadr_t` on HL/CoF (36-byte on Sven),
  `NS_SERVER` 1, `sv.active` +0, `sizebuf_t` data +8 / cursize +16, 1400-byte
  `outputbuf`. The runtime still checks the three scalars and the main-frame
  callsite target.
- HL 10210 IDA spot checks: `SV_Rcon` inlines the failure check and redirect
  begin/end; `SV_CheckForRcon` inlines the packet parser; the active-server
  connectionless handler calls `SV_Rcon` and `SVC_ServiceChallenge` directly,
  so the hooks cover it; `Cmd_ExecuteString` tokenizes and calls client
  commands with the native argument state; `SV_AddFailedRcon` keeps a 32-entry
  table of 116-byte records (20 failure times), mirrored by
  `RconPolicy::FailureTracker`.
- Cry of Fear `build_number` is 5936 (compile date `Jan 24 2013`).
- FocusLock on all 11 builds: IGame slot 10 is a getter of the byte
  `CGame_AppActivate` stores (`this+4` on Sven and HL 6153-10210, `this+0x24`
  on HL 3248-4554 and CoF, directly or through a setter); `AppActivate` and
  `SleepUntilInput` are thiscall with one stack argument (`retn 4`).

### Commands and results

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
uv run --script mcp/tests/test_halflife_mcp.py
python mcp/tests/test_native_rcon.py            # with HALFLIFE_RCON_APPID/MOD/DIR per target
$env:HALFLIFE_MOUSE_APPID = '70'; python mcp/tests/test_native_mouse.py
```

Discovering every `mcp/tests/test_*.py` also starts real games
(`test_native_mouse.py`, `test_native_rcon.py`); run the offline suites by name.

Release build and CTest 3/3 passed (`RconPolicy` adds the 20-byte layout,
packet-line/verb/dispatch rules and the failure tracker). Offline Python:
`test_halflife_mcp` 53 (1 opt-in skip), `test_udp_rcon` 14, `test_endpoint` 5,
`test_target` 14, `test_udp_manager` 3, all OK.

MetaHookSv `v20261006a` was installed into the non-Sven games first.
`test_native_rcon.py` covers, in one session: banner and endpoint protocol,
wrong/correct password at the menu, a multi-datagram `cvarlist`, the inert
`cli._rconpacket` console invocation, two failures from a fixed `127.0.0.2`
peer marking it, the next request banned through `addip` and listed by
`listip`, `removeip` + `resetrcon` restoring access, an empty password
accepted from `127.0.0.1` only, `map`, active-server echo and peer RCON,
`disconnect` back to the menu, RCON quit with **exit code 0** and stopped
endpoint metadata.

| Target | Engine | Path | Result |
| --- | --- | --- | --- |
| Sven Co-op | svencoop-10257 | native helpers | passed (`osprey`) |
| Half-Life (valve) | hl-10210 | four plugin fallbacks | passed (`crossfire`) |
| Counter-Strike (Steam) | hl-10210 | four plugin fallbacks | passed (`de_dust2`) |
| CS 1.6 3266 (BLOB) | hl-3266 | native helpers | passed with the previous launcher, see below |
| Cry of Fear | cof-5936 | native helpers | passed (`c_apartment1`) |

`test_native_mouse.py` on Half-Life 10210 passed 4/4, including all 24 window
mode / block / input lock / focus lock combinations and `_restart`.

With the `halflifecli` catalog moved out of the Half-Life gamedata directory,
the plugin printed `Native UDP unavailable (no Native UDP gamedata for this
hw.dll (symbol was not found in the matched gamedata snapshot)); using Source
TCP`, listened on `source-tcp` and quit with exit code 0.

hl-4554, hl-6153, hl-8684, hl-3248, hl-3329 and hl-3647 have catalog and ABI
coverage only; no installation of those builds was run.

### CS 1.6 3266 and MetaHookSv v20261006a

The D:\CS3266 install (with server-side metamod and AMX Mod X 1.8.1) does not
survive a map load under `MetaHook_blob.exe` from `v20261006a`
(SHA-256 prefix `58f68b2f5658d6cb`): the process exits with `0xE06D7363`
(unhandled C++ exception) right after AMXX registers its client menus, also
with an **empty** `plugins.lst` and without piped stdio. Stock `hl.exe` and the
previously installed `MetaHook_blob.exe` (`7f65af0e726d3834`) keep running.
Separately, `Renderer.dll` crashes that install at startup (`0xC0000005`)
under the new launcher, with or without HalflifeCLI. Neither involves this
plugin; the 3266 row above therefore used the previous launcher (temporarily
swapped back) and a plugin list without Renderer. The new launcher was restored
afterwards.

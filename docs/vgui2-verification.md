# VGUI2 port verification — 2026-10-08

Port source: HLND2T-DiligentGraphics `7b2818ee99b83dd5e29881741d199979d0068f29`
and `98f8635a361a02250fb40448ccd27d7cf0ae46cb`. The plugin exposes the same five
MCP tools; RequestInfo-based semantics and set_text remain unsupported.

## Automated checks

- Win32 Release build with explicit local MetaHook/VGUI2Extension sources: passed.
- Clean default FetchContent configure/build in `build/vgui2-pinned`: passed.
  MetaHook fetched only its pinned RapidJSON submodule; no SDK version changes.
- CTest: 5/5 passed in both builds (VGUI2, RconPolicy, UserMsgHook, InputState,
  EarlyConsole).
- `uv run --with 'mcp>=2.2,<3' --with 'pillow>=10' --with 'pywin32>=311'
  python -m unittest discover -s mcp/tests`: 102 tests, 101 passed, 1 skipped
  (the separately opt-in full-cycle test). Existing fake-server tests printed
  resource warnings and a background socket timeout; unittest exited zero.
- Recompiled `tests/engine_input_test.cpp` and ran with Sven's SDL2.dll:
  `engine_input: 0 failure(s)` (includes the recording legacy trampoline).
- `git diff --check`: passed.

The VGUI2 tests execute the actual engine-independent state machine, including
TCP/UDP page budgets, reference invalidation, unavailable roots, hidden and
disabled controls, modal/occlusion checks, focus, single/double click stages,
destruction during a click, release failure/retry, timeout, geometry scaling,
invalid UTF-8 replacement and set_text rejection. MCP tests exercise the five
registered tools and structured results, not source-text matching.

The final review changed snapshot storage to compact serialized nodes rather
than one RapidJSON allocator arena per node. Paging tests also verify that
later changes to live controls do not change an existing snapshot.
After this storage change, the pinned build and all five CTests passed again;
Sven's 577-node/24-page menu flow and clean quit also passed again.

## Game verification

Final smoke runs used the DLL built against pinned dependencies, 800x600
windowed/off-screen, native GoldSrc UDP. Each row passed opening Options by
reference, focusing Cancel, closing by reference, rejection of the destroyed
Cancel reference, and RCON quit with exit code zero.

| Target | Build | Hidden-inclusive tree | Pages in final run |
| --- | --- | --- | --- |
| Sven Co-op | 10257 | 577 nodes | 24 |
| Half-Life / HL25 | 10210 | 654 nodes | 27 |
| Cry of Fear / legacy | 5936 | 616 nodes | 25 |

Page counts depend on serialized reference lengths and installed UI plugins.
Cry of Fear automatically loads `c_game_menu1`; send an engine-level Escape
press/release first to expose its VGUI2 menu.

During the earlier local-SDK build run, screenshots visually confirmed Options
opened on all three engines. Sven additionally passed NameEntry focus, a
double click on NameEntry, `map osprey`, and a subsequent tree query. A double
click on the Options menu entry opened the dialog on the first click and
correctly rejected the second click as obstructed. The set_text begin request
returned unsupported without a text upload.

## Limits and observations

- Source TCP framing/page limits were tested automatically, not against a
  live TCP-selected engine. Other catalog engine builds and fullscreen
  coordinate behavior are not claimed as runtime-verified.
- The first Sven launch of the initial local-SDK build exited with
  `0xC0000374` (heap corruption) before its RCON banner. No matching new dump
  was available. It did not reproduce under x86 CDB, on the subsequent normal
  local-SDK launch, or in the final pinned-SDK run. Its cause remains unknown;
  the passing later runs do not establish a fix for that one-off exit.
- On Cry of Fear, the existing MCP snapshot finder timed out because the
  engine wrote `HalfLife44.tga` in the game root. Reading that file directly
  confirmed the Options dialog; the converted evidence was kept locally at
  `build/vgui2-cof-options.png`. The screenshot finder was not changed by this
  port.
- All task-launched games and debugger sessions were stopped. Test DLLs/PDBs
  were deployed to the three existing game installations; source repositories
  for the engine, MetaHook and VGUI2Extension were not modified.

# Native input regression test

CMake builds the harness as the `EngineInputTest` target whenever tests are
enabled (`cmake --build build --config Release --target EngineInputTest`);
CI compiles it on every run. The manual recipe below is the equivalent
without CMake.

VGUI2 regression: build `VGUI2Test`, then run `ctest --test-dir build -C Release
--output-on-failure`. Its fake backend exercises the real automation state
machine: paging budgets, stale references, modal/visibility restrictions,
focus, destruction during a click, release failure and timeout. Python wire
tests run with `python -m unittest discover -s mcp/tests -p test_vgui2.py`;
`uv run --script mcp/tests/test_halflife_mcp.py` also checks all five MCP tools.
These tests do not replace game-level interface ABI and screenshot checks.

From an **x86 Native Tools Command Prompt for Visual Studio**, at the repo
root (with `build/` already configured). Set `METAHOOK_SOURCE_PATH` to the
resolved path printed by CMake; for the default FetchContent build:

```bat
set "METAHOOK_SOURCE_PATH=%CD%\build\_deps\halflife_cli_metahook-src"
cl /nologo /std:c++20 /EHsc /MT /DNOMINMAX /wd4819 /Isrc ^
  /I"%METAHOOK_SOURCE_PATH%/include" /I"%METAHOOK_SOURCE_PATH%/include/Interface" ^
  /I"%METAHOOK_SOURCE_PATH%/include/HLSDK/common" ^
  /I"%METAHOOK_SOURCE_PATH%/include/HLSDK/engine" ^
  /I"%METAHOOK_SOURCE_PATH%/include/HLSDK/cl_dll" ^
  /I"%METAHOOK_SOURCE_PATH%/include/HLSDK/pm_shared" ^
  /I"%METAHOOK_SOURCE_PATH%/include/HLSDK/public" ^
  /I"%METAHOOK_SOURCE_PATH%/include/SourceSDK" ^
  tests/engine_input_test.cpp /Fobuild/engine_input_test.obj ^
  /Febuild/engine_input_test.exe /link user32.lib
build\engine_input_test.exe "C:\absolute\path\to\SDL2.dll"
```

Use a **32-bit** SDL2 DLL. Dependencies such as SDL3.dll must be alongside it.
The harness loads the actual runtime, tests event filtering and injection,
filter chaining/restoration, button transitions and error paths. A recording
trampoline checks legacy WindowProc messages and scan-code flags, scaled motion,
button coordinates, synchronous cursor reads, later-frame recentres, resize,
failed dispatch and physical takeover. Mock user32 functions use a negative
desktop origin to catch client/screen coordinate confusion. It does not
launch a game or synthesize OS input; legacy gamedata resolution and actual
legacy game dispatch still need an integration test on that engine.

Legacy acceptance (CoF 5936, 800x600, off-screen window): move to `(400, 503)`
and click to open Options, then move to `(245, 142)` and click to select Mouse.
Check the resulting screenshot, not just the command's position reply. VGUI
loads after `LoadClient` on this engine; without a DLL-load notification
installing its cursor import hooks, `InternalCursorMoved` receives the target
but the next `CVGui::RunFrame`
reads the desktop cursor and clears mouse focus. Keep both moves and clicks
on separate command/frame boundaries to expose this regression. Reference:
`vgui2/src/vgui.cpp` (`GetCursorPos` then `UpdateMouseFocus`) and
`vgui2/src/InputWin32.cpp` in the engine reference source.

Native SDL2 2.32.10 and [sdl2-compat-fork](https://github.com/hzqst/sdl2-compat-fork)
with fix [`c24acad`](https://github.com/hzqst/sdl2-compat-fork/commit/c24acad)
pass. The wheel
checks cover up/down pulses, release without duplication, injection bypass,
and the prior filter receiving the correct integer delta and rejecting input.
Unpatched sdl2-compat 2.32.57 fails: integer `y` is lost while `preciseY`
survives. The GoldSrc handler reads integer `y`; use a runtime containing the
fork's `Event2to3` integer wheel fix. No SDL3 injection workaround is needed.

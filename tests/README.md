# Native input regression test

From an **x86 Native Tools Command Prompt for Visual Studio**, at the repo
root (with `build/` already present):

```bat
cl /nologo /std:c++20 /EHsc /MT /DNOMINMAX /wd4819 /Isrc ^
  /Ithirdparty/MetaHookSv/include /Ithirdparty/MetaHookSv/include/Interface ^
  /Ithirdparty/MetaHookSv/include/HLSDK/common ^
  /Ithirdparty/MetaHookSv/include/HLSDK/engine ^
  /Ithirdparty/MetaHookSv/include/HLSDK/cl_dll ^
  /Ithirdparty/MetaHookSv/include/HLSDK/pm_shared ^
  /Ithirdparty/MetaHookSv/include/HLSDK/public ^
  /Ithirdparty/MetaHookSv/include/SourceSDK ^
  tests/engine_input_test.cpp /Fobuild/engine_input_test.obj ^
  /Febuild/engine_input_test.exe /link user32.lib
build\engine_input_test.exe "C:\absolute\path\to\SDL2.dll"
```

Use a **32-bit** SDL2 DLL. Dependencies such as SDL3.dll must be alongside it.
The harness loads the actual runtime, tests event filtering and injection,
filter chaining/restoration, button transitions and error paths. A recording
trampoline checks legacy WindowProc messages and scan-code flags. It does not
launch a game or synthesize OS input; legacy gamedata resolution and actual
legacy game dispatch still need an integration test on that engine.

Native SDL2 2.32.10 and sdl2-compat-fork with fix `c24acad` pass. The wheel
checks cover up/down pulses, release without duplication, injection bypass,
and the prior filter receiving the correct integer delta and rejecting input.
Unpatched sdl2-compat 2.32.57 fails: integer `y` is lost while `preciseY`
survives. The GoldSrc handler reads integer `y`; use a runtime containing the
fork's `Event2to3` integer wheel fix. No SDL3 injection workaround is needed.

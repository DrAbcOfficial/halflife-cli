cmake_minimum_required(VERSION 3.21)

project(halflife-cli LANGUAGES C CXX ASM_MASM)

if(NOT WIN32)
	message(FATAL_ERROR "halflife-cli only supports Windows.")
endif()

if(NOT CMAKE_GENERATOR MATCHES "Visual Studio" AND NOT CMAKE_VS_PLATFORM_NAME)
	message(WARNING "halflife-cli is normally built with a Visual Studio generator (-A Win32).")
endif()

if(CMAKE_VS_PLATFORM_NAME AND NOT CMAKE_VS_PLATFORM_NAME MATCHES "Win32|x86")
	message(FATAL_ERROR "halflife-cli must target Win32/x86 (the game process is 32-bit). Configure with: cmake -A Win32")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "halflife-cli must target Win32/x86 (the game process is 32-bit). Configure with: cmake -A Win32")
endif()

include(cmake/Dependencies.cmake)
halflife_cli_prepare_dependencies()

set(TOMLPLUSPLUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tomlplusplus")
if(NOT EXISTS "${TOMLPLUSPLUS_DIR}/include/toml++/toml.hpp")
	message(FATAL_ERROR "tomlplusplus submodule is missing. Run: git submodule update --init --depth 1")
endif()

# One folder per responsibility: core (lifecycle + cli.* commands), config,
# console I/O, input blocking/locking, rcon server, window management, usermsg
# monitoring, and util (helpers shared across modules).
add_library(HalflifeCLI SHARED
	src/core/plugins.cpp
	src/core/exportfuncs.cpp
	src/core/cli_commands.cpp
	src/config/config.cpp
	src/console/console_bridge.cpp
	src/console/output_capture.cpp
	src/console/sys_error.cpp
	src/input/engine_input.cpp
	src/input/focus_lock.cpp
	src/input/input_lock.cpp
	src/rcon/rcon_server.cpp
	src/rcon/tcp_server.cpp
	src/rcon/native_udp.cpp
	src/rcon/shutdown_thunks.asm
	src/window/window_manager.cpp
	src/usermsg/usermsg_schema.cpp
	src/usermsg/usermsg_decoder.cpp
	src/usermsg/usermsg_monitor.cpp
	src/util/import_hook.cpp
	src/util/toml_file.cpp
	src/vgui2/automation.cpp
	src/vgui2/vgui2.cpp
	"${METAHOOK_SOURCE_PATH}/include/HLSDK/common/interface.cpp"
)

target_compile_features(HalflifeCLI PRIVATE cxx_std_20)
# x86 links with /SAFESEH; the MASM thunks register no exception handlers.
target_compile_options(HalflifeCLI PRIVATE $<$<COMPILE_LANGUAGE:ASM_MASM>:/safeseh>)

target_include_directories(HalflifeCLI PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${TOMLPLUSPLUS_DIR}/include
	"${METAHOOK_SOURCE_PATH}/thirdparty/rapidjson/include"
	"${VGUI2EXTENSION_SOURCE_PATH}/include/Interface"
	"${VGUI2EXTENSION_SOURCE_PATH}/include/Interface/VGUI"
	"${METAHOOK_SOURCE_PATH}/include"
	"${METAHOOK_SOURCE_PATH}/include/Interface"
	"${METAHOOK_SOURCE_PATH}/include/Interface/VGUI"
	"${METAHOOK_SOURCE_PATH}/include/HLSDK/common"
	"${METAHOOK_SOURCE_PATH}/include/HLSDK/engine"
	"${METAHOOK_SOURCE_PATH}/include/HLSDK/cl_dll"
	"${METAHOOK_SOURCE_PATH}/include/HLSDK/pm_shared"
	"${METAHOOK_SOURCE_PATH}/include/HLSDK/public"
	"${METAHOOK_SOURCE_PATH}/include/VGUI"
	"${METAHOOK_SOURCE_PATH}/include/SourceSDK"
	"${METAHOOK_SOURCE_PATH}/include/SourceSDK/tier0"
	"${METAHOOK_SOURCE_PATH}/include/SourceSDK/tier1"
)

if(MSVC)
	# CRT and warning switches are compiler-only; ml.exe rejects them.
	target_compile_options(HalflifeCLI PRIVATE
		$<$<COMPILE_LANGUAGE:C,CXX>:/MT$<$<CONFIG:Debug>:d>>	# static CRT, the game must not need our redist
		/W3
		"$<$<COMPILE_LANGUAGE:C,CXX>:/wd4311;/wd4312;/wd4819;/wd4996>"	# pointer truncation / charset noise from MetaHookSv headers
	)
	target_compile_definitions(HalflifeCLI PRIVATE
		NOMINMAX
		_CRT_SECURE_NO_WARNINGS
	)
	# Emit a program database in every configuration so the DeployGame/LaunchGame
	# workflow can resolve symbols in the running game; the PDB ships next to the
	# DLL (see the install rules below).
	target_compile_options(HalflifeCLI PRIVATE /Zi)
	target_link_options(HalflifeCLI PRIVATE /DEBUG)
endif()

target_link_libraries(HalflifeCLI PRIVATE ws2_32 user32)

set_target_properties(HalflifeCLI PROPERTIES
	OUTPUT_NAME "HalflifeCLI"
	PREFIX ""
)

# Install layout consumed by MetahookInstallerCLI's payload convention: paths are
# relative to the game root so the DeployGame staging prefix maps 1:1 onto it.
# The PDB is what makes the LaunchGame debugger usable, so it ships with the DLL.
install(TARGETS HalflifeCLI RUNTIME DESTINATION svencoop/metahook/plugins)
install(FILES "$<TARGET_PDB_FILE:HalflifeCLI>" DESTINATION svencoop/metahook/plugins)
# Configuration template plus the usermsg schemas, both under the plugin's own
# config folder.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/configs/halflifecli/halflifecli.toml"
	DESTINATION svencoop/metahook/configs/halflifecli)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/configs/usermsgs/"
	DESTINATION svencoop/metahook/configs/halflifecli/usermsgs)

# The same direct consumer workflow used by MetaHookSv plugins: synchronize
# upstream snapshots, then enforce this plugin's required-symbol manifest.
# scripts/sync-gamedata.py requires Python 3.8 (checked again at runtime);
# enforce it at configure time so the failure is a clear message, not a script
# error mid-build.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(HALFLIFECLI_GAMEDATA_DIR "${PROJECT_BINARY_DIR}/metahook/gamedata/halflifecli")
add_custom_target(HalflifeCLIGameData
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/sync-gamedata.py"
        --manifest "${PROJECT_SOURCE_DIR}/scripts/manifests/halflifecli.json"
        --target-dir "${HALFLIFECLI_GAMEDATA_DIR}" --temp-root "${PROJECT_BINARY_DIR}/gamedata-sync"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/validate-gamedata.py"
        "${HALFLIFECLI_GAMEDATA_DIR}" --manifest "${PROJECT_SOURCE_DIR}/scripts/manifests/halflifecli.json"
    COMMENT "Synchronizing and validating HalflifeCLI gamedata" VERBATIM)
add_dependencies(HalflifeCLI HalflifeCLIGameData)

# Ship the synchronized gamedata catalog; the target above produces it before
# HalflifeCLI builds, so it exists by the time the install rule runs.
install(DIRECTORY "${HALFLIFECLI_GAMEDATA_DIR}/"
	DESTINATION svencoop/metahook/gamedata/halflifecli OPTIONAL)

include(CTest)
if(BUILD_TESTING)
    add_executable(VGUI2Test tests/vgui2_test.cpp src/vgui2/automation.cpp)
    target_compile_features(VGUI2Test PRIVATE cxx_std_20)
    target_include_directories(VGUI2Test PRIVATE src "${METAHOOK_SOURCE_PATH}/thirdparty/rapidjson/include")
    add_test(NAME VGUI2 COMMAND VGUI2Test)
    add_executable(RconPolicyTest tests/rcon_policy_test.cpp)
    target_compile_features(RconPolicyTest PRIVATE cxx_std_20)
    target_include_directories(RconPolicyTest PRIVATE src "${METAHOOK_SOURCE_PATH}/include/HLSDK/common")
    add_test(NAME RconPolicy COMMAND RconPolicyTest)
    add_executable(UserMsgHookTest tests/usermsg_hook_test.cpp)
    target_include_directories(UserMsgHookTest PRIVATE src)
    add_test(NAME UserMsgHook COMMAND UserMsgHookTest)
    add_executable(InputStateTest tests/input_state_test.cpp)
    target_compile_features(InputStateTest PRIVATE cxx_std_20)
    target_include_directories(InputStateTest PRIVATE src)
    add_test(NAME InputState COMMAND InputStateTest)
    add_executable(EarlyConsoleTest
        tests/early_console_test.cpp
        src/core/plugins.cpp
        src/core/exportfuncs.cpp
        src/console/console_bridge.cpp
        src/console/sys_error.cpp
        "${METAHOOK_SOURCE_PATH}/include/HLSDK/common/interface.cpp")
    target_compile_features(EarlyConsoleTest PRIVATE cxx_std_20)
    target_include_directories(EarlyConsoleTest PRIVATE $<TARGET_PROPERTY:HalflifeCLI,INCLUDE_DIRECTORIES>)
    target_compile_definitions(EarlyConsoleTest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(EarlyConsoleTest PRIVATE user32)
    add_test(NAME EarlyConsole COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/early_console_test.py" $<TARGET_FILE:EarlyConsoleTest>)
	set_tests_properties(EarlyConsole PROPERTIES TIMEOUT 45)

	# Compile-only regression harness (see tests/README.md): at runtime it loads
	# a real 32-bit SDL2.dll, so it has no add_test here; building it in the
	# default target still keeps CI from shipping compile breaks.
	add_executable(EngineInputTest tests/engine_input_test.cpp)
	target_compile_features(EngineInputTest PRIVATE cxx_std_20)
	target_include_directories(EngineInputTest PRIVATE $<TARGET_PROPERTY:HalflifeCLI,INCLUDE_DIRECTORIES>)
	target_compile_definitions(EngineInputTest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
	target_link_libraries(EngineInputTest PRIVATE user32)
endif()

# Optional Visual Studio F5 workflow: build HalflifeCLI, deploy it into the game
# and launch svencoop.exe under the debugger. Off by default so CI and ordinary
# builds never require a game install. The switch lives here (before the module
# include) so it is visible in cmake-gui even when the workflow is disabled.
option(HALFLIFECLI_ENABLE_LAUNCH_GAME
    "Add the Visual Studio build/deploy/debug targets (requires a game install)" OFF)
if(HALFLIFECLI_ENABLE_LAUNCH_GAME)
    include(cmake/LaunchGame.cmake)
    halflifecli_add_launch_game()
endif()

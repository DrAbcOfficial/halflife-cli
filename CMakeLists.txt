cmake_minimum_required(VERSION 3.21)

project(halflife-cli LANGUAGES C CXX)

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
	src/input/engine_input.cpp
	src/input/input_lock.cpp
	src/rcon/rcon_server.cpp
	src/rcon/tcp_server.cpp
	src/rcon/sven_udp.cpp
	src/window/window_manager.cpp
	src/usermsg/usermsg_schema.cpp
	src/usermsg/usermsg_decoder.cpp
	src/usermsg/usermsg_monitor.cpp
	src/util/toml_file.cpp
	"${METAHOOK_SOURCE_PATH}/include/HLSDK/common/interface.cpp"
)

target_compile_features(HalflifeCLI PRIVATE cxx_std_20)

target_include_directories(HalflifeCLI PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${TOMLPLUSPLUS_DIR}/include
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
	target_compile_options(HalflifeCLI PRIVATE
		/MT$<$<CONFIG:Debug>:d>		# static CRT, the game must not need our redist
		/W3
		/wd4311 /wd4312 /wd4819 /wd4996	# pointer truncation / charset noise from MetaHookSv headers
	)
	target_compile_definitions(HalflifeCLI PRIVATE
		NOMINMAX
		_CRT_SECURE_NO_WARNINGS
	)
endif()

target_link_libraries(HalflifeCLI PRIVATE ws2_32 user32)

set_target_properties(HalflifeCLI PROPERTIES
	OUTPUT_NAME "HalflifeCLI"
	PREFIX ""
)

# The same direct consumer workflow used by MetaHookSv plugins: synchronize
# upstream snapshots, then enforce this plugin's required-symbol manifest.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(HALFLIFECLI_GAMEDATA_DIR "${PROJECT_BINARY_DIR}/metahook/gamedata/halflifecli")
add_custom_target(HalflifeCLIGameData
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/sync-gamedata.py"
        --manifest "${PROJECT_SOURCE_DIR}/scripts/manifests/halflifecli.json"
        --target-dir "${HALFLIFECLI_GAMEDATA_DIR}" --temp-root "${PROJECT_BINARY_DIR}/gamedata-sync"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/validate-gamedata.py"
        "${HALFLIFECLI_GAMEDATA_DIR}" --manifest "${PROJECT_SOURCE_DIR}/scripts/manifests/halflifecli.json"
    COMMENT "Synchronizing and validating HalflifeCLI gamedata" VERBATIM)
add_dependencies(HalflifeCLI HalflifeCLIGameData)

include(CTest)
if(BUILD_TESTING)
    add_executable(SvenRconPolicyTest tests/sven_policy_test.cpp)
    target_compile_features(SvenRconPolicyTest PRIVATE cxx_std_20)
    target_include_directories(SvenRconPolicyTest PRIVATE src "${METAHOOK_SOURCE_PATH}/include/HLSDK/common")
    add_test(NAME SvenRconPolicy COMMAND SvenRconPolicyTest)
    add_executable(UserMsgHookTest tests/usermsg_hook_test.cpp)
    target_include_directories(UserMsgHookTest PRIVATE src)
    add_test(NAME UserMsgHook COMMAND UserMsgHookTest)
endif()

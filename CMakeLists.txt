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

set(METAHOOKSV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/MetaHookSv")
if(NOT EXISTS "${METAHOOKSV_DIR}/include/metahook.h")
	message(FATAL_ERROR "MetaHookSv submodule is missing. Run: git submodule update --init --depth 1")
endif()

set(TOMLPLUSPLUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tomlplusplus")
if(NOT EXISTS "${TOMLPLUSPLUS_DIR}/include/toml++/toml.hpp")
	message(FATAL_ERROR "tomlplusplus submodule is missing. Run: git submodule update --init --depth 1")
endif()

add_library(HalflifeCLI SHARED
	src/plugins.cpp
	src/exportfuncs.cpp
	src/config.cpp
	src/output_capture.cpp
	src/console_bridge.cpp
	src/rcon_server.cpp
	src/window_manager.cpp
	src/usermsg_schema.cpp
	src/usermsg_monitor.cpp
	"${METAHOOKSV_DIR}/include/HLSDK/common/interface.cpp"
)

target_compile_features(HalflifeCLI PRIVATE cxx_std_20)

target_include_directories(HalflifeCLI PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${TOMLPLUSPLUS_DIR}/include
	${METAHOOKSV_DIR}/include
	${METAHOOKSV_DIR}/include/Interface
	${METAHOOKSV_DIR}/include/Interface/VGUI
	${METAHOOKSV_DIR}/include/HLSDK/common
	${METAHOOKSV_DIR}/include/HLSDK/engine
	${METAHOOKSV_DIR}/include/HLSDK/cl_dll
	${METAHOOKSV_DIR}/include/HLSDK/pm_shared
	${METAHOOKSV_DIR}/include/HLSDK/public
	${METAHOOKSV_DIR}/include/VGUI
	${METAHOOKSV_DIR}/include/SourceSDK
	${METAHOOKSV_DIR}/include/SourceSDK/tier0
	${METAHOOKSV_DIR}/include/SourceSDK/tier1
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

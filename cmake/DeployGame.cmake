# Runs as a script (`cmake -P`) from the DeployGame custom target.
#
# 1. Stage this build tree's install rules into a private payload directory
#    (never the game's own files).
# 2. Deploy the payload with MetahookInstallerCLI in -plugins-only mode, which
#    keeps the game's plugins.lst untouched (it is user-owned) and never
#    replaces the MetaHook launcher.
# 3. Register HalflifeCLI.dll in plugins.lst, which MetaHook only reads from
#    there (MetaHook's MH_LoadPlugins returns early without it).
cmake_minimum_required(VERSION 3.21)
include("${SETTINGS}")
if(NOT CONFIG MATCHES "^(Debug|Release)$")
    message(FATAL_ERROR "LaunchGame requires Debug or Release.")
endif()

function(run_checked)
    execute_process(COMMAND ${ARGV} WORKING_DIRECTORY "${stage}" RESULT_VARIABLE result)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "DeployGame command failed (${result}): ${ARGV0}")
    endif()
endfunction()

set(stage "${launch_root}/${CONFIG}")
file(MAKE_DIRECTORY "${stage}")
# Serialize deployments from this build tree, including Debug/Release switches.
file(LOCK "${launch_root}/deploy.lock" GUARD PROCESS TIMEOUT 120)

# InstallerCLI builds only search for their payload next to their own
# executable, so place the CLI and the payload side by side in the stage dir.
# A single-file release exe is self-contained (copy just it); a framework-
# dependent override has a sibling MetahookInstallerCLI.dll and needs the whole
# directory, whose assembly set sits next to it.
get_filename_component(_cli_dir "${cli_executable}" DIRECTORY)
if(EXISTS "${_cli_dir}/MetahookInstallerCLI.dll")
    run_checked("${CMAKE_COMMAND}" -E copy_directory "${_cli_dir}" "${stage}")
else()
    run_checked("${CMAKE_COMMAND}" -E copy_if_different "${cli_executable}" "${stage}/MetahookInstallerCLI.exe")
endif()
set(cli_command "${stage}/MetahookInstallerCLI.exe")

# Only remove the private staging payload; never clean the game's directory.
file(MAKE_DIRECTORY "${stage}/install/output")
file(REAL_PATH "${binary_dir}" binary_real)
file(REAL_PATH "${stage}/install/output" payload_real)
set(expected_payload "${binary_real}/launch-game/${CONFIG}/install/output")
if(NOT payload_real STREQUAL expected_payload)
    message(FATAL_ERROR "Refusing to clean an unexpected DeployGame payload: ${payload_real}")
endif()
file(REMOVE_RECURSE "${payload_real}")
if(EXISTS "${payload_real}")
    message(FATAL_ERROR "Cannot clear the private DeployGame payload (a file may be locked): ${payload_real}")
endif()
run_checked("${CMAKE_COMMAND}" --install "${binary_dir}" --config "${CONFIG}" --prefix "${payload_real}")

# Verify the CLI still resolves to the launcher we configured against.
set(target_args -appid "${game_appid}" -gamedir "${game_directory}" -moddir "${game_mod}")
if(plugins_only)
    list(APPEND target_args -plugins-only)
endif()
execute_process(COMMAND ${cli_command} ${target_args} -describe-target
    WORKING_DIRECTORY "${stage}" RESULT_VARIABLE result OUTPUT_VARIABLE description ERROR_VARIABLE error)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Cannot query the game before deployment: ${error}")
endif()
string(JSON current_launcher GET "${description}" LauncherPath)
if(NOT current_launcher STREQUAL launcher)
    message(FATAL_ERROR "The game's launcher changed. Reconfigure CMake before deploying. Expected '${launcher}', now '${current_launcher}'.")
endif()

run_checked(${cli_command} ${target_args})

set(_mod_dir "${game_directory}/${game_mod}")
set(_configs_dir "${_mod_dir}/metahook/configs")
set(_plugin_data_dir "${_configs_dir}/halflifecli")

# Migrate an install from the pre-halflifecli layout (files at the top level of
# metahook/configs). The TOML holds user settings, so it must not be dropped;
# the port file is merely stale and is rewritten at every plugin start.
file(MAKE_DIRECTORY "${_plugin_data_dir}")
if(EXISTS "${_configs_dir}/halflifecli.toml" AND NOT EXISTS "${_plugin_data_dir}/halflifecli.toml")
    file(RENAME "${_configs_dir}/halflifecli.toml" "${_plugin_data_dir}/halflifecli.toml")
    message(STATUS "Migrated halflifecli.toml into ${_plugin_data_dir}")
endif()
if(EXISTS "${_configs_dir}/halflifecli.port")
    file(RENAME "${_configs_dir}/halflifecli.port" "${_plugin_data_dir}/halflifecli.port")
endif()

# ---------------------------------------------------------------------------
# plugins.lst: -plugins-only preserves it, so register our own entries here.
# ---------------------------------------------------------------------------
set(_plugins_dir "${_mod_dir}/metahook/plugins")
set(_plugins_lst "${_mod_dir}/metahook/configs/plugins.lst")

if(NOT EXISTS "${_plugins_lst}")
    set(_text "")
else()
    file(READ "${_plugins_lst}" _text)
endif()
# Normalize to LF so the appended-entry regexes and newline repair are stable.
string(REPLACE "\r\n" "\n" _text "${_text}")
string(REPLACE "\r" "\n" _text "${_text}")

# VGUI2Extension.dll is a hard dependency (console output is captured through
# its callbacks) and must load first. Only register it when the DLL is present,
# so a partial install cannot make MetaHook fail to start.
string(TOLOWER "${_text}" _text_lower)
if(NOT _text_lower MATCHES "(^|\n)[ \t]*vgui2extension\\.dll[ \t]*(\n|$)")
    if(EXISTS "${_plugins_dir}/VGUI2Extension.dll")
        set(_text "VGUI2Extension.dll\n${_text}")
    else()
        message(WARNING "VGUI2Extension.dll is not installed under ${_plugins_dir}; console output mirroring will be disabled. Install VGUI2Extension, then deploy again.")
    endif()
endif()

# Append the plugin once. A line break is written first because a file without
# a trailing newline would otherwise glue the entry onto the last line (e.g.
# "VGUI2Extension.dllHalflifeCLI.dll"), breaking both entries.
string(TOLOWER "${_text}" _text_lower)
if(NOT _text_lower MATCHES "(^|\n)[ \t]*halflifecli\\.dll[ \t]*(\n|$)")
    if(NOT _text MATCHES "\n$")
        string(APPEND _text "\n")
    endif()
    string(APPEND _text "HalflifeCLI.dll\n")
endif()

file(WRITE "${_plugins_lst}" "${_text}")
message(STATUS "DeployGame ready: ${_plugins_lst}")

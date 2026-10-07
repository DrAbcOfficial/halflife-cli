# Visual Studio F5 workflow: build -> deploy into the game -> launch under the
# debugger. Keeps every machine setting in the cache.
#
# The game directory is never hardcoded. It is resolved at configure time by
# asking MetahookInstallerCLI (-describe-target), which knows Steam's install
# layout, so non-standard installs only need HALFLIFECLI_GAME_DIRECTORY set.
include_guard(GLOBAL)

option(HALFLIFECLI_ENABLE_LAUNCH_GAME
    "Add the Visual Studio build/deploy/debug targets (requires a game install)" OFF)
set(HALFLIFECLI_GAME_DIRECTORY "" CACHE PATH
    "Game root (empty: discover the Steam app through MetahookInstallerCLI)")
set(HALFLIFECLI_GAME_APPID "225840" CACHE STRING "Steam app ID (225840 = Sven Co-op)")
set(HALFLIFECLI_GAME_MOD "" CACHE STRING
    "Game mod directory (empty: MetahookInstallerCLI's app default)")
set(HALFLIFECLI_GAME_ARGUMENTS "" CACHE STRING
    "Extra game arguments appended after -insecure -game <mod> -windowed -novid")
set(HALFLIFECLI_INSTALLER_CLI_EXECUTABLE "" CACHE FILEPATH
    "Override with a self-contained MetahookInstallerCLI.exe")
set(HALFLIFECLI_INSTALLER_RELEASE "latest" CACHE STRING
    "Installer release tag to cache when no CLI is provided (or latest)")

function(halflifecli_add_launch_game)
    if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio " OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "LaunchGame requires a Visual Studio generator with -A Win32.")
    endif()
    if(TARGET LaunchGame OR TARGET DeployGame)
        message(FATAL_ERROR "LaunchGame/DeployGame already exists; add the workflow once per build tree.")
    endif()
    if(NOT TARGET HalflifeCLI)
        message(FATAL_ERROR "LaunchGame requires the HalflifeCLI target.")
    endif()

    set(_launch_root "${CMAKE_BINARY_DIR}/launch-game")
    file(MAKE_DIRECTORY "${_launch_root}/configure")
    set(_cli_executable "")
    if(HALFLIFECLI_INSTALLER_CLI_EXECUTABLE)
        get_filename_component(_cli_executable "${HALFLIFECLI_INSTALLER_CLI_EXECUTABLE}" ABSOLUTE)
        if(NOT EXISTS "${_cli_executable}" OR IS_DIRECTORY "${_cli_executable}")
            message(FATAL_ERROR "InstallerCLI override does not exist: ${_cli_executable}")
        endif()
    else()
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/InstallerCLI.cmake")
        halflifecli_download_installer("${_launch_root}" "${HALFLIFECLI_INSTALLER_RELEASE}" _cli_executable)
    endif()

    # Query the target without -plugins-only so configuring works even before
    # MetaHook itself has been installed; this only locates the game/launcher.
    set(_query_args -appid "${HALFLIFECLI_GAME_APPID}")
    if(NOT HALFLIFECLI_GAME_DIRECTORY STREQUAL "")
        list(APPEND _query_args -gamedir "${HALFLIFECLI_GAME_DIRECTORY}")
    endif()
    if(NOT HALFLIFECLI_GAME_MOD STREQUAL "")
        list(APPEND _query_args -moddir "${HALFLIFECLI_GAME_MOD}")
    endif()
    execute_process(
        COMMAND "${_cli_executable}" ${_query_args} -describe-target
        WORKING_DIRECTORY "${_launch_root}/configure"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _description ERROR_VARIABLE _error)
    if(NOT _result STREQUAL "0")
        message(FATAL_ERROR "Cannot resolve the LaunchGame target. Check HALFLIFECLI_GAME_DIRECTORY/APPID/MOD.\n${_error}")
    endif()
    string(JSON _game_directory GET "${_description}" GameDirectory)
    string(JSON _game_mod GET "${_description}" ModDirectory)
    string(JSON _launcher GET "${_description}" LauncherPath)

    # HalflifeCLI is always deployed as a plugin into an existing MetaHook
    # install, so DeployGame uses MetahookInstallerCLI's -plugins-only mode
    # (keeps plugins.lst and the launcher, replaces our plugin files).
    set(_plugins_only TRUE)

    # Bracket-quoted settings keep Windows paths and argument quoting intact.
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/LaunchGameSettings.cmake.in"
        "${_launch_root}/settings.cmake" @ONLY)
    file(CONFIGURE OUTPUT "${_launch_root}/dummy_launcher.cpp"
        CONTENT "int main() { return 0; }\n" @ONLY)
    add_executable(LaunchGame EXCLUDE_FROM_ALL "${_launch_root}/dummy_launcher.cpp")
    # MetaHookSv's launcher arguments (-insecure -game <mod>) plus this plugin's
    # requirements: -windowed lets the plugin hide the render window off-screen,
    # -novid skips the intro movie.
    set(_arguments "-insecure -game \"${_game_mod}\" -windowed -novid")
    if(NOT HALFLIFECLI_GAME_ARGUMENTS STREQUAL "")
        string(APPEND _arguments " ${HALFLIFECLI_GAME_ARGUMENTS}")
    endif()
    set_target_properties(LaunchGame PROPERTIES
        FOLDER "Launch-debugging"
        VS_DEBUGGER_COMMAND "${_launcher}"
        VS_DEBUGGER_WORKING_DIRECTORY "${_game_directory}"
        VS_DEBUGGER_COMMAND_ARGUMENTS "${_arguments}"
        VS_GLOBAL_DebuggerFlavor WindowsLocalDebugger
        VS_GLOBAL_DebuggerType NativeOnly
        VS_GLOBAL_DisableFastUpToDateCheck true)
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT LaunchGame)

    # No output stamp: every build/F5 must redeploy, even if sources are unchanged.
    add_custom_target(DeployGame
        COMMAND "${CMAKE_COMMAND}" "-DSETTINGS=${_launch_root}/settings.cmake"
            "-DCONFIG=$<CONFIG>" -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DeployGame.cmake"
        COMMENT "Deploying HalflifeCLI to ${_game_directory} ($<CONFIG>)"
        VERBATIM USES_TERMINAL)
    set_target_properties(DeployGame PROPERTIES
        FOLDER "Launch-debugging"
        VS_GLOBAL_DisableFastUpToDateCheck true)
    add_dependencies(DeployGame HalflifeCLI)
    add_dependencies(LaunchGame DeployGame)
    message(STATUS "LaunchGame debugger: ${_launcher} ${_arguments}")
endfunction()

foreach(dependency METAHOOK VGUI2EXTENSION)
    # Normalize untyped -D paths before CACHE PATH can resolve them from the
    # invoking shell's directory instead of the project root.
    if(DEFINED ${dependency}_SOURCE_PATH AND NOT "${${dependency}_SOURCE_PATH}" STREQUAL ""
        AND NOT IS_ABSOLUTE "${${dependency}_SOURCE_PATH}")
        get_filename_component(source_path "${${dependency}_SOURCE_PATH}"
            ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
        set(${dependency}_SOURCE_PATH "${source_path}" CACHE PATH
            "${dependency} source tree; empty fetches the pinned version" FORCE)
    endif()
    set(${dependency}_SOURCE_PATH "$ENV{${dependency}_SOURCE_PATH}" CACHE PATH
        "${dependency} source tree; empty fetches the pinned version")
endforeach()

function(halflife_cli_require_files name source)
    foreach(required IN LISTS ARGN)
        if(NOT EXISTS "${source}/${required}" OR IS_DIRECTORY "${source}/${required}")
            message(FATAL_ERROR "${name} is missing ${required}: ${source}")
        endif()
    endforeach()
endfunction()

function(halflife_cli_fetch_source name url commit out_var)
    include(FetchContent)
    set(submodules "")
    if(name STREQUAL "halflife_cli_metahook")
        set(submodules thirdparty/rapidjson)
    endif()
    FetchContent_Declare(${name}
        GIT_REPOSITORY "${url}" GIT_TAG "${commit}"
        GIT_SUBMODULES "${submodules}" GIT_SUBMODULES_RECURSE FALSE
        # Fetch headers and sources without configuring the dependency project.
        SOURCE_SUBDIR _halflife_cli_source_only)
    FetchContent_MakeAvailable(${name})
    set(${out_var} "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(halflife_cli_prepare_dependencies)
    set(METAHOOK_files include/metahook.h include/HLSDK/common/interface.cpp
        thirdparty/rapidjson/include/rapidjson/document.h)
    set(VGUI2EXTENSION_files include/Interface/IVGUI2Extension.h)

    # Validate every explicit path before starting any downloads.
    foreach(dependency METAHOOK VGUI2EXTENSION)
        if(${dependency}_SOURCE_PATH)
            get_filename_component(${dependency}_SOURCE_PATH "${${dependency}_SOURCE_PATH}"
                ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
            halflife_cli_require_files("${dependency}_SOURCE_PATH"
                "${${dependency}_SOURCE_PATH}" ${${dependency}_files})
        endif()
    endforeach()

    set(METAHOOK_url https://github.com/MetaHookSv/MetaHook)
    set(METAHOOK_commit 1d23fe946e6f0f09a1a892aa2156c3b462774026)
    set(VGUI2EXTENSION_url https://github.com/MetaHookSv/VGUI2Extension)
    set(VGUI2EXTENSION_commit 07933adf727f8a9a5443d4591d4c1be23b1b9edf)
    foreach(dependency METAHOOK VGUI2EXTENSION)
        if(NOT ${dependency}_SOURCE_PATH)
            string(TOLOWER "${dependency}" name)
            halflife_cli_fetch_source(halflife_cli_${name}
                "${${dependency}_url}" "${${dependency}_commit}" ${dependency}_SOURCE_PATH)
        endif()
        halflife_cli_require_files("${dependency}_SOURCE_PATH"
            "${${dependency}_SOURCE_PATH}" ${${dependency}_files})
        set(${dependency}_SOURCE_PATH "${${dependency}_SOURCE_PATH}" PARENT_SCOPE)
        message(STATUS "${dependency}_SOURCE_PATH: ${${dependency}_SOURCE_PATH}")
    endforeach()
endfunction()

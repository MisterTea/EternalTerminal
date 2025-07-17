#[===[.md:
# vcpkg_gn_configure

Generate Ninja (GN) targets

## Usage:
```cmake
vcpkg_gn_configure(
    SOURCE_PATH <SOURCE_PATH>
    [OPTIONS <OPTIONS>]
    [OPTIONS_DEBUG <OPTIONS_DEBUG>]
    [OPTIONS_RELEASE <OPTIONS_RELEASE>]
)
```

## Parameters:
### SOURCE_PATH (required)
The path to the GN project.

### OPTIONS
Options to be passed to both the debug and release targets.
Note: Must be provided as a space-separated string.

### OPTIONS_DEBUG (space-separated string)
Options to be passed to the debug target.

### OPTIONS_RELEASE (space-separated string)
Options to be passed to the release target.
#]===]
if(Z_VCPKG_GN_CONFIGURE_GUARD)
    return()
endif()
set(Z_VCPKG_GN_CONFIGURE_GUARD ON CACHE INTERNAL "guard variable")

function(z_vcpkg_gn_configure_generate)
    cmake_parse_arguments(PARSE_ARGV 0 "arg" "" "SOURCE_PATH;CONFIG;ARGS" "")
    if(DEFINED arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Internal error: generate was passed extra arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()

    message(STATUS "Generating build (${arg_CONFIG})...")
    vcpkg_execute_required_process(
        COMMAND "${VCPKG_GN}" gen "${CURRENT_BUILDTREES_DIR}/${arg_CONFIG}" "${arg_ARGS}"
        WORKING_DIRECTORY "${arg_SOURCE_PATH}"
        LOGNAME "generate-${arg_CONFIG}"
    )
endfunction()

function(vcpkg_gn_configure)
    cmake_parse_arguments(PARSE_ARGV 0 "arg" "" "SOURCE_PATH;OPTIONS;OPTIONS_DEBUG;OPTIONS_RELEASE" "")

    if(DEFINED arg_UNPARSED_ARGUMENTS)
        message(WARNING "vcpkg_gn_configure was passed extra arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT DEFINED arg_SOURCE_PATH)
        message(FATAL_ERROR "SOURCE_PATH must be specified.")
    endif()

    vcpkg_find_acquire_program(PYTHON2)
    get_filename_component(PYTHON2_DIR "${PYTHON2}" DIRECTORY)
    vcpkg_add_to_path(PREPEND "${PYTHON2_DIR}")

    vcpkg_find_acquire_program(GN)

    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
        z_vcpkg_gn_configure_generate(
            SOURCE_PATH "${arg_SOURCE_PATH}"
            CONFIG "${TARGET_TRIPLET}-dbg"
            ARGS "--args=${arg_OPTIONS} ${arg_OPTIONS_DEBUG}"
        )
    endif()

    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "release")
        z_vcpkg_gn_configure_generate(
            SOURCE_PATH "${arg_SOURCE_PATH}"
            CONFIG "${TARGET_TRIPLET}-rel"
            ARGS "--args=${arg_OPTIONS} ${arg_OPTIONS_RELEASE}"
        )
    endif()
endfunction()

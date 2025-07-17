
# It is possible to run into some issues when profiling when we uses Tracy client as a shared client
# As as safety measure let's build Tracy as a static library for now
# More details on Tracy Discord (e.g. https://discord.com/channels/585214693895962624/585214693895962630/953599951328403506)
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wolfpld/tracy
    REF a8511d357650282c6915f7aa9775e9c4945e42b5
    SHA512 6e0ff0be7621490b607eaae46d4ec13ea66b0061ed580c99d75eda3a7c9479b34c70da9ad844bd4a05e329bb40cebf48e7c6bab05d2a946d56b2d432c7ddc125
    HEAD_REF master
    PATCHES
        001-fix-vcxproj-vcpkg.patch
        002-fix-capstone-5.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
)
vcpkg_cmake_install()

if(VCPKG_TARGET_IS_LINUX)
    set(any_tracy_tool_requested OFF)
    if(profiler IN_LIST FEATURES)
        message(WARNING
"Tracy currently requires the following libraries from the system package manager to build its tools:
    gtk+-3.0
    tbb

These can be installed on Ubuntu systems via sudo apt install libgtk-3-dev libtbb-dev")
        set(any_tracy_tool_requested ON)
    else()
        foreach(CLI_TOOL capture csvexport import-chrome update)
            if(${CLI_TOOL} IN_LIST FEATURES)
                message(WARNING
"Tracy currently requires the following libraries from the system package manager to build its tools:
    tbb

These can be installed on Ubuntu systems via sudo apt install libtbb-dev")
                set(any_tracy_tool_requested ON)
                break()
            endif()
        endforeach()
    endif()

endif()

vcpkg_list(SET tracy_tools)
if("cli-tools" IN_LIST FEATURES)
    vcpkg_list(APPEND tracy_tools capture csvexport import-chrome update)
endif()
if("gui-tools" IN_LIST FEATURES)
    vcpkg_list(APPEND tracy_tools profiler)
endif()

function(tracy_tool_install_make tracy_TOOL tracy_TOOL_NAME)
    foreach(buildtype IN ITEMS "debug" "release")
        if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "${buildtype}")
            if("${buildtype}" STREQUAL "debug")
                set(short_buildtype "-dbg")
                set(path_suffix "/debug")
            else()
                set(short_buildtype "-rel")
                set(path_suffix "")
            endif()

            file(COPY "${SOURCE_PATH}/${tracy_TOOL}/build/unix" DESTINATION "${SOURCE_PATH}/${tracy_TOOL}/_build")
            file(RENAME "${SOURCE_PATH}/${tracy_TOOL}/_build/unix" "${SOURCE_PATH}/${tracy_TOOL}/build/unix${short_buildtype}")
            file(REMOVE_RECURSE "${SOURCE_PATH}/${tracy_TOOL}/_build")

            set(path_makefile_dir "${SOURCE_PATH}/${tracy_TOOL}/build/unix${short_buildtype}")
            cmake_path(RELATIVE_PATH path_makefile_dir 
                BASE_DIRECTORY "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}${short_buildtype}"
                OUTPUT_VARIABLE relative_path_makefile_dir)

            vcpkg_backup_env_variables(VARS PKG_CONFIG_PATH)
            vcpkg_host_path_list(PREPEND ENV{PKG_CONFIG_PATH} "${CURRENT_INSTALLED_DIR}${path_suffix}/lib/pkgconfig")

            message(STATUS "Building ${tracy_TOOL_NAME} ${TARGET_TRIPLET}${short_buildtype}")
            vcpkg_build_make(
                BUILD_TARGET ${buildtype}
                SUBPATH ${relative_path_makefile_dir}
                LOGFILE_ROOT "build-${tracy_TOOL}"
            )
            vcpkg_restore_env_variables(VARS PKG_CONFIG_PATH)

            file(INSTALL "${SOURCE_PATH}/${tracy_TOOL}/build/unix${short_buildtype}/${tracy_TOOL_NAME}-${buildtype}" DESTINATION "${CURRENT_PACKAGES_DIR}${path_suffix}/tools/${PORT}" RENAME "${tracy_TOOL_NAME}")
        endif()
    endforeach()
endfunction()

if("capture" IN_LIST tracy_tools)
    if(VCPKG_TARGET_IS_WINDOWS)
        vcpkg_install_msbuild(
            SOURCE_PATH "${SOURCE_PATH}"
            PROJECT_SUBPATH "capture/build/win32/capture.vcxproj"
            USE_VCPKG_INTEGRATION
        )
    else()
        tracy_tool_install_make(capture capture)
    endif()
endif()

if("csvexport" IN_LIST tracy_tools)
    if(VCPKG_TARGET_IS_WINDOWS)
        vcpkg_install_msbuild(
            SOURCE_PATH "${SOURCE_PATH}"
            PROJECT_SUBPATH "csvexport/build/win32/csvexport.vcxproj"
            USE_VCPKG_INTEGRATION
        )
    else()
        tracy_tool_install_make(csvexport csvexport)
    endif()
endif()

if("import-chrome" IN_LIST tracy_tools)
    if(VCPKG_TARGET_IS_WINDOWS)
        vcpkg_install_msbuild(
            SOURCE_PATH "${SOURCE_PATH}"
            PROJECT_SUBPATH "import-chrome/build/win32/import-chrome.vcxproj"
            USE_VCPKG_INTEGRATION
        )
    else()
        tracy_tool_install_make(import-chrome import-chrome)
    endif()
endif()

if("profiler" IN_LIST tracy_tools)
    if(VCPKG_TARGET_IS_WINDOWS)
        vcpkg_install_msbuild(
            SOURCE_PATH "${SOURCE_PATH}"
            PROJECT_SUBPATH "profiler/build/win32/Tracy.vcxproj"
            USE_VCPKG_INTEGRATION
        )
    else()
        tracy_tool_install_make(profiler Tracy)
    endif()
endif()

if("update" IN_LIST tracy_tools)
    if(VCPKG_TARGET_IS_WINDOWS)
        vcpkg_install_msbuild(
            SOURCE_PATH "${SOURCE_PATH}"
            PROJECT_SUBPATH "update/build/win32/update.vcxproj"
            USE_VCPKG_INTEGRATION
        )
    else()
        tracy_tool_install_make(update update)
    endif()
endif()

vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME Tracy)
vcpkg_fixup_pkgconfig()

foreach(TOOL capture csvexport import-chrome profiler update)
    if(${TOOL} IN_LIST tracy_tools)
        if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
            vcpkg_copy_tool_dependencies("${CURRENT_PACKAGES_DIR}/debug/tools/${PORT}")
        endif()
        if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "release")
            vcpkg_copy_tool_dependencies("${CURRENT_PACKAGES_DIR}/tools/${PORT}")
        endif()
        break()
    endif()
endforeach()

# Handle copyright
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

# Cleanup
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

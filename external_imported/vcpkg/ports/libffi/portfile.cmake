vcpkg_minimum_required(VERSION 2022-10-12) # for ${VERSION}

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO libffi/libffi
    REF "v${VERSION}"
    SHA512 e3b261a7900cec61225c768ebd443884465669e0904db3f523aaaeeed74b4c03dbe23d74ff8bb69554791a798e25894a5fcbe2b13b883d3ee38aeff4c1e16a49
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/libffiConfig.cmake.in" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DFFI_CONFIG_FILE=${CMAKE_CURRENT_LIST_DIR}/fficonfig.h"
    OPTIONS_DEBUG
        -DFFI_SKIP_HEADERS=ON
)

vcpkg_cmake_install()

# Create pkgconfig file
set(PACKAGE_VERSION ${VERSION})
set(prefix "${CURRENT_INSTALLED_DIR}")
set(exec_prefix "\${prefix}")
set(libdir "\${prefix}/lib")
set(toolexeclibdir "\${libdir}")
set(includedir "\${prefix}/include")
configure_file("${SOURCE_PATH}/libffi.pc.in" "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libffi.pc" @ONLY)

# debug
if (NOT VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
  set(prefix "${CURRENT_INSTALLED_DIR}/debug")
  set(exec_prefix "\${prefix}")
  set(libdir "\${prefix}/lib")
  set(toolexeclibdir "\${libdir}")
  set(includedir "\${prefix}/../include")
    configure_file("${SOURCE_PATH}/libffi.pc.in" "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libffi.pc" @ONLY)
endif()

vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup()

if(VCPKG_TARGET_IS_WINDOWS OR VCPKG_TARGET_IS_MINGW)
    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "release")
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libffi.pc"
            "-lffi" "-llibffi")
    endif()
    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libffi.pc"
            "-lffi" "-llibffi")
    endif()
endif()
vcpkg_fixup_pkgconfig()

if (VCPKG_LIBRARY_LINKAGE STREQUAL static)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/ffi.h"
        "   *know* they are going to link with the static library.  */"
        "   *know* they are going to link with the static library.  */

#define FFI_BUILDING
"
    )
endif()

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

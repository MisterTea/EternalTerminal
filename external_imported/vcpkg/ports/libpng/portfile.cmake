# Download the apng patch
set(LIBPNG_APNG_PATCH_PATH "")
set(LIBPNG_APNG_OPTION "")
if ("apng" IN_LIST FEATURES)
    if(VCPKG_HOST_IS_WINDOWS)
        # Get (g)awk and gzip installed
        vcpkg_acquire_msys(MSYS_ROOT PACKAGES gawk gzip)
        set(AWK_EXE_PATH "${MSYS_ROOT}/usr/bin")
        vcpkg_add_to_path("${AWK_EXE_PATH}")
    endif()
    
    set(LIBPNG_APNG_PATCH_NAME "libpng-${VERSION}-apng.patch")
    vcpkg_download_distfile(LIBPNG_APNG_PATCH_ARCHIVE
        URLS "https://downloads.sourceforge.net/project/libpng-apng/libpng16/${VERSION}/${LIBPNG_APNG_PATCH_NAME}.gz"
        FILENAME "${LIBPNG_APNG_PATCH_NAME}.gz"
        SHA512 97a182da0b3b54aecf735e3655d8e8f1a569ae957b23fc3d7a9c8cc65dcdd26f34f173ce9f60af99b01d5347267b2afefaf787c500ce1005e86bf2810b3d0738
    )
    set(LIBPNG_APNG_PATCH_PATH "${CURRENT_BUILDTREES_DIR}/src/${LIBPNG_APNG_PATCH_NAME}")
    if (NOT EXISTS "${LIBPNG_APNG_PATCH_PATH}")
        file(INSTALL "${LIBPNG_APNG_PATCH_ARCHIVE}" DESTINATION "${CURRENT_BUILDTREES_DIR}/src")
        vcpkg_execute_required_process(
            COMMAND gzip -d "${LIBPNG_APNG_PATCH_NAME}.gz"
            WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}/src"
            ALLOW_IN_DOWNLOAD_MODE
            LOGNAME extract-patch.log
        )
    endif()
    set(LIBPNG_APNG_OPTION "-DPNG_PREFIX=a")
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO glennrp/libpng
    REF v${VERSION}
    SHA512 d61408cee5850582baa57166547ccab6cc171bc809076e53494ace26157fd7787c3209e3b757fd68c541bfb95afe309745d887fb5cd2005b2024af7355c809a0
    HEAD_REF master
    PATCHES
        "${LIBPNG_APNG_PATCH_PATH}"
        cmake.patch
        fix-export-targets.patch
        pkgconfig.patch
        fix-msa-support-for-mips.patch
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" PNG_SHARED)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" PNG_STATIC)

vcpkg_list(SET LIBPNG_HARDWARE_OPTIMIZATIONS_OPTION)
if(VCPKG_TARGET_IS_IOS)
    vcpkg_list(APPEND LIBPNG_HARDWARE_OPTIMIZATIONS_OPTION "-DPNG_HARDWARE_OPTIMIZATIONS=OFF")
endif()

vcpkg_list(SET LD_VERSION_SCRIPT_OPTION)
if(VCPKG_TARGET_IS_ANDROID)
    vcpkg_list(APPEND LD_VERSION_SCRIPT_OPTION "-Dld-version-script=OFF")
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm")
        vcpkg_cmake_get_vars(cmake_vars_file)
        include("${cmake_vars_file}")
        if(VCPKG_DETECTED_CMAKE_ANDROID_ARM_NEON)
            vcpkg_list(APPEND LIBPNG_HARDWARE_OPTIMIZATIONS_OPTION "-DPNG_ARM_NEON=on")
        else()
            # for armeabi-v7a, check whether NEON is available
            vcpkg_list(APPEND LIBPNG_HARDWARE_OPTIMIZATIONS_OPTION "-DPNG_ARM_NEON=check")
        endif()
    endif()
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${LIBPNG_APNG_OPTION}
        ${LIBPNG_HARDWARE_OPTIMIZATIONS_OPTION}
        ${LD_VERSION_SCRIPT_OPTION}
        -DPNG_STATIC=${PNG_STATIC}
        -DPNG_SHARED=${PNG_SHARED}
        -DPNG_TESTS=OFF
        -DSKIP_INSTALL_PROGRAMS=ON
        -DSKIP_INSTALL_EXECUTABLES=ON
        -DSKIP_INSTALL_FILES=OFF
    OPTIONS_DEBUG
        -DSKIP_INSTALL_HEADERS=ON
    MAYBE_UNUSED_VARIABLES
        PNG_ARM_NEON
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/libpng)
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/png")

vcpkg_fixup_pkgconfig()
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    if(NOT VCPKG_BUILD_TYPE)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libpng16.pc" "-lpng16" "-llibpng16d")
        file(INSTALL "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libpng16.pc" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig" RENAME "libpng.pc")
    endif()
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libpng16.pc" "-lpng16" "-llibpng16")
elseif(NOT VCPKG_BUILD_TYPE)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libpng16.pc" "-lpng16" "-lpng16d")
    file(INSTALL "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libpng16.pc" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig" RENAME "libpng.pc")
endif()
file(INSTALL "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libpng16.pc" DESTINATION "${CURRENT_PACKAGES_DIR}/lib/pkgconfig" RENAME "libpng.pc")

vcpkg_copy_pdbs()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO PDAL/PDAL
    REF "${VERSION}"
    SHA512 cefc610682f8dafd5c186ed612edc2db904690c3a53d5111ece0965d197053b064bd8cbd9adab293c47ec1894949b5e33623b0f0e6b6cad35617a20f0039bd79
    HEAD_REF master
    PATCHES
        fix-dependency.patch
        fix-unix-compiler-options.patch
        fix-find-library-suffix.patch
        no-pkgconfig-requires.patch
        no-rpath.patch
        fix-gcc-13-build.patch  #upstream PR: https://github.com/PDAL/PDAL/pull/4039
        gdal-3.7.patch
        mingw.patch
        install-dimbuilder.patch
)

# Prefer pristine CMake find modules + wrappers and config files from vcpkg.
foreach(package IN ITEMS Curl GeoTIFF ICONV ZSTD)
    file(REMOVE "${SOURCE_PATH}/cmake/modules/Find${package}.cmake")
endforeach()

# De-vendoring
file(REMOVE_RECURSE
    "${SOURCE_PATH}/vendor/nanoflann"
    "${SOURCE_PATH}/vendor/nlohmann"
    "${SOURCE_PATH}/pdal/JsonFwd.hpp"
)
file(INSTALL "${CURRENT_INSTALLED_DIR}/include/nanoflann.hpp" DESTINATION "${SOURCE_PATH}/vendor/nanoflann")
file(INSTALL "${CURRENT_INSTALLED_DIR}/include/nlohmann/json.hpp" DESTINATION "${SOURCE_PATH}/vendor/nlohmann/nlohmann")
file(APPEND "${SOURCE_PATH}/vendor/nlohmann/nlohmann/json.hpp" "namespace NL = nlohmann;\n")
file(INSTALL "${CURRENT_INSTALLED_DIR}/include/nlohmann/json_fwd.hpp" DESTINATION "${SOURCE_PATH}/pdal")
file(RENAME "${SOURCE_PATH}/pdal/json_fwd.hpp" "${SOURCE_PATH}/pdal/JsonFwd.hpp")
file(APPEND "${SOURCE_PATH}/pdal/JsonFwd.hpp" "namespace NL = nlohmann;\n")

unset(ENV{OSGEO4W_HOME})

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        draco       BUILD_PLUGIN_DRACO
        e57         BUILD_PLUGIN_E57
        hdf5        BUILD_PLUGIN_HDF
        i3s         BUILD_PLUGIN_I3S
        lzma        WITH_LZMA
        pgpointcloud BUILD_PLUGIN_PGPOINTCLOUD
        zstd        WITH_ZSTD
)

if(VCPKG_CROSSCOMPILING)
    set(DIMBUILDER_EXECUTABLE "-DDIMBUILDER_EXECUTABLE=${CURRENT_HOST_INSTALLED_DIR}/tools/pdal/dimbuilder${VCPKG_HOST_EXECUTABLE_SUFFIX}")
endif()

vcpkg_find_acquire_program(PKGCONFIG)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/cmake-project-include.cmake"
        -DPDAL_PLUGIN_INSTALL_PATH=.
        "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}"
        -DWITH_TESTS:BOOL=OFF
        -DWITH_COMPLETION:BOOL=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_Libexecinfo:BOOL=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Libunwind:BOOL=ON
        ${FEATURE_OPTIONS}
        ${DIMBUILDER_EXECUTABLE}
    MAYBE_UNUSED_VARIABLES
        PKG_CONFIG_EXECUTABLE
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/PDAL)
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

# Install and cleanup executables
file(GLOB pdal_unsupported
    "${CURRENT_PACKAGES_DIR}/bin/*.bat"
    "${CURRENT_PACKAGES_DIR}/bin/pdal-config"
    "${CURRENT_PACKAGES_DIR}/debug/bin/*.bat"
    "${CURRENT_PACKAGES_DIR}/debug/bin/*.exe"
    "${CURRENT_PACKAGES_DIR}/debug/bin/pdal-config"
)
file(REMOVE ${pdal_unsupported})
vcpkg_copy_tools(TOOL_NAMES pdal dimbuilder AUTO_CLEAN)

# Post-install clean-up
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/include/pdal/filters/private/csf"
    "${CURRENT_PACKAGES_DIR}/include/pdal/filters/private/miniball"
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

file(READ "${SOURCE_PATH}/LICENSE.txt" pdal_license)
file(READ "${SOURCE_PATH}/vendor/arbiter/LICENSE" arbiter_license)
file(READ "${SOURCE_PATH}/vendor/kazhdan/PoissonRecon.h" kazhdan_license)
string(REGEX REPLACE "^/\\*\n|\\*/.*\$" "" kazhdan_license "${kazhdan_license}")
file(READ "${SOURCE_PATH}/vendor/lazperf/lazperf.hpp" lazperf_license)
string(REGEX REPLACE "^/\\*\n|\\*/.*\$" "" lazperf_license "${lazperf_license}")
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright"
"${pdal_license}
---

Files in vendor/arbiter/:

${arbiter_license}
---

Files in vendor/kazhdan/:

${kazhdan_license}
---

Files in vendor/lazperf/:

${lazperf_license}
---

Files in vendor/eigen:

Most Eigen source code is subject to the terms of the Mozilla Public License
v. 2.0. You can obtain a copy the MPL 2.0 at http://mozilla.org/MPL/2.0/.

Some files included in Eigen are under one of the following licenses:
 - Apache License, Version 2.0 
 - BSD 3-Clause \"New\" or \"Revised\" License
")

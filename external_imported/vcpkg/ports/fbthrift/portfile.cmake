vcpkg_find_acquire_program(FLEX)
vcpkg_find_acquire_program(BISON)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO facebook/fbthrift
    REF v2022.03.21.00
    SHA512 8d2d9430dc3a4ecc23042cd9bcf4eee888824449d05d98baec408aef806b934d643e578d3876169f69966c846aeddbe0aa84416c4e020cba028a49d2fccfe7ab
    HEAD_REF master
    PATCHES 
        0001-fix-compatibility-with-boost-1.79.0.patch
        fix-glog.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DBISON_EXECUTABLE=${BISON}"
        "-DFLEX_EXECUTABLE=${FLEX}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/fbthrift)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

# There should be no empty directories in vcpkg/packages/fbthrift_x64-linux
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp/transport/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp/util/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/transport/http2/server/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/transport/http2/common/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/transport/http2/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/transport/core/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/transport/inmemory/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/protocol/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/security/extensions/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/security/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/frozen/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/reflection/docs"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/util/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/visitation/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/cpp2/server/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/py3/test"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/py3/benchmark"
    "${CURRENT_PACKAGES_DIR}/include/thrift/lib/thrift/annotation"
)

vcpkg_copy_tools(TOOL_NAMES thrift1 AUTO_CLEAN)
vcpkg_copy_pdbs()

if(EXISTS "${CURRENT_PACKAGES_DIR}/share/fbthrift/FBThriftConfig.cmake")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/fbthrift/FBThriftConfig.cmake" 
        "${PACKAGE_PREFIX_DIR}/lib/cmake/fbthrift" "${PACKAGE_PREFIX_DIR}/share/fbthrift")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/fbthrift/FBThriftConfig.cmake" 
        "${PACKAGE_PREFIX_DIR}/bin/thrift1.exe" "${PACKAGE_PREFIX_DIR}/tools/fbthrift/thrift1.exe")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/fbthrift/FBThriftConfig.cmake" 
        "${PACKAGE_PREFIX_DIR}/bin/thrift1" "${PACKAGE_PREFIX_DIR}/tools/fbthrift/thrift1")
endif()

# Only used internally and removed in master
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/fbthrift/FBThriftTargets.cmake" "LOCATION_HH=\\\"${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/thrift/compiler/location.hh\\\"" "")

# Handle copyright
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

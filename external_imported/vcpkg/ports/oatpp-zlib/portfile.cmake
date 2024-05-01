set(OATPP_VERSION "1.3.0")

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO oatpp/oatpp-zlib
    REF ${OATPP_VERSION}
    SHA512 574f0440cbb2cd2bc14ad89e33538a1a300ad23ecc941629b74aa8ccb9aeae5158b1b57e2f1af09d7a6b9b97430a5685354677002dab2261120afa9c6ea74381
    HEAD_REF master
    PATCHES
        missing-find_dependency.patch
        fix-usage.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DOATPP_BUILD_TESTS:BOOL=OFF"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME oatpp-zlib CONFIG_PATH lib/cmake/oatpp-zlib-${OATPP_VERSION})
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO libcheck/check
    REF 11970a7e112dfe243a2e68773f014687df2900e8 # 0.15.2
    SHA512 210c9617fa1c1ce16bef983b0e6cb587b1774c3f7ce27a53ca7799642dc7a14be8de567d69dc0e57845684c6f7991d772c73654f63c8755afda3b37a35c7156e
    HEAD_REF master
    PATCHES fix-lib-path.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCHECK_ENABLE_TESTS=OFF
        -DBUILD_TESTING=OFF
        -DCHECK_ENABLE_GCOV=OFF
        -DENABLE_MEMORY_LEAKING_TESTS=OFF
        -DCHECK_ENABLE_TIMEOUT_TESTS=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/check)

vcpkg_copy_pdbs()

if(VCPKG_LIBRARY_LINKAGE STREQUAL static)
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

# cleanup
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/man")

vcpkg_fixup_pkgconfig()

file(INSTALL "${SOURCE_PATH}/COPYING.LESSER" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

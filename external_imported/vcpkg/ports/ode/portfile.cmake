vcpkg_from_bitbucket(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO odedevs/ode
    REF ${VERSION}
    SHA512 d253bc06bca85ed5af95e92acc57b5a9c4382393365c4a0c852a5872a4061ce1bff875a9121624276a60d74e919fb976c2a2b37864203f6c3bcd2def8af953c6
    HEAD_REF master
    PATCHES
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DODE_WITH_DEMOS=0
        -DODE_WITH_TESTS=0
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/ode-${VERSION})

if(VCPKG_LIBRARY_LINKAGE STREQUAL static)
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")
else()
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/bin/ode-config" "${CURRENT_PACKAGES_DIR}" "`dirname $0`/..")
    if(NOT VCPKG_BUILD_TYPE)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/bin/ode-config" "${CURRENT_PACKAGES_DIR}" "`dirname $0`/../..")
    endif()
endif()

file(INSTALL "${SOURCE_PATH}/COPYING" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/lib/cmake")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib/cmake")

vcpkg_fixup_pkgconfig()

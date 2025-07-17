vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO marzer/tomlplusplus
    REF v2.5.0
    SHA512 7394cda2009b37e88f9028ee5d1887120bed7042833c7cb218d7705cdc92273c81a84163ff0be034d3f23c8dd93e63b7615134a4b0e1c580e1e945fae45c7d35
    HEAD_REF master
)

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Dgenerate_cmake_config=true
        -Dbuild_tests=false
        -Dbuild_examples=false
)

vcpkg_install_meson()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/${PORT})
cmake_path(NATIVE_PATH SOURCE_PATH native_source_path)
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/tomlplusplus/tomlplusplusConfig.cmake" "${native_source_path}" "")
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug"
                    "${CURRENT_PACKAGES_DIR}/lib")

file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)

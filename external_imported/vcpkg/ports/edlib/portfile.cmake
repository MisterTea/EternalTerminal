vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Martinsos/edlib
    REF v1.2.7
    SHA512 720C732C76D0D9ABE28ADCE9972B355864571A2E6CBD2C72C3B4A92E045A99E3A688153865586F7E8B6C90433E2EB1BB024AD0A6E9D4EB4B4D401A160D3F13CE
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/edlib)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

# Handle copyright
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

vcpkg_fixup_pkgconfig()

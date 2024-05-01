vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO CoolProp/IF97
    REF v2.1.3
    SHA512 b29a74f134d69b72ba4bb825b25f0631f2fb335500da5d9016c4e6e417d8c93a5b309e95770eb6a7b723948dd82a7b46d873a1fe4e3f3047a881603442d73eff
    HEAD_REF master
    PATCHES
        relax-encoding.diff
)

file(INSTALL "${SOURCE_PATH}/IF97.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

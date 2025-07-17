# Header-only library

vcpkg_fail_port_install(ON_TARGET "UWP" "LINUX" "OSX" "FREEBSD" "ANDROID" "MINGW")

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO microsoft/krabsetw
    REF 31679cf84bc85360158672699f2f68a821e8a6d0
    SHA512 4fcc4ee1c41c6d40770a5b57111e6fd29eedf1f4a29038ab1dfb8bffb3ad0464c4ec06b90b65fabadcd419564d55172d4d9fdc3750c1898545f7c6e00fbe99c8
    HEAD_REF master
)

file(INSTALL ${SOURCE_PATH}/krabs/krabs/ DESTINATION ${CURRENT_PACKAGES_DIR}/include/krabs)
file(INSTALL ${SOURCE_PATH}/krabs/krabs.hpp DESTINATION ${CURRENT_PACKAGES_DIR}/include)
file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)

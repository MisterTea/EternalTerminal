set(VCPKG_POLICY_EMPTY_PACKAGE enabled)
vcpkg_minimum_required(VERSION 2022-10-12) # for ${VERSION}

vcpkg_download_distfile(LICENSE
    URLS "https://raw.githubusercontent.com/boostorg/boost/boost-${VERSION}/LICENSE_1_0.txt"
    FILENAME "boost_LICENSE_1_0.txt"
    SHA512 d6078467835dba8932314c1c1e945569a64b065474d7aced27c9a7acc391d52e9f234138ed9f1aa9cd576f25f12f557e0b733c14891d42c16ecdc4a7bd4d60b8
)

file(INSTALL
    "${CMAKE_CURRENT_LIST_DIR}/boost-modular-headers.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/usage"
    "${LICENSE}"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
)

vcpkg_install_copyright(FILE_LIST "${CMAKE_CURRENT_LIST_DIR}/LICENSE.txt")

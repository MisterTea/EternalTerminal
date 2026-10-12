vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO altlinux/libutempter
    REF 1623010f57c1224ffcc2cf6f4485a2fc3ff5876d # 1.2.1-alt1
    SHA512 1b14ee186fe307b366974e20dabd815e65e8d72d74aa26681089946ad8240abf4180539e575ce75db6fba7933e0f47b03d4c55d3d1fec7622b252810b0ff8646
)
if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    set(HELPER_LIBEXEC_DIR /usr/lib/x86_64-linux-gnu)
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    set(HELPER_LIBEXEC_DIR /usr/lib/aarch64-linux-gnu)
else()
    message(FATAL_ERROR "MSan libutempter supports Ubuntu x64 and arm64")
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" "${SOURCE_PATH}/libutempter/CMakeLists.txt" COPYONLY)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/libutempter"
    OPTIONS "-DHELPER_LIBEXEC_DIR=${HELPER_LIBEXEC_DIR}"
)
vcpkg_cmake_install()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/libutempter/COPYING")

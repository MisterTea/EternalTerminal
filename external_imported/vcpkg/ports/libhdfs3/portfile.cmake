vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO erikmuttersbach/libhdfs3
    REF 9a60d79812d6dee72455f61bff57a93c3c7d56f5
    SHA512 2b635ab979230c251243f01717105872245d7948f75832e58f50a09b0b06d1b366b3c5f3a3253fa538076e9f199003f28d10b9958293144dbc301276073a0633
    HEAD_REF apache-rpc-9
    PATCHES
        fix_dependency_uuid.patch
)

if(VCPKG_TARGET_IS_LINUX )
    message(STATUS [[
To build libhdfs3, the following libraries are needed.
    libgsasl
This development package can be installed on the system by
    apt install libgsasl-dev
]])
elseif(VCPKG_TARGET_IS_OSX)
    message(WARNING [[
To build libhdfs3, the following libraries are needed.
    libgsasl
These development packages can be installed on the system via
    git clone https://gitlab.com/gsasl/gsasl.git]])
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
file(INSTALL "${SOURCE_PATH}/LICENSE.txt" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
FILE(INSTALL "${CMAKE_CURRENT_LIST_DIR}/libhdfs3Config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
FILE(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

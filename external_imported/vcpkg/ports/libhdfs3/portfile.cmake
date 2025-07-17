vcpkg_fail_port_install(ON_TARGET "UWP" "Windows")

vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO erikmuttersbach/libhdfs3
        REF 9a60d79812d6dee72455f61bff57a93c3c7d56f5
        SHA512 2b635ab979230c251243f01717105872245d7948f75832e58f50a09b0b06d1b366b3c5f3a3253fa538076e9f199003f28d10b9958293144dbc301276073a0633
        HEAD_REF apache-rpc-9
)

vcpkg_configure_cmake(
        SOURCE_PATH ${SOURCE_PATH}
        PREFER_NINJA
)

vcpkg_install_cmake()

vcpkg_copy_pdbs()

file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/include ${CURRENT_PACKAGES_DIR}/debug/share)
file(INSTALL ${SOURCE_PATH}/LICENSE.txt DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)
FILE(INSTALL ${CMAKE_CURRENT_LIST_DIR}/libhdfs3Config.cmake DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT})
FILE(INSTALL ${CMAKE_CURRENT_LIST_DIR}/usage DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT})

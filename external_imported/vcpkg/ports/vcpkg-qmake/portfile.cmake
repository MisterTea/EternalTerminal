file(INSTALL
    "${CMAKE_CURRENT_LIST_DIR}/vcpkg_qmake_configure.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/vcpkg_qmake_build.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/vcpkg_qmake_install.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/z_vcpkg_qmake_fix_makefiles.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/vcpkg-port-config.cmake"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

file(INSTALL "${VCPKG_ROOT_DIR}/LICENSE.txt" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
set(VCPKG_POLICY_CMAKE_HELPER_PORT enabled)

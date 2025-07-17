if (NOT VCPKG_TARGET_IS_OSX)
    vcpkg_fail_port_install(ON_ARCH "arm" "arm64" ON_TARGET "UWP")
endif()

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO bulletphysics/bullet3
    REF 3.17
    SHA512 a5105bf5f1dd365a64a350755c7d2c97942f74897a18dcdb3651e6732fd55cc1030a096f5808cf50575281f05e3ac09aa50a48d271a47b94cd61f5167a72b7cc
    HEAD_REF master
    PATCHES cmake-fix.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        multithreading       BULLET2_MULTITHREADING
)

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        -DUSE_MSVC_RUNTIME_LIBRARY_DLL=ON
        -DBUILD_CPU_DEMOS=OFF
        -DBUILD_BULLET2_DEMOS=OFF
        -DBUILD_BULLET3=OFF
        -DBUILD_EXTRAS=OFF
        -DBUILD_UNIT_TESTS=OFF
        -DINSTALL_LIBS=ON
        ${FEATURE_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(PACKAGE_NAME bullet CONFIG_PATH share/bullet)

vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/bullet/BulletInverseDynamics/details")

file(INSTALL "${SOURCE_PATH}/LICENSE.txt" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

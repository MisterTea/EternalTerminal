vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

if("public-preview" IN_LIST FEATURES)
    vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO Azure/azure-uhttp-c
        REF d84a20609a2b5a555920389451fb3c9a2ed3656c
        SHA512 4eadd7e120082cc3bcf696d6cd16bc7ee8e1082380dd7583fba7fad1bb95109f3456890495e25ae7675e656ef721fa12eff22eeb96d8a4cf359be5c96889cbd6
        HEAD_REF master
        PATCHES
            package-location-fix-preview.patch
    )
else()
    vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO Azure/azure-uhttp-c
        REF 2b86b92628e55c9597aef8b96b0a73048ef072b3
        SHA512 bc4cadaf4f64ffff4d8839fe8f604b77d6a912ab28b6100df93f4c7fbc96fa68a4e09b4670d27d078aac32a2d7792c408aa78a580328c02759c7a1c04d82d721
        HEAD_REF master
    )
endif()

file(COPY ${CURRENT_INSTALLED_DIR}/share/azure-c-shared-utility/azure_iot_build_rules.cmake DESTINATION ${SOURCE_PATH}/deps/c-utility/configs/)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Dskip_samples=ON
        -Duse_installed_dependencies=ON
        -Dbuild_as_dynamic=OFF
        -DCMAKE_INSTALL_INCLUDEDIR=include
    MAYBE_UNUSED_VARIABLES
        build_as_dynamic
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME uhttp CONFIG_PATH "lib/cmake/uhttp")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

configure_file("${SOURCE_PATH}/LICENSE" "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright" COPYONLY)

vcpkg_copy_pdbs()

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/flatbuffers
    REF v2.0.0
    SHA512 26a06b572c0e4c9685743bd2d2162ac7dcd74b9324624cc3f3ef5b154c0cee7c52a04b77cdc184245d2d6ae38dfdcc4fd66001c318aa8ca001d2bf1d85d66a89
    HEAD_REF master
    PATCHES
        ignore_use_of_cmake_toolchain_file.patch
        no-werror.patch
        fix-uwp-build.patch
)

set(OPTIONS)
if(VCPKG_TARGET_IS_UWP OR VCPKG_TARGET_IS_IOS)
    list(APPEND OPTIONS -DFLATBUFFERS_BUILD_FLATC=OFF -DFLATBUFFERS_BUILD_FLATHASH=OFF)
endif()

vcpkg_configure_cmake(
    SOURCE_PATH ${SOURCE_PATH}
    PREFER_NINJA
    OPTIONS
        -DFLATBUFFERS_BUILD_TESTS=OFF
        -DFLATBUFFERS_BUILD_GRPCTEST=OFF
        ${OPTIONS}
)

vcpkg_install_cmake()
vcpkg_fixup_cmake_targets(CONFIG_PATH lib/cmake/flatbuffers)

file(GLOB flatc_path ${CURRENT_PACKAGES_DIR}/bin/flatc*)
if(flatc_path)
    make_directory(${CURRENT_PACKAGES_DIR}/tools/flatbuffers)
    get_filename_component(flatc_executable ${flatc_path} NAME)
    file(
        RENAME
        ${flatc_path}
        ${CURRENT_PACKAGES_DIR}/tools/flatbuffers/${flatc_executable}
    )
    vcpkg_copy_tool_dependencies(${CURRENT_PACKAGES_DIR}/tools/flatbuffers)
endif()

file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/include)
file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/bin ${CURRENT_PACKAGES_DIR}/debug/bin)

# Handle copyright
file(INSTALL ${SOURCE_PATH}/LICENSE.txt DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)

vcpkg_fixup_pkgconfig()

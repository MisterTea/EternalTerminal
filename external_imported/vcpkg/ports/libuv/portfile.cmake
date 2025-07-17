vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO libuv/libuv
    REF v1.43.0
    SHA512 66ee11f8f6fc1313c432858572789cf67acd6364b29a06c73323ab20626e2d6e3d3dcea748cf5d9d4368b40ad7fe0d5fd35e9369c22e531db523703f005248d3
    HEAD_REF v1.x
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS_DEBUG
        -DUV_SKIP_HEADERS=ON
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-libuv CONFIG_PATH share/unofficial-libuv)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/unofficial-libuv-config.in.cmake"
    "${CURRENT_PACKAGES_DIR}/share/unofficial-libuv/unofficial-libuv-config.cmake"
    @ONLY
)

file(READ "${CURRENT_PACKAGES_DIR}/include/uv.h" UV_H)
if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    string(REPLACE "defined(USING_UV_SHARED)" "1" UV_H "${UV_H}")
else()
    string(REPLACE "defined(USING_UV_SHARED)" "0" UV_H "${UV_H}")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper.cmake" "${CURRENT_PACKAGES_DIR}/share/${PORT}/vcpkg-cmake-wrapper.cmake" @ONLY)
endif()
file(WRITE "${CURRENT_PACKAGES_DIR}/include/uv.h" "${UV_H}")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)


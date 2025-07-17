vcpkg_fail_port_install(ON_TARGET "uwp")

set(COMMIT_HASH 188427d7e18102c45fc6d0e20c135e226f215992)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO dotnet/runtime
    REF ${COMMIT_HASH}
    SHA512 5a93c66c87e2113f733702d938efd39456c99fb74b383097b8d877df21536fcbcba901606aa70db6c8f1a16421ea8f06822c5b0ab1d882631b6daecbed8d03cc
    HEAD_REF master
    PATCHES
        0001-nethost-cmakelists.patch
        0002-settings-cmake.patch
)

set(PRODUCT_VERSION "5.0.0")

if(VCPKG_TARGET_IS_WINDOWS)
  set(RID_PLAT "win")
elseif(VCPKG_TARGET_IS_OSX)
  set(RID_PLAT "osx")
elseif(VCPKG_TARGET_IS_LINUX)
  set(RID_PLAT "linux")
else()
  message(FATAL_ERROR "Unsupported platform")
endif()

if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
  set(RID_ARCH "x86")
  set(ARCH_NAME "I386")
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
  set(RID_ARCH "x64")
  set(ARCH_NAME "AMD64")
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm")
  set(RID_ARCH "arm")
  set(ARCH_NAME "ARM")
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
  set(RID_ARCH "arm64")
  set(ARCH_NAME "ARM64")
else()
  message(FATAL_ERROR "Unsupported architecture")
endif()

set(BASE_RID "${RID_PLAT}-${RID_ARCH}")

vcpkg_configure_cmake(
    SOURCE_PATH ${SOURCE_PATH}/src/installer/corehost/cli/nethost
    PREFER_NINJA
    OPTIONS
        "-DSKIP_VERSIONING=1"
        "-DCLI_CMAKE_HOST_POLICY_VER:STRING=${PRODUCT_VERSION}"
        "-DCLI_CMAKE_HOST_FXR_VER:STRING=${PRODUCT_VERSION}"
        "-DCLI_CMAKE_HOST_VER:STRING=${PRODUCT_VERSION}"
        "-DCLI_CMAKE_COMMON_HOST_VER:STRING=${PRODUCT_VERSION}"
        "-DCLI_CMAKE_PKG_RID:STRING=${BASE_RID}"
        "-DCLI_CMAKE_COMMIT_HASH:STRING=${COMMIT_HASH}"
        "-DCLI_CMAKE_PLATFORM_ARCH_${ARCH_NAME}=1"
        "-DCMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION=10.0"
)

vcpkg_install_cmake()

vcpkg_copy_pdbs()

file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/include)

file(INSTALL ${SOURCE_PATH}/LICENSE.TXT DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)
file(INSTALL ${CMAKE_CURRENT_LIST_DIR}/usage DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT})

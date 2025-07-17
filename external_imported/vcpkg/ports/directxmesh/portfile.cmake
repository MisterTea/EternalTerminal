set(DIRECTXMESH_TAG dec2022)

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Microsoft/DirectXMesh
    REF dec2022b
    SHA512 8e7b757b311223b6ea10fcd6ca9729a4fed51526966239da7d9c3c22f09bc50bbae80b2f8f899badbcc0bfcabc62dcd8d7a662c5881118cce97d3198c014c51e
    HEAD_REF main
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        dx12 BUILD_DX12
        spectre ENABLE_SPECTRE_MITIGATION
)

if (VCPKG_HOST_IS_LINUX)
    message(WARNING "Build ${PORT} requires GCC version 9 or later")
endif()

set(EXTRA_OPTIONS -DBUILD_TESTING=OFF)

if(VCPKG_TARGET_IS_UWP OR VCPKG_TARGET_IS_XBOX)
  list(APPEND EXTRA_OPTIONS -DBUILD_TOOLS=OFF)
else()
  list(APPEND EXTRA_OPTIONS -DBUILD_TOOLS=ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS ${FEATURE_OPTIONS} ${EXTRA_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH share/directxmesh)

if((VCPKG_HOST_IS_WINDOWS) AND (VCPKG_TARGET_ARCHITECTURE MATCHES x64))
  vcpkg_download_distfile(
    MESHCONVERT_EXE
    URLS "https://github.com/Microsoft/DirectXMesh/releases/download/${DIRECTXMESH_TAG}/meshconvert.exe"
    FILENAME "meshconvert-${DIRECTXMESH_TAG}.exe"
    SHA512 46b5fc3dcf58a7c03075927511de5ae4c62c09ceb22076125d3be29044d7da1cc32225a43500ed53ddf0c30d969091b705345a5eb3bb49cc07233dba988357c8
  )

  file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/directxmesh/")

  file(INSTALL
    ${MESHCONVERT_EXE}
    DESTINATION ${CURRENT_PACKAGES_DIR}/tools/directxmesh/)

  file(RENAME ${CURRENT_PACKAGES_DIR}/tools/directxmesh/meshconvert-${DIRECTXMESH_TAG}.exe ${CURRENT_PACKAGES_DIR}/tools/directxmesh/meshconvert.exe)

elseif(VCPKG_TARGET_IS_WINDOWS AND (NOT VCPKG_TARGET_IS_UWP) AND (NOT VCPKG_TARGET_IS_XBOX))

  vcpkg_copy_tools(
        TOOL_NAMES meshconvert
        SEARCH_DIR ${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/bin/CMake
    )

endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)

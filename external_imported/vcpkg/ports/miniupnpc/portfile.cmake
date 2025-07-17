vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO miniupnp/miniupnp
    REF miniupnpc_2_1
    SHA512 f2ab5116c094982f7838ccab460d3db07a99de1094448277fc45841e0e64ea1b4216d75a7e5dd471c79ff9b0132b89e4d801c3ad1b60d55631c12c916df658f5
    HEAD_REF master
    PATCHES
        cmakelists.diff
)

string(COMPARE EQUAL ${VCPKG_LIBRARY_LINKAGE} "dynamic" MINIUPNPC_BUILD_SHARED)
string(COMPARE EQUAL ${VCPKG_LIBRARY_LINKAGE} "static" MINIUPNPC_BUILD_STATIC)

vcpkg_configure_cmake(
    SOURCE_PATH ${SOURCE_PATH}/miniupnpc
    PREFER_NINJA # Disable this option if project cannot be built with Ninja
    OPTIONS
    -DUPNPC_BUILD_STATIC=${MINIUPNPC_BUILD_STATIC}
    -DUPNPC_BUILD_SHARED=${MINIUPNPC_BUILD_SHARED}
    -DUPNPC_BUILD_TESTS=OFF
    -DUPNPC_BUILD_SAMPLE=OFF
)

vcpkg_install_cmake()

file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/include)
file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/miniupnpc RENAME copyright)

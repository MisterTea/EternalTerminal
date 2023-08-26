vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/libphonenumber
    REF "v${VERSION}"
    SHA512 401d6bdfe603ffd994ebb76c8b073e0f0d135390bee72fe4783b5fd9b684e2531af154906f49bf7803d3e720b2d0ccc00fc0ea6fbbd2729556a488c5f5647bee
    HEAD_REF master
    PATCHES 
        "fix-re2-identifiers.patch"
        "fix-icui18n-lib-name.patch"
        fix-find-protobuf.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/cpp"
    OPTIONS
        -DREGENERATE_METADATA=OFF
        -DUSE_RE2=ON
        -DBUILD_GEOCODER=OFF
        -DUSE_PROTOBUF_LITE=ON
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_TESTING=OFF)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

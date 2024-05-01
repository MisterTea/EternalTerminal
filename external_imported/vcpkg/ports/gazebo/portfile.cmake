vcpkg_download_distfile(gazebo3211
    URLS "https://patch-diff.githubusercontent.com/raw/osrf/gazebo/pull/3211.diff?full_index=1"
    FILENAME "gazebo3211.diff"
    SHA512 761e254866d4705acc0b81479285f979c436b3b611739a207a575031d8a8daba48de4fc0c8de5edb9a9f89725586c5caeef9e6e1e3d63a2d961ca09df974f7de
)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO osrf/gazebo
    REF 382dcc3f36095a8d79b5bc9c8b8ad346e867c51d
    SHA512 57638cd0b23b5f2bfd32fdc159d6cd77ca34e3bd695c225591979aef4b7271eac93d3706fa1ffa2340f90013267a4171bebe1e4c142f19ad2bf67963dfed627e
    HEAD_REF gazebo11
    PATCHES
        0001-Fix-deps.patch
        "${gazebo3211}"
        fix-tbb.patch # Effectively https://github.com/osrf/gazebo/pull/3174 with merge conflict resolutions
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        openal    HAVE_OPENAL
        ffmpeg    FFMPEG_FEATURE
        gts       GTS_FEATURE
    INVERTED_FEATURES
        simbody   CMAKE_DISABLE_FIND_PACKAGE_Simbody
        dart      CMAKE_DISABLE_FIND_PACKAGE_DART
        bullet    CMAKE_DISABLE_FIND_PACKAGE_BULLET
        libusb    NO_LIBUSB_FEATURE
        gdal      NO_GDAL_FEATURE
        graphviz  NO_GRAPHVIZ_FEATURE
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DUSE_EXTERNAL_TINY_PROCESS_LIBRARY=ON
        "-DPKG_CONFIG_EXECUTABLE=${CURRENT_HOST_INSTALLED_DIR}/tools/pkgconf/pkgconf${VCPKG_HOST_EXECUTABLE_SUFFIX}"
        ${FEATURE_OPTIONS}
        -DBUILD_TESTING=OFF  # Not enabled by default, but to be sure
)

vcpkg_cmake_install(ADD_BIN_TO_PATH)
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/gazebo")
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/gazebo-11/gazebo/test")

foreach(postfix "" "-11")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/${PORT}${postfix}/setup.sh" "${CURRENT_PACKAGES_DIR}" "`dirname $0`/../..")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/${PORT}${postfix}/setup.sh" "${CURRENT_INSTALLED_DIR}" "`dirname $0`/../..")
endforeach()

vcpkg_copy_tools(
    TOOL_NAMES gazebo gz gzclient gzserver
    AUTO_CLEAN
)
set(EXTRA_OGRE_LIBS Codec_EXR Codec_FreeImage Codec_STBI OgreBites OgreMain OgreMeshLodGenerator OgreOverlay OgrePaging OgreProperty OgreRTShaderSystem OgreTerrain OgreVolume Plugin_BSPSceneManager Plugin_DotScene Plugin_OctreeSceneManager Plugin_OctreeZone Plugin_ParticleFX Plugin_PCZSceneManager RenderSystem_Direct3D11 RenderSystem_GL RenderSystem_GL3Plus)
foreach(LIB IN LISTS EXTRA_OGRE_LIBS)
    set(FILE_NAME "${CMAKE_SHARED_LIBRARY_PREFIX}${LIB}${CMAKE_SHARED_LIBRARY_SUFFIX}")
    file(COPY "${CURRENT_INSTALLED_DIR}/bin/${FILE_NAME}" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
endforeach()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_fixup_pkgconfig()
# Handle copyright
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

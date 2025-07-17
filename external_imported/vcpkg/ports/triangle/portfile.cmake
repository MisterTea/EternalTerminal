vcpkg_fail_port_install(ON_TARGET "uwp")

vcpkg_download_distfile(ARCHIVE_FILE
    URLS "http://www.netlib.org/voronoi/triangle.zip"
    FILENAME "triangle.zip"
    SHA512 c9c1ac527c4bf836ed877b1c5495abf9fd2c453741f4c9698777e23cde939ebf0dd73c84cec64f35a93ca01bff4b86ce32ec559da33e570a0744a764e46d2186
)

vcpkg_extract_source_archive_ex(
    OUT_SOURCE_PATH SOURCE_PATH
    NO_REMOVE_ONE_LEVEL
    ARCHIVE ${ARCHIVE_FILE}
    PATCHES
        "enable_64bit_architecture.patch"
)

file(COPY ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt DESTINATION ${SOURCE_PATH})
file(COPY ${CMAKE_CURRENT_LIST_DIR}/exports.def DESTINATION ${SOURCE_PATH})

vcpkg_configure_cmake(
    SOURCE_PATH ${SOURCE_PATH}
    PREFER_NINJA
)

vcpkg_install_cmake()

vcpkg_copy_pdbs()

vcpkg_fixup_cmake_targets()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/tools")

file(INSTALL ${SOURCE_PATH}/README DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

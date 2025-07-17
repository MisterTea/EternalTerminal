vcpkg_download_distfile(ARCHIVE
    URLS "http://www.colm.net/files/ragel/ragel-6.10.tar.gz"
    FILENAME "ragel-6.10.tar.gz"
    SHA512 6c1fe4f6fa8546ae28b92ccfbae94355ff0d3cea346b9ae8ce4cf6c2bdbeb823e0ccd355332643ea72d3befd533a8b3030ddbf82be7ffa811c2c58cbb01aaa38
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    PATCHES
        0001-remove-unistd-h.patch
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/config.h" DESTINATION "${SOURCE_PATH}/ragel")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

# Allow empty include directory
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

# Handle copyright
file(INSTALL "${SOURCE_PATH}/COPYING" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

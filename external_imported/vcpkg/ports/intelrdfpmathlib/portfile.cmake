vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_download_distfile(ARCHIVE
    URLS "http://www.netlib.org/misc/intel/IntelRDFPMathLib20U2.tar.gz"
    FILENAME "IntelRDFPMathLib20U2.tar.gz"
    SHA512 4d445855f41b066b784f0c6b4e52f854df4129fa9d43569b1e1518f002b860f69796459c78de46a8ea24fb6e7aefe7f8bc1f253e78971a5ef202dab2a7b1b75a
)

vcpkg_extract_source_archive_ex(
    ARCHIVE ${ARCHIVE}
    OUT_SOURCE_PATH SOURCE_PATH
)

set(LIB_SOURCE_PATH "${SOURCE_PATH}/LIBRARY")

file(COPY ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt DESTINATION "${LIB_SOURCE_PATH}")

vcpkg_configure_cmake(
    SOURCE_PATH "${LIB_SOURCE_PATH}"
    PREFER_NINJA
    OPTIONS_DEBUG
    -DDISABLE_INSTALL_HEADERS=ON
)

vcpkg_install_cmake()

# Handle copyright
file(INSTALL ${SOURCE_PATH}/eula.txt DESTINATION ${CURRENT_PACKAGES_DIR}/share/IntelRDFPMathLib RENAME copyright)

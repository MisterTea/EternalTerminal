vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO fribidi/fribidi
    REF v1.0.11
    SHA512 9ddb0dbfb28ee4f1d9dd5d46fe1fff75c5446bfdacaabf8647847c36ef72eec823f6e6d6d1369d67c292d680004f7113c9e69114a8fae1d73ab11a58c66a8b8a
    HEAD_REF master
)

vcpkg_configure_make(
    SOURCE_PATH ${SOURCE_PATH}
)

vcpkg_install_make(DISABLE_PARALLEL)
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

# Define static macro
file(READ ${CURRENT_PACKAGES_DIR}/include/fribidi/fribidi-common.h FRIBIDI_COMMON_H)
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    string(REPLACE "#ifndef FRIBIDI_LIB_STATIC" "#if 0" FRIBIDI_COMMON_H "${FRIBIDI_COMMON_H}")
else()
    string(REPLACE "#ifndef FRIBIDI_LIB_STATIC" "#if 1" FRIBIDI_COMMON_H "${FRIBIDI_COMMON_H}")
endif()
file(WRITE ${CURRENT_PACKAGES_DIR}/include/fribidi/fribidi-common.h "${FRIBIDI_COMMON_H}")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

# Handle copyright
file(INSTALL ${SOURCE_PATH}/COPYING DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)

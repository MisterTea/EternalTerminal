if (VCPKG_CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    message(FATAL_ERROR "leveldb doesn't supports UWP")
endif()

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO "google/leveldb"
    REF 1.22
    SHA512  f9bbf5f466e7f707b94e19261762319ea9f65d41911690e84f59098551e2e69beccf756a414d705ade74ee96fd979bdb8b94c171c6f2cc83873cbd4a9380dbab
    HEAD_REF master
    PATCHES
        fix_config.patch
)

file(COPY ${CURRENT_PORT_DIR}/leveldbConfig.cmake.in DESTINATION ${SOURCE_PATH}/cmake)

vcpkg_configure_cmake(
    SOURCE_PATH ${SOURCE_PATH}
    PREFER_NINJA # Disable this option if project cannot be built with Ninja
    OPTIONS_DEBUG -DINSTALL_HEADERS=OFF
)

vcpkg_install_cmake()

vcpkg_fixup_cmake_targets(CONFIG_PATH lib/cmake/leveldb)

file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/include)
file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/share)

# Handle copyright
file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/leveldb RENAME copyright)

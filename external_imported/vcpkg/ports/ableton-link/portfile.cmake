#header-only library
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Ableton/link
    REF Link-3.0.6
    SHA512 7dd811d3b7792722a8754cd0875777b8cf4902a0babff2822a6fd997137eb5feac576263169c71fca24358189e56b5106a32ae1313b33fb6148eb845691a6438
    HEAD_REF master
    PATCHES
        replace_local_asiostandalone_by_vcpkg_asio.patch
        replace_asiosdk_download_by_vcpkg_asiosdk.patch
        replace_local_catch_by_vcpkg_catch2.patch
        no-werror.patch
)
# Note that the dependencies ASIO and ASIOSDK are completely different things:
# -ASIO (ASyncronous IO) is a cross-platform C++ library for network and low-level I/O programming
# -ASIOSDK is the SDK for the Steinberg ASIO (Audio Stream Input/Output) driver, for profesional Windows audio applications

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
FEATURES
    "discoverytest"           LinkDiscoveryTest
    "coretest"                LinkCoreTest
    "hut"                     LinkHut
    "hutsilent"               LinkHutSilent
)

file(REMOVE_RECURSE "${SOURCE_PATH}/ci")
file(REMOVE_RECURSE "${SOURCE_PATH}/modules")
file(REMOVE_RECURSE "${SOURCE_PATH}/third_party")

set(NEED_CATCH2 OFF)
if ("coretest" IN_LIST FEATURES)
    set(NEED_CATCH2 ON)
endif()
if ("discoverytest" IN_LIST FEATURES)
    set(NEED_CATCH2 ON)
endif()

set(NEED_ASIOSDK OFF)
if ("hut" IN_LIST FEATURES)
  if(WIN32)
    # Need Steinberg ASIO audio driver SDK (only this low-latency audio driver makes the developer tool 'hut' useful on Windows)
    set(NEED_ASIOSDK ON)
  endif()
endif()
    
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS    
        -DNEED_CATCH2=${NEED_CATCH2}
        -DLINK_BUILD_ASIO=${NEED_ASIOSDK}
)

if ("coretest" IN_LIST FEATURES)
    vcpkg_cmake_build(TARGET LinkCoreTest)
    file(INSTALL "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-dbg/bin/LinkCoreTest${VCPKG_TARGET_EXECUTABLE_SUFFIX}" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
endif()
if ("discoverytest" IN_LIST FEATURES)
    vcpkg_cmake_build(TARGET LinkDiscoveryTest)
    file(INSTALL "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-dbg/bin/LinkDiscoveryTest${VCPKG_TARGET_EXECUTABLE_SUFFIX}" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
endif()
if ("hut" IN_LIST FEATURES)
    vcpkg_cmake_build(TARGET LinkHut)
    file(INSTALL "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-dbg/bin/LinkHut${VCPKG_TARGET_EXECUTABLE_SUFFIX}" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
endif()
if ("hutsilent" IN_LIST FEATURES)
    vcpkg_cmake_build(TARGET LinkHutSilent)
    file(INSTALL "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-dbg/bin/LinkHutSilent${VCPKG_TARGET_EXECUTABLE_SUFFIX}" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
endif()

# We must not correct the CMake include path before build
vcpkg_apply_patches(
    SOURCE_PATH "${SOURCE_PATH}"
    PATCHES 
        correct_cmake_include_directory.patch
)

file(INSTALL "${SOURCE_PATH}/AbletonLinkConfig.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/")
file(INSTALL "${SOURCE_PATH}/cmake_include/" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/cmake_include/")
file(INSTALL "${SOURCE_PATH}/include/" DESTINATION "${CURRENT_PACKAGES_DIR}/include" PATTERN "CMakeLists.txt" EXCLUDE)

# Handle copyright
file(INSTALL "${SOURCE_PATH}/LICENSE.md" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

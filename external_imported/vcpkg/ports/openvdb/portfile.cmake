vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO AcademySoftwareFoundation/openvdb
    REF 89873d2bd29870cc9f176ed12b3f3a930ca38d1a # v8.2.0
    SHA512 d72a0eec6b7ce8e25fbe3100d4a291c35d7c2448f23131aaa9f247210f26e965198ef2991d9cf789afc754cbcddaace7a27ab62a2609a19a896034859c518699
    PATCHES
        0003-fix-cmake.patch
)

file(REMOVE "${SOURCE_PATH}/cmake/FindTBB.cmake")
file(REMOVE "${SOURCE_PATH}/cmake/FindIlmBase.cmake")
file(REMOVE "${SOURCE_PATH}/cmake/FindBlosc.cmake")
file(REMOVE "${SOURCE_PATH}/cmake/FindOpenEXR.cmake")

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" OPENVDB_STATIC)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" OPENVDB_SHARED)

set(OPENVDB_BUILD_TOOLS OFF)
if ("tools" IN_LIST FEATURES)
  if (VCPKG_LIBRARY_LINKAGE STREQUAL dynamic)
    set(OPENVDB_BUILD_TOOLS ON)
  else()
    message(FATAL_ERROR "Unable to build tools if static libraries are required")
  endif()
endif()

if ("ax" IN_LIST FEATURES)
  if(NOT VCPKG_TARGET_IS_WINDOWS)
    set(OPENVDB_BUILD_AX ON)
  else()
    message(FATAL_ERROR "Currently no support for building OpenVDB AX on Windows.")  
  endif()
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DOPENVDB_BUILD_UNITTESTS=OFF
        -DOPENVDB_BUILD_PYTHON_MODULE=OFF
        -DOPENVDB_ENABLE_3_ABI_COMPATIBLE=OFF
        -DUSE_GLFW3=ON
        -DOPENVDB_CORE_STATIC=${OPENVDB_STATIC}
        -DOPENVDB_CORE_SHARED=${OPENVDB_SHARED}
        -DOPENVDB_BUILD_VDB_PRINT=${OPENVDB_BUILD_TOOLS}
        -DOPENVDB_BUILD_VDB_VIEW=${OPENVDB_BUILD_TOOLS}
        -DOPENVDB_BUILD_VDB_RENDER=${OPENVDB_BUILD_TOOLS}
        -DOPENVDB_BUILD_VDB_LOD=${OPENVDB_BUILD_TOOLS}
        -DUSE_PKGCONFIG=OFF
        ${OPENVDB_BUILD_AX}
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/OpenVDB)

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

if (OPENVDB_BUILD_TOOLS)
    vcpkg_copy_tools(TOOL_NAMES vdb_print vdb_render vdb_view vdb_lod AUTO_CLEAN)
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper.cmake.in" "${CURRENT_PACKAGES_DIR}/share/${PORT}/vcpkg-cmake-wrapper.cmake" @ONLY)
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${SOURCE_PATH}/openvdb/openvdb/COPYRIGHT" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

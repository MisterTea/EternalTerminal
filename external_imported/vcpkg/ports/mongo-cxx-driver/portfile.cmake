vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO mongodb/mongo-cxx-driver
    REF "r${VERSION}"
    SHA512 34ff303d496dd2c9b8cada16dc215c40fddccfe660bdc7fe59c92449861876b820c3ea4e3e5c91029e0322411bbe98a11cb1f3fa046b028d92d3c9a3509ce988
    HEAD_REF master
    PATCHES
        fix-dependencies.patch
)
file(WRITE "${SOURCE_PATH}/build/VERSION_CURRENT" "${VERSION}")

# This port offered C++17 ABI alternative via features.
# This was reduced to boost only.
vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        boost   BSONCXX_POLY_USE_BOOST
    INVERTED_FEATURES
        boost   BSONCXX_POLY_USE_STD
        boost   CMAKE_DISABLE_FIND_PACKAGE_Boost
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        "-DCMAKE_PROJECT_MONGO_CXX_DRIVER_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/cmake-project-include.cmake"
        -DBSONCXX_HEADER_INSTALL_DIR=include
        -DENABLE_TESTS=OFF
        -DENABLE_UNINSTALL=OFF
        -DMONGOCXX_HEADER_INSTALL_DIR=include
    MAYBE_UNUSED_VARIABLES
        CMAKE_DISABLE_FIND_PACKAGE_Boost
        BSONCXX_HEADER_INSTALL_DIR
        MONGOCXX_HEADER_INSTALL_DIR
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

vcpkg_cmake_config_fixup(PACKAGE_NAME "bsoncxx" CONFIG_PATH "lib/cmake/bsoncxx-${VERSION}" DO_NOT_DELETE_PARENT_CONFIG_PATH)
vcpkg_cmake_config_fixup(PACKAGE_NAME "mongocxx" CONFIG_PATH "lib/cmake/mongocxx-${VERSION}" DO_NOT_DELETE_PARENT_CONFIG_PATH)
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_cmake_config_fixup(PACKAGE_NAME "libbsoncxx-static" CONFIG_PATH "lib/cmake/libbsoncxx-static-${VERSION}" DO_NOT_DELETE_PARENT_CONFIG_PATH)
    file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/libbsoncxx-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/libbsoncxx")
    vcpkg_cmake_config_fixup(PACKAGE_NAME "libmongocxx-static" CONFIG_PATH "lib/cmake/libmongocxx-static-${VERSION}")
    file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/libmongocxx-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/libmongocxx")
else()
    vcpkg_cmake_config_fixup(PACKAGE_NAME "libbsoncxx" CONFIG_PATH "lib/cmake/libbsoncxx-${VERSION}" DO_NOT_DELETE_PARENT_CONFIG_PATH)
    vcpkg_cmake_config_fixup(PACKAGE_NAME "libmongocxx" CONFIG_PATH "lib/cmake/libmongocxx-${VERSION}")
endif()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

function(auto_clean dir)
    file(GLOB entries "${dir}/*")
    file(GLOB files LIST_DIRECTORIES false "${dir}/*")
    foreach(entry IN LISTS entries)
        if(entry IN_LIST files)
            continue()
        endif()
        file(GLOB_RECURSE children "${entry}/*")
        if(children)
            auto_clean("${entry}")
        else()
            file(REMOVE_RECURSE "${entry}")
        endif()
    endforeach()
endfunction()
auto_clean("${CURRENT_PACKAGES_DIR}/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

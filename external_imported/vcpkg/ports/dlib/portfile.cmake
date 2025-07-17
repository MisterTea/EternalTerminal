vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO davisking/dlib
    REF 70ea028f12e10f4d992a0c4f0169749eae5bb185 #v19.22
    SHA512 f7c5988d6a1b2384a54a3e1a330c50011d3325bb87b4edd9fa08a2f4daeffdcb276ea21bc1d4cfd766b4e4d0a9c13b235e4b6002aa97af5327b833f91b688fd0
    HEAD_REF master
    PATCHES
        fix-sqlite3-fftw-linkage.patch
        force_finding_packages.patch
        find_blas.patch
)

file(REMOVE_RECURSE "${SOURCE_PATH}/dlib/external/libjpeg")
file(REMOVE_RECURSE "${SOURCE_PATH}/dlib/external/libpng")
file(REMOVE_RECURSE "${SOURCE_PATH}/dlib/external/zlib")

# This fixes static builds; dlib doesn't pull in the needed transitive dependencies
file(READ "${SOURCE_PATH}/dlib/CMakeLists.txt" DLIB_CMAKE)
string(REPLACE "PNG_LIBRARY" "PNG_LIBRARIES" DLIB_CMAKE "${DLIB_CMAKE}")
file(WRITE "${SOURCE_PATH}/dlib/CMakeLists.txt" "${DLIB_CMAKE}")

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        "sqlite3"   DLIB_LINK_WITH_SQLITE3
        "fftw3"     DLIB_USE_FFTW
        "cuda"      DLIB_USE_CUDA
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DDLIB_PNG_SUPPORT=ON
        -DDLIB_JPEG_SUPPORT=ON
        -DDLIB_USE_BLAS=ON
        -DDLIB_USE_LAPACK=ON
        -DDLIB_GIF_SUPPORT=OFF
        -DDLIB_USE_MKL_FFT=OFF
        -DCMAKE_DEBUG_POSTFIX=d
    OPTIONS_DEBUG
        -DDLIB_ENABLE_ASSERTS=ON
        #-DDLIB_ENABLE_STACK_TRACE=ON
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/dlib)

vcpkg_fixup_pkgconfig()

# There is no way to suppress installation of the headers and resource files in debug build.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

# Remove other files not required in package
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/all")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/appveyor")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/test")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/travis")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/cmake_utils/test_for_neon")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/cmake_utils/test_for_cudnn")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/cmake_utils/test_for_cuda")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/cmake_utils/test_for_cpp11")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/cmake_utils/test_for_avx")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/cmake_utils/test_for_sse4")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/cmake_utils/test_for_libjpeg")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/cmake_utils/test_for_libpng")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/dlib/external/libpng/arm")

# Dlib encodes debug/release in its config.h. Patch it to respond to the NDEBUG macro instead.
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/dlib/config.h" "/* #undef ENABLE_ASSERTS */" "#if defined(_DEBUG)\n#define ENABLE_ASSERTS\n#endif")
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/dlib/config.h" "#define DLIB_DISABLE_ASSERTS" "#if !defined(_DEBUG)\n#define DLIB_DISABLE_ASSERTS\n#endif")

# Handle copyright
file(INSTALL "${SOURCE_PATH}/dlib/LICENSE.txt" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/doc")

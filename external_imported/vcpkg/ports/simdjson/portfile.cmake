vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO simdjson/simdjson
    REF 933c2ebeacf9a1df12acd9a6781e590cad2f81f7 # v2.2.2
    HEAD_REF master
    SHA512 6dda3fbbba7e788adc7b29710c2b08c428a180fadd298c4129a5856e65de1d947b38ecff0098fcd29ad98923e14910ba77eaedbd3d3a9fa3a0c49c686ecbb1b4
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        exceptions SIMDJSON_EXCEPTIONS
        threads    SIMDJSON_ENABLE_THREADS
    INVERTED_FEATURES
        deprecated SIMDJSON_DISABLE_DEPRECATED_API
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" SIMDJSON_BUILD_STATIC)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DSIMDJSON_JUST_LIBRARY=ON
        -DSIMDJSON_SANITIZE_UNDEFINED=OFF
        -DSIMDJSON_SANITIZE=OFF
        -DSIMDJSON_SANITIZE_THREADS=OFF
        -DSIMDJSON_BUILD_STATIC=${SIMDJSON_BUILD_STATIC}
        -DSIMDJSON_DEVELOPMENT_CHECKS=OFF
        -DSIMDJSON_VERBOSE_LOGGING=OFF
        ${FEATURE_OPTIONS}
)

vcpkg_cmake_install()

vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/${PORT}")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
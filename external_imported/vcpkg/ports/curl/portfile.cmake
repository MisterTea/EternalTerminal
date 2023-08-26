vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO curl/curl
    REF curl-7_88_1
    SHA512 c5caa1f95580ddbf2041c9c4b885f84d4f5c5fcb905a5ea59f9dbb58a98fc292260f95cb935e963bf83d7dcecf98561deef5ce3ff91cdcb29a080559cff0ed64
    HEAD_REF master
    PATCHES
        0002_fix_uwp.patch
        0005_remove_imp_suffix.patch
        0012-fix-dependency-idn2.patch
        0020-fix-pc-file.patch
        0022-deduplicate-libs.patch
        mbedtls-ws2_32.patch
        export-components.patch
        0023-fix-find-cares.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        # Support HTTP2 TLS Download https://curl.haxx.se/ca/cacert.pem rename to curl-ca-bundle.crt, copy it to libcurl.dll location.
        http2       USE_NGHTTP2
        wolfssl     CURL_USE_WOLFSSL
        openssl     CURL_USE_OPENSSL
        mbedtls     CURL_USE_MBEDTLS
        ssh         CURL_USE_LIBSSH2
        tool        BUILD_CURL_EXE
        c-ares      ENABLE_ARES
        sspi        CURL_WINDOWS_SSPI
        brotli      CURL_BROTLI
        schannel    CURL_USE_SCHANNEL
        sectransp   CURL_USE_SECTRANSP
        idn2        USE_LIBIDN2
        winidn      USE_WIN32_IDN
        winldap     USE_WIN32_LDAP
        websockets  ENABLE_WEBSOCKETS
    INVERTED_FEATURES
        non-http    HTTP_ONLY
        winldap     CURL_DISABLE_LDAP # Only WinLDAP support ATM
)

set(OPTIONS "")
if("idn2" IN_LIST FEATURES)
    vcpkg_find_acquire_program(PKGCONFIG)
    list(APPEND OPTIONS "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}")
endif()

if("sectransp" IN_LIST FEATURES)
    list(APPEND OPTIONS -DCURL_CA_PATH=none -DCURL_CA_BUNDLE=none)
endif()

# UWP targets
if(VCPKG_TARGET_IS_UWP)
    list(APPEND OPTIONS
        -DCURL_DISABLE_TELNET=ON
        -DENABLE_IPV6=OFF
        -DENABLE_UNIX_SOCKETS=OFF
    )
endif()

if(VCPKG_TARGET_IS_WINDOWS)
    list(APPEND OPTIONS -DENABLE_UNICODE=ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS 
        "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/cmake-project-include.cmake"
        ${FEATURE_OPTIONS}
        ${OPTIONS}
        -DBUILD_TESTING=OFF
        -DENABLE_MANUAL=OFF
        -DCURL_CA_FALLBACK=ON
        -DCURL_USE_LIBPSL=OFF
    OPTIONS_DEBUG
        -DENABLE_DEBUG=ON
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()

if ("tool" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES curl AUTO_CLEAN)
endif()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/CURL)

vcpkg_fixup_pkgconfig()
set(namespec "curl")
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    set(namespec "libcurl")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libcurl.pc" " -lcurl" " -l${namespec}")
endif()
if(NOT DEFINED VCPKG_BUILD_TYPE)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libcurl.pc" " -lcurl" " -l${namespec}-d")
endif()

#Fix install path
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/bin/curl-config" "${CURRENT_PACKAGES_DIR}" "\${prefix}")
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/bin/curl-config" "${CURRENT_INSTALLED_DIR}" "\${prefix}")
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/bin/curl-config" "\nprefix=\${prefix}" [=[prefix=$(CDPATH= cd -- "$(dirname -- "$0")"/../../.. && pwd -P)]=])
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin")
file(RENAME "${CURRENT_PACKAGES_DIR}/bin/curl-config" "${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin/curl-config")
if(EXISTS "${CURRENT_PACKAGES_DIR}/debug/bin/curl-config")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "${CURRENT_PACKAGES_DIR}" "\${prefix}")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "${CURRENT_INSTALLED_DIR}" "\${prefix}")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "\nprefix=\${prefix}/debug" [=[prefix=$(CDPATH= cd -- "$(dirname -- "$0")"/../../../.. && pwd -P)]=])
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "-lcurl" "-l${namespec}-d")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "curl." "curl-d.")
    file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/${PORT}/debug/bin")
    file(RENAME "${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "${CURRENT_PACKAGES_DIR}/tools/${PORT}/debug/bin/curl-config")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static" OR NOT VCPKG_TARGET_IS_WINDOWS)
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/curl/curl.h"
        "#ifdef CURL_STATICLIB"
        "#if 1"
    )
endif()

file(INSTALL "${CURRENT_PORT_DIR}/vcpkg-cmake-wrapper.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")

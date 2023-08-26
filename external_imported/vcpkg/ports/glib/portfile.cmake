string(REGEX MATCH "^([0-9]*[.][0-9]*)" GLIB_MAJOR_MINOR "${VERSION}")
vcpkg_download_distfile(GLIB_ARCHIVE
    URLS "https://download.gnome.org/sources/glib/${GLIB_MAJOR_MINOR}/glib-${VERSION}.tar.xz"
    FILENAME "glib-${VERSION}.tar.xz"
    SHA512 44aa53d0dae69e50c8d5d9adb7c90034c4c2ab5db3c74106c91bfdbdca41bc5523b32b16c33bc65b3c6bf0037bdecb344b80e9708930bde54ac7c618c34ea5a8
)

vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${GLIB_ARCHIVE}"
    PATCHES
        use-libiconv-on-windows.patch
        libintl.patch
)

vcpkg_list(SET OPTIONS)
if (selinux IN_LIST FEATURES)
    if(NOT EXISTS "/usr/include/selinux")
        message(WARNING "SELinux was not found in its typical system location. Your build may fail. You can install SELinux with \"apt-get install selinux libselinux1-dev\".")
    endif()
    list(APPEND OPTIONS -Dselinux=enabled)
else()
    list(APPEND OPTIONS -Dselinux=disabled)
endif()

if (libmount IN_LIST FEATURES)
    list(APPEND OPTIONS -Dlibmount=enabled)
else()
    list(APPEND OPTIONS -Dlibmount=disabled)
endif()

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${OPTIONS}
        -Dgtk_doc=false
        -Dinstalled_tests=false
        -Dlibelf=disabled
        -Dman=false
        -Dtests=false
        -Dxattr=false
)
vcpkg_install_meson(ADD_BIN_TO_PATH)
vcpkg_copy_pdbs()

file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
set(GLIB_SCRIPTS
    gdbus-codegen
    glib-genmarshal
    glib-gettextize
    glib-mkenums
    gtester-report
)
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    list(REMOVE_ITEM GLIB_SCRIPTS glib-gettextize)
endif()
foreach(script IN LISTS GLIB_SCRIPTS)
    file(RENAME "${CURRENT_PACKAGES_DIR}/bin/${script}" "${CURRENT_PACKAGES_DIR}/tools/${PORT}/${script}")
    file(REMOVE "${CURRENT_PACKAGES_DIR}/debug/bin/${script}")
endforeach()

set(GLIB_TOOLS
    gapplication
    gdbus
    gio
    gio-querymodules
    glib-compile-resources
    glib-compile-schemas
    gobject-query
    gresource
    gsettings
    gtester
)
if(VCPKG_TARGET_IS_WINDOWS)
    list(REMOVE_ITEM GLIB_TOOLS gapplication gtester)
    if(VCPKG_TARGET_ARCHITECTURE MATCHES "x64|arm64")
        list(APPEND GLIB_TOOLS gspawn-win64-helper gspawn-win64-helper-console)
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
        list(APPEND GLIB_TOOLS gspawn-win32-helper gspawn-win32-helper-console)
    endif()
elseif(VCPKG_TARGET_IS_OSX)
    list(REMOVE_ITEM GLIB_TOOLS gapplication)
endif()
vcpkg_copy_tools(TOOL_NAMES ${GLIB_TOOLS} AUTO_CLEAN)

vcpkg_fixup_pkgconfig()

if(VCPKG_TARGET_IS_WINDOWS)
    set(LIBINTL_NAME "intl.lib")
else()
    set(LIBINTL_NAME "libintl")
    if(VCPKG_LIBRARY_LINKAGE STREQUAL dynamic)
        string(APPEND LIBINTL_NAME "${VCPKG_TARGET_SHARED_LIBRARY_SUFFIX}")
    else()
        string(APPEND LIBINTL_NAME "${VCPKG_TARGET_STATIC_LIBRARY_SUFFIX}")
    endif()
endif()

set(pc_replace_intl_path gio glib gmodule-no-export gobject gthread)
foreach(pc_prefix IN LISTS pc_replace_intl_path)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/${pc_prefix}-2.0.pc" "\"" "")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/${pc_prefix}-2.0.pc" "\${prefix}/debug/lib/${LIBINTL_NAME}" "-lintl")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/${pc_prefix}-2.0.pc" "\${prefix}/lib/${LIBINTL_NAME}" "-lintl")
    if(NOT VCPKG_BUILD_TYPE)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/${pc_prefix}-2.0.pc" "\"" "")
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/${pc_prefix}-2.0.pc" "\${prefix}/lib/${LIBINTL_NAME}" "-lintl")
    endif()
endforeach()

vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/gio-2.0.pc" "\${bindir}" "\${prefix}/tools/${PORT}")
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/glib-2.0.pc" "\${bindir}" "\${prefix}/tools/${PORT}")
if(NOT VCPKG_BUILD_TYPE)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/gio-2.0.pc" "\${bindir}" "\${prefix}/../tools/${PORT}")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/glib-2.0.pc" "\${bindir}" "\${prefix}/../tools/${PORT}")
endif()

# Fix python scripts
set(_file "${CURRENT_PACKAGES_DIR}/tools/${PORT}/gdbus-codegen")
file(READ "${_file}" _contents)
string(REPLACE "elif os.path.basename(filedir) == 'bin':" "elif os.path.basename(filedir) == 'tools':" _contents "${_contents}")
string(REPLACE "path = os.path.join(filedir, '..', 'share', 'glib-2.0')" "path = os.path.join(filedir, '../..', 'share', 'glib-2.0')" _contents "${_contents}")
string(REPLACE "path = os.path.join(filedir, '..')" "path = os.path.join(filedir, '../../share/glib-2.0')" _contents "${_contents}")
string(REPLACE "path = os.path.join('${CURRENT_PACKAGES_DIR}/share', 'glib-2.0')" "path = os.path.join('unuseable/share', 'glib-2.0')" _contents "${_contents}")
file(WRITE "${_file}" "${_contents}")

if(EXISTS "${CURRENT_PACKAGES_DIR}/tools/${PORT}/glib-gettextize")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/${PORT}/glib-gettextize" "${CURRENT_PACKAGES_DIR}" "`dirname $0`/../..")
endif()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/gdb"
    "${CURRENT_PACKAGES_DIR}/debug/lib/gio"
    "${CURRENT_PACKAGES_DIR}/lib/gio"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSES/LGPL-2.1-or-later.txt")

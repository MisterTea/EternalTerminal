# The upstream v1.8.3 Git tag moved after release. Use the published tarball.
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/libunwind/libunwind/releases/download/v${VERSION}/libunwind-${VERSION}.tar.gz"
    FILENAME "libunwind-${VERSION}.tar.gz"
    SHA512 732c9b74635cf4e65bc90fd5b9a4b90fe5968c7fcce6cc704295038f0d41b848f78a59d725e813072e78df72ff8fc77001096abdfb9ef1c73e3bd82433987679
)
vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    SOURCE_BASE "${VERSION}"
    PATCHES liblzma.diff
)

vcpkg_find_acquire_program(PKGCONFIG)

vcpkg_make_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    AUTORECONF
    OPTIONS
        --disable-documentation
        --disable-tests
        --disable-zlibdebuginfo
        --enable-minidebuginfo
)
vcpkg_make_install()
vcpkg_fixup_pkgconfig()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")

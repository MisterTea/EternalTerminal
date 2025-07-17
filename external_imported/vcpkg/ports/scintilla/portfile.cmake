vcpkg_fail_port_install(ON_TARGET "Linux" "OSX" "UWP")

vcpkg_download_distfile(ARCHIVE
  URLS "https://www.scintilla.org/scintilla446.zip"
  FILENAME "scintilla446.zip"
  SHA512 db6fa38283401497d8331f97dc5b57ea11d998988001f06b95892de769de5829b9f567635f3c1f2d9cfbc4384024d11666d28224ce90c5813ceef865b0dec255
)

if (VCPKG_LIBRARY_LINKAGE STREQUAL "static")
  list(APPEND PATCHES 0001-static-lib.patch)
endif()

if(VCPKG_CRT_LINKAGE STREQUAL "static")
  list(APPEND PATCHES 0002-static-crt.patch)
endif()

vcpkg_extract_source_archive_ex(
  OUT_SOURCE_PATH SOURCE_PATH
  ARCHIVE ${ARCHIVE}
  REF 4.4.6
  PATCHES ${PATCHES}
)

vcpkg_install_msbuild(
  SOURCE_PATH ${SOURCE_PATH}
  PROJECT_SUBPATH Win32/SciLexer.vcxproj
  LICENSE_SUBPATH License.txt
)

file(INSTALL ${SOURCE_PATH}/include/ DESTINATION ${CURRENT_PACKAGES_DIR}/include/${PORT} FILES_MATCHING PATTERN "*.*")

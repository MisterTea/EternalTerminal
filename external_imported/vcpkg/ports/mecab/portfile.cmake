if (VCPKG_TARGET_TRIPLET STREQUAL "x64-uwp" OR VCPKG_TARGET_TRIPLET STREQUAL "arm64-windows" OR VCPKG_TARGET_TRIPLET STREQUAL "arm-uwp")
    message(FATAL_ERROR "mecab does not support on this platform")
endif()

vcpkg_from_github(
	OUT_SOURCE_PATH SOURCE_PATH
	REPO taku910/mecab
	REF 3a07c4eefaffb4e7a0690a7f4e5e0263d3ddb8a3
	SHA512 d3288cca7207daf66df4349819b64fc9cc069c775512cf0607ca855e9e5512509c36b0e2bb0e955478acae13ff0c35df7442f18a8458fab0ed664d62854c0b25
	HEAD_REF master
	PATCHES
		fix_wpath_unsigned.patch
)

file(COPY ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt DESTINATION ${SOURCE_PATH}/mecab/src)
file(COPY ${CMAKE_CURRENT_LIST_DIR}/Config.cmake.in DESTINATION ${SOURCE_PATH}/mecab/src)
file(COPY ${SOURCE_PATH}/mecab/COPYING DESTINATION ${SOURCE_PATH}/mecab/src)

vcpkg_configure_cmake(
    SOURCE_PATH ${SOURCE_PATH}/mecab/src
)

vcpkg_install_cmake()
vcpkg_fixup_cmake_targets()
vcpkg_copy_pdbs()

file(COPY ${SOURCE_PATH}/mecab/COPYING DESTINATION ${CURRENT_PACKAGES_DIR}/share/mecab)
file(RENAME ${CURRENT_PACKAGES_DIR}/share/mecab/COPYING ${CURRENT_PACKAGES_DIR}/share/mecab/copyright)

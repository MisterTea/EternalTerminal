vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO laurikari/tre
    REF 6fb7206b935b35814c5078c20046dbe065435363
    SHA512 f1d664719eab23b665d71e34ca3d11f8ba49da23ff20dc28f46d4ce30fe155c12208ba7fd212dbeb20a7037e069909f0c2120ce1fc01074656399805e3289a90
    HEAD_REF master
    PATCHES
        fix-config.patch
)

if(VCPKG_TARGET_IS_MINGW)
    vcpkg_replace_string("${SOURCE_PATH}/win32/tre.def" "tre.dll" "libtre.dll")
endif()

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/tre" RENAME copyright)

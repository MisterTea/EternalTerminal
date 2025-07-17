vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO FluidSynth/fluidsynth
    REF 8b00644751578ba67b709a827cbe5133d849d339 #v2.2.6
    SHA512 37361c6fdbb54eba78e59f03c9ca702129f0fb522721dfb4e744fdc9a8721e665728fa5606bc68c2fb2ce971b4829cfc472f0a7cd72ce3fe14b3a335b098f7ec
    HEAD_REF master
    PATCHES
        fix-dependencies.patch
        separate-gentables.patch
)

if ("buildtools" IN_LIST FEATURES)
    vcpkg_cmake_configure(
        SOURCE_PATH "${SOURCE_PATH}/src/gentables"
        LOGFILE_BASE configure-tools
    )

    vcpkg_cmake_build(
        LOGFILE_BASE install-tools
        TARGET install
    )

    vcpkg_copy_tools(TOOL_NAMES make_tables AUTO_CLEAN)

    vcpkg_add_to_path(APPEND "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
endif()

set(feature_list dbus jack libinstpatch libsndfile midishare opensles oboe oss sdl2 pulseaudio readline lash alsa systemd coreaudio coremidi dart)
vcpkg_list(SET FEATURE_OPTIONS)
foreach(_feature IN LISTS feature_list)
    list(APPEND FEATURE_OPTIONS -Denable-${_feature}:BOOL=OFF)
endforeach()

vcpkg_add_to_path("${CURRENT_HOST_INSTALLED_DIR}/tools/${PORT}")

vcpkg_find_acquire_program(PKGCONFIG)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}
        -DLIB_INSTALL_DIR=lib
        -Denable-pkgconfig=ON
        -Denable-framework=OFF # Needs system permission to install framework
    OPTIONS_DEBUG
        -Denable-debug:BOOL=ON
    MAYBE_UNUSED_VARIABLES
        enable-coreaudio
        enable-coremidi
        enable-dart
)

vcpkg_cmake_install()
vcpkg_fixup_pkgconfig()

# Copy fluidsynth.exe to tools dir
vcpkg_copy_tools(TOOL_NAMES fluidsynth AUTO_CLEAN)

# Remove unnecessary files
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

if(VCPKG_LIBRARY_LINKAGE STREQUAL static)
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

# Handle copyright
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

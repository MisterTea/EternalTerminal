vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO juce-framework/JUCE
  REF "${VERSION}"
  SHA512 e8519ca4ee55f126e14d7c927e2f36e7c30117a34da84f2b4e730afcea4a864b07455ad3f89d751fa47c8c155548b0c3150a667f1a9d199e582eadd146b6c697
  HEAD_REF master
  PATCHES
  "0001-build-allow-setting-JUCE_PLUGINHOST_LADSPA.patch"
  "0002-build-linux-find_packages.patch"
  "0003-build-forward-vcpkg-toolchain.patch"
  "0004-install-paths.patch"
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
FEATURES
  "extras"      JUCE_BUILD_EXTRAS
  "ladspa"      JUCE_PLUGINHOST_LADSPA
  "jack"        JUCE_JACK
  "curl"        JUCE_USE_CURL
  "freetype"    JUCE_USE_FREETYPE
  "xcursor"     JUCE_USE_XCURSOR
  "xinerama"    JUCE_USE_XINERAMA
  "xrandr"      JUCE_USE_XRANDR
  "xrender"     JUCE_USE_XRENDER
  "web-browser" JUCE_WEB_BROWSER
  "opengl"      JUCE_OPENGL
)
# Based on https://github.com/juce-framework/JUCE/blob/master/docs/Linux%20Dependencies.md
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  message("juce currently requires the following programs from the system package manager:
  libx11-dev libxcomposite-dev libxext-dev
On Ubuntu derivatives:
    sudo apt install libx11-dev libxcomposite-dev libxext-dev
")
  if(JUCE_OPENGL)
    message("juce with opengl feature requires the following packages via the system package manager:
  libglu1-mesa-dev mesa-common-dev
On Ubuntu derivatives:
  sudo apt install libglu1-mesa-dev mesa-common-dev
")
  endif()

  if(${JUCE_PLUGINHOST_LADSPA})
    message("juce with ladspa feature requires the following packages via the system package manager:
  ladspa-sdk
On Ubuntu derivatives:
  sudo apt install ladspa-sdk
")
  endif()

  if(JUCE_USE_XCURSOR)
    message("juce with xcursor feature requires the following packages via the system package manager:
  libxcursor-dev
On Ubuntu derivatives:
  sudo apt install libxcursor-dev
")
  endif()

  if(JUCE_USE_XINERAMA)
    message("juce with xinerama feature requires the following packages via the system package manager:
  libxinerama-dev
On Ubuntu derivatives:
  sudo apt install libxinerama-dev
")
  endif()

  if(JUCE_USE_XRANDR)
    message("juce with xrandr feature requires the following packages via the system package manager:
  libxrandr-dev
On Ubuntu derivatives:
  sudo apt install libxrandr-dev
")
  endif()

  if(JUCE_USE_XRENDER)
    message("juce with xrender feature requires the following packages via the system package manager:
  libxrender-dev
On Ubuntu derivatives:
  sudo apt install libxrender-dev
")
  endif()

  if(JUCE_WEB_BROWSER)
    message("juce with web-browser feature requires the following packages via the system package manager:
  libwebkit2gtk-4.0-dev
On Ubuntu derivatives:
  sudo apt install libwebkit2gtk-4.0-dev
")
  endif()
endif()

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
  -DJUCE_ENABLE_MODULE_SOURCE_GROUPS=ON
  ${FEATURE_OPTIONS}
  MAYBE_UNUSED_VARIABLES
    JUCE_PLUGINHOST_LADSPA
    JUCE_JACK
    JUCE_OPENGL
    JUCE_USE_CURL
    JUCE_USE_FREETYPE
    JUCE_USE_XCURSOR
    JUCE_USE_XINERAMA
    JUCE_USE_XRANDR
    JUCE_USE_XRENDER
    JUCE_WEB_BROWSER
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup()
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

# Copy tools
file(GLOB JUCE_TOOLS "${CURRENT_PACKAGES_DIR}/bin/JUCE-${VERSION}/*")
foreach(JUCE_TOOL_PATH IN LISTS JUCE_TOOLS)
  get_filename_component(JUCE_TOOL "${JUCE_TOOL_PATH}" NAME_WLE)
  get_filename_component(JUCE_TOOL_DIR "${JUCE_TOOL_PATH}" DIRECTORY)
  vcpkg_copy_tools(TOOL_NAMES ${JUCE_TOOL} SEARCH_DIR "${JUCE_TOOL_DIR}")
endforeach()

# Copy extras tools
if(JUCE_BUILD_EXTRAS)
  file(GLOB JUCE_EXTRA_TOOLS "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/extras/*/*_artefacts/Release/*")
  foreach(JUCE_EXTRA_TOOL_PATH IN LISTS JUCE_EXTRA_TOOLS)
    get_filename_component(JUCE_EXTRA_TOOL "${JUCE_EXTRA_TOOL_PATH}" NAME_WLE)
    get_filename_component(JUCE_EXTRA_TOOL_DIR "${JUCE_EXTRA_TOOL_PATH}" DIRECTORY)
    vcpkg_copy_tools(TOOL_NAMES ${JUCE_EXTRA_TOOL} SEARCH_DIR "${JUCE_EXTRA_TOOL_DIR}")
  endforeach()
endif()

# Copy JUCE modules including the cpp/cmake files
file(GLOB JUCE_MODULES_FOLDERS "${CURRENT_PACKAGES_DIR}/include/JUCE-${VERSION}/modules/*")
foreach(JUCE_MODULE_FOLDER IN LISTS JUCE_MODULES_FOLDERS)
  file(COPY "${JUCE_MODULE_FOLDER}" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
endforeach()

# Remove duplicate tools directories
file(REMOVE_RECURSE
"${CURRENT_PACKAGES_DIR}/bin"
"${CURRENT_PACKAGES_DIR}/debug/bin"
)

# Remove duplicate debug files
file(REMOVE_RECURSE
"${CURRENT_PACKAGES_DIR}/debug/"
)

# Copy license
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")

# Copy usage examples
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)
include(${CURRENT_INSTALLED_DIR}/share/qt5/qt_port_functions.cmake)


list(APPEND CORE_OPTIONS
    -system-tiff
    -system-webp
    -jasper 
    -no-mng # must be explicitly disabled to not automatically pick up mng
    -verbose)
    
find_library(TIFF_RELEASE NAMES tiff PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH) # Depends on lzma
find_library(TIFF_DEBUG NAMES tiffd PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
find_library(JPEG_RELEASE NAMES jpeg jpeg-static PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
find_library(JPEG_DEBUG NAMES jpeg jpeg-static jpegd jpeg-staticd PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
find_library(ZLIB_RELEASE NAMES z zlib PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
find_library(ZLIB_DEBUG NAMES z zlib zd zlibd PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)

find_library(JASPER_RELEASE NAMES jasper PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
find_library(JASPER_DEBUG NAMES jasperd jasper libjasperd libjasper PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
if(NOT VCPKG_TARGET_IS_OSX)
    find_library(FREEGLUT_RELEASE NAMES freeglut glut PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
    find_library(FREEGLUT_DEBUG NAMES freeglutd freeglut glutd glut PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
endif()

find_library(WEBP_RELEASE NAMES webp PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH) 
find_library(WEBP_DEBUG NAMES webpd webp PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
find_library(WEBPDEMUX_RELEASE NAMES webpdemux PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH) 
find_library(WEBPDEMUX_DEBUG NAMES webpdemuxd webpdemux PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
find_library(WEBPMUX_RELEASE NAMES webpmux libwebpmux PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH) 
find_library(WEBPMUX_DEBUG NAMES webpmuxd webpmux libwebpmuxd libwebpmux PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
find_library(WEBPDECODER_RELEASE NAMES webpdecoder libwebpdecoder PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH) 
find_library(WEBPDECODER_DEBUG NAMES webpdecoder libwebpdecoder webpdecoderd libwebpdecoderd PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)
# Depends on opengl in default build but might depend on giflib, libjpeg-turbo, zlib, libpng, tiff, freeglut (!osx), sdl1 (windows) 
# which would require extra libraries to be linked e.g. giflib freeglut sdl1 other ones are already linked

#Dependent libraries
find_library(LZMA_RELEASE lzma PATHS "${CURRENT_INSTALLED_DIR}/lib" NO_DEFAULT_PATH)
find_library(LZMA_DEBUG lzmad lzma PATHS "${CURRENT_INSTALLED_DIR}/debug/lib" NO_DEFAULT_PATH)

if(NOT VCPKG_TARGET_IS_WINDOWS)
    string(APPEND WEBP_RELEASE " -pthread")
    string(APPEND WEBP_DEBUG " -pthread")
endif()

set(OPT_REL "TIFF_LIBS=${TIFF_RELEASE} ${LZMA_RELEASE} ${JPEG_RELEASE} ${ZLIB_RELEASE}"
            "WEBP_LIBS=${WEBPDECODER_RELEASE} ${WEBPDEMUX_RELEASE} ${WEBPMUX_RELEASE} ${WEBP_RELEASE}" 
            "JASPER_LIBS=${JASPER_RELEASE} ${FREEGLUT_RELEASE} ${JPEG_RELEASE} ${ZLIB_RELEASE}") # This will still fail if LIBWEBP is installed with all available features due to the missing additional dependencies
set(OPT_DBG "TIFF_LIBS=${TIFF_DEBUG} ${LZMA_DEBUG} ${JPEG_DEBUG} ${ZLIB_DEBUG}"
            "WEBP_LIBS=${WEBPDECODER_DEBUG} ${WEBPDEMUX_DEBUG} ${WEBPMUX_DEBUG} ${WEBP_DEBUG}"
            "JASPER_LIBS=${JASPER_DEBUG} ${FREEGLUT_DEBUG} ${JPEG_DEBUG} ${ZLIB_DEBUG}")
list(APPEND CORE_OPTIONS "WEBP_INCDIR=${CURRENT_INSTALLED_DIR}/include") # Requires libwebp[all]

qt_submodule_installation(BUILD_OPTIONS ${CORE_OPTIONS} BUILD_OPTIONS_RELEASE ${OPT_REL} BUILD_OPTIONS_DEBUG ${OPT_DBG})
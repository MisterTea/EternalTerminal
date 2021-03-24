if(NOT DEFINED MINGW_ROOT)
    if(DEFINED ENV{MINGW_ROOT})
        SET(MINGW_ROOT $ENV{MINGW_ROOT})
    elseif(DEFINED ENV{MINGW64_ROOT})
        SET(MINGW_ROOT $ENV{MINGW64_ROOT})
    elseif(DEFINED ENV{MINGW32_ROOT})
        SET(MINGW_ROOT $ENV{MINGW32_ROOT})
    else()
        message(FATAL_ERROR "Required variable MINGW_ROOT is not defined. Please check README.md for more details !")
    endif()
endif()

# search for programs in the build host directories
set (CMAKE_FIND_ROOT_PATH_MODE_PROGRAM ONLY)

# for libraries and headers in the target directories
set (CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set (CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set (CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

list(APPEND CMAKE_PREFIX_PATH
    ${MINGW_ROOT}
)

SET (CMAKE_ASM_MASM_COMPILER    "uasm")
SET (CMAKE_C_COMPILER           "clang")
SET (CMAKE_CXX_COMPILER         "clang++")

SET (CMAKE_C_FLAGS              "-fuse-ld=lld")
SET (CMAKE_CXX_FLAGS            ${CMAKE_C_FLAGS})

SET (CMAKE_C_FLAGS_DEBUG        "-O0 -g")
SET (CMAKE_CXX_FLAGS_DEBUG      ${CMAKE_C_FLAGS_DEBUG})

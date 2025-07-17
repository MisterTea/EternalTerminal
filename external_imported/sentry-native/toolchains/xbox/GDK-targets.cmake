# Adapted by Sentry from:
# https://github.com/microsoft/Xbox-GDK-Samples/blob/e5328b9c06443739ec9c7c0089a36c5743c9da15/Samples/Tools/CMakeExample/CMake/GDK-targets.cmake
#
# GDK-targets.cmake : Defines library imports for the Microsoft GDK shared libraries
#
# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

if(_GDK_TARGETS_)
  return()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ERROR: Microsoft GDK only supports 64-bit")
endif()

if(NOT GDK_VERSION)
    message(FATAL_ERROR "ERROR: GDK_VERSION must be set")
endif()

#--- Locate Microsoft GDK
if(BUILD_USING_BWOI)
    if(DEFINED ENV{ExtractedFolder})
        cmake_path(SET ExtractedFolder "$ENV{ExtractedFolder}")
    else()
        set(ExtractedFolder "d:/xtrctd.sdks/BWOIExample/")
    endif()

    if(NOT EXISTS ${ExtractedFolder})
        message(FATAL_ERROR "ERROR: BWOI requires a valid ExtractedFolder (${ExtractedFolder})")
    endif()

    set(Console_SdkRoot "${ExtractedFolder}/Microsoft GDK")
else()
    GET_FILENAME_COMPONENT(Console_SdkRoot "[HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\GDK;GRDKInstallPath]" ABSOLUTE CACHE)
endif()

if(NOT EXISTS "${Console_SdkRoot}/${GDK_VERSION}")
    message(FATAL_ERROR "ERROR: Cannot locate Microsoft Game Development Kit (GDK) - ${GDK_VERSION}")
endif()

#--- GameRuntime Library (for Xbox these are included in the Console_Libs variable)
if(NOT _GDK_XBOX_)
    add_library(Xbox::GameRuntime STATIC IMPORTED)
    set_target_properties(Xbox::GameRuntime PROPERTIES
        IMPORTED_LOCATION "${Console_SdkRoot}/${GDK_VERSION}/GRDK/gameKit/Lib/amd64/xgameruntime.lib"
        MAP_IMPORTED_CONFIG_MINSIZEREL ""
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO ""
        INTERFACE_INCLUDE_DIRECTORIES "${Console_SdkRoot}/${GDK_VERSION}/GRDK/gameKit/Include"
        INTERFACE_COMPILE_FEATURES "cxx_std_11"
        IMPORTED_LINK_INTERFACE_LANGUAGES "CXX")

    if(GDK_VERSION GREATER_EQUAL 220600)
        add_library(Xbox::GameInput STATIC IMPORTED)
        set_target_properties(Xbox::GameInput PROPERTIES
        IMPORTED_LOCATION "${Console_SdkRoot}/${GDK_VERSION}/GRDK/gameKit/Lib/amd64/gameinput.lib"
        MAP_IMPORTED_CONFIG_MINSIZEREL ""
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO ""
        INTERFACE_INCLUDE_DIRECTORIES "${Console_SdkRoot}/${GDK_VERSION}/GRDK/gameKit/Include"
        IMPORTED_LINK_INTERFACE_LANGUAGES "CXX")
    endif()
endif()

#--- Extension Libraries
set(Console_GRDKExtLibRoot "${Console_SdkRoot}/${GDK_VERSION}/GRDK/ExtensionLibraries")
set(ExtensionPlatformToolset 142)

set(_GDK_TARGETS_ ON)

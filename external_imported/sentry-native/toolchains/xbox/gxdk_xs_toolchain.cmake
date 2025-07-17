# Adapted by Sentry from:
# https://github.com/microsoft/Xbox-GDK-Samples/blob/aa45b831e7a71160a69a7d13e9d74844dc6aa210/Samples/Tools/CMakeGDKExample/gxdk_xs_toolchain.cmake
#
# grdk_toolchain.cmake : CMake Toolchain file for Gaming.Xbox.Scarlett.x64
#
# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

mark_as_advanced(CMAKE_TOOLCHAIN_FILE)

if(_GXDK_XS_TOOLCHAIN_)
    return()
endif()

# Microsoft Game Development Kit

include("${CMAKE_CURRENT_LIST_DIR}/DetectGDK.cmake")

set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES GDK_VERSION)

set(CMAKE_SYSTEM_NAME WINDOWS)
set(CMAKE_SYSTEM_VERSION ${GDK_VERSION})

set(CMAKE_GENERATOR_PLATFORM "Gaming.Xbox.Scarlett.x64" CACHE STRING "" FORCE)
set(CMAKE_VS_PLATFORM_NAME "Gaming.Xbox.Scarlett.x64" CACHE STRING "" FORCE)

# Ensure our platform toolset is x64
set(CMAKE_VS_PLATFORM_TOOLSET_HOST_ARCHITECTURE "x64" CACHE STRING "" FORCE)

# Let the GDK MSBuild rules decide the WindowsTargetPlatformVersion
set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "" CACHE STRING "" FORCE)

# Sets platform defines
set(CMAKE_CXX_FLAGS_INIT "$ENV{CFLAGS} ${CMAKE_CXX_FLAGS_INIT} -D_GAMING_XBOX -D_GAMING_XBOX_SCARLETT -DWINAPI_FAMILY=WINAPI_FAMILY_GAMES -D_ATL_NO_DEFAULT_LIBS -D__WRL_NO_DEFAULT_LIB__ -D_CRT_USE_WINAPI_PARTITION_APP -D_UITHREADCTXT_SUPPORT=0 -D__WRL_CLASSIC_COM_STRICT__ /arch:AVX2 /favor:AMD64" CACHE STRING "" FORCE)

# Set platform libraries
set(CMAKE_CXX_STANDARD_LIBRARIES_INIT "xgameplatform.lib" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES ${CMAKE_CXX_STANDARD_LIBRARIES_INIT} CACHE STRING "" FORCE)

foreach(t EXE SHARED MODULE)
    # Prevent accidental use of libraries that are not supported by Game Core on Xbox
    string(APPEND CMAKE_${t}_LINKER_FLAGS_INIT " /NODEFAULTLIB:advapi32.lib /NODEFAULTLIB:comctl32.lib /NODEFAULTLIB:comsupp.lib /NODEFAULTLIB:dbghelp.lib /NODEFAULTLIB:gdi32.lib /NODEFAULTLIB:gdiplus.lib /NODEFAULTLIB:guardcfw.lib /NODEFAULTLIB:kernel32.lib /NODEFAULTLIB:mmc.lib /NODEFAULTLIB:msimg32.lib /NODEFAULTLIB:msvcole.lib /NODEFAULTLIB:msvcoled.lib /NODEFAULTLIB:mswsock.lib /NODEFAULTLIB:ntstrsafe.lib /NODEFAULTLIB:ole2.lib /NODEFAULTLIB:ole2autd.lib /NODEFAULTLIB:ole2auto.lib /NODEFAULTLIB:ole2d.lib /NODEFAULTLIB:ole2ui.lib /NODEFAULTLIB:ole2uid.lib /NODEFAULTLIB:ole32.lib /NODEFAULTLIB:oleacc.lib /NODEFAULTLIB:oleaut32.lib /NODEFAULTLIB:oledlg.lib /NODEFAULTLIB:oledlgd.lib /NODEFAULTLIB:oldnames.lib /NODEFAULTLIB:runtimeobject.lib /NODEFAULTLIB:shell32.lib /NODEFAULTLIB:shlwapi.lib /NODEFAULTLIB:strsafe.lib /NODEFAULTLIB:urlmon.lib /NODEFAULTLIB:user32.lib /NODEFAULTLIB:userenv.lib /NODEFAULTLIB:wlmole.lib /NODEFAULTLIB:wlmoled.lib /NODEFAULTLIB:onecore.lib")
endforeach()

# Add GDK props file
file(GENERATE OUTPUT gdk_build.props INPUT ${CMAKE_CURRENT_LIST_DIR}/gdk_build.props)

# Find DXC compiler
if(NOT GDK_DXCTool)
    GET_FILENAME_COMPONENT(Console_SdkRoot "[HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\GDK;InstallPath]" ABSOLUTE CACHE)

    find_program(
            GDK_DXCTool
            NAMES dxc
            PATHS "${Console_SdkRoot}/${GDK_VERSION}/GXDK/bin/Scarlett"
    )

    mark_as_advanced(GDK_DXCTool)
endif()

set(_GXDK_XS_TOOLCHAIN_ ON)

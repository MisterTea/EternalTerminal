# Adapted by Sentry from:
# https://github.com/microsoft/Xbox-GDK-Samples/blob/e5328b9c06443739ec9c7c0089a36c5743c9da15/Samples/Tools/CMakeExample/CMake/CMakeGDKScarlett.cmake
#
# CMakeGDKScarlett.cmake : CMake definitions for Microsoft GDK targeting Xbox Series X|S
#
# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

mark_as_advanced(CMAKE_TOOLCHAIN_FILE)

if(_GDK_SCARLETT_TOOLCHAIN_)
    return()
endif()

set(XBOX_CONSOLE_TARGET "scarlett" CACHE STRING "")

#--- Microsoft Game Development Kit

include("${CMAKE_CURRENT_LIST_DIR}/DetectGDK.cmake")

set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES GDK_VERSION BUILD_USING_BWOI)

#--- Windows SDK
include("${CMAKE_CURRENT_LIST_DIR}/DetectWindowsSDK.cmake")

set(CMAKE_SYSTEM_NAME WINDOWS)
set(CMAKE_SYSTEM_VERSION ${WINDOWS_SDK_VER})
set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION ${WINDOWS_SDK_VER})

#--- Locate Visual Studio (needed for VC Runtime DLLs)

if (NOT DEFINED VCInstallDir AND DEFINED ENV{VCINSTALLDIR})
    cmake_path(SET VCInstallDir "$ENV{VCINSTALLDIR}")
endif()

if (NOT DEFINED VCInstallDir)
    set(GDK_VS_EDITIONS "Community" "Professional" "Enterprise" "Preview" "BuildTools")

    if (MSVC_TOOLSET_VERSION MATCHES "143")
        foreach(vsedition IN LISTS GDK_VS_EDITIONS)
            cmake_path(SET VCInstallDir "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/${vsedition}/VC")
            if(EXISTS ${VCInstallDir})
                break()
            endif()
        endforeach()
    else()
        foreach(vsedition IN LISTS GDK_VS_EDITIONS)
            cmake_path(SET VCInstallDir "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/2019/${vsedition}/VC")
            if(EXISTS ${VCInstallDir})
                break()
            endif()
        endforeach()

        if (NOT EXISTS ${VCInstallDir})
            foreach(vsedition IN LISTS GDK_VS_EDITIONS)
                cmake_path(SET VCInstallDir "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/${vsedition}/VC")
                if(EXISTS ${VCInstallDir})
                    break()
                endif()
            endforeach()
        endif()
    endif()
endif()

if(EXISTS ${VCInstallDir})
    message("VCInstallDir = ${VCInstallDir}")
else()
    message(FATAL_ERROR "ERROR: Failed to locate Visual Studio 2019 or 2022 install")
endif()

# Find VC toolset/runtime versions
file(STRINGS "${VCInstallDir}/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt" VCToolsVersion)
message("VCToolsVersion = ${VCToolsVersion}")

file(STRINGS "${VCInstallDir}/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt" VCToolsRedistVersion)
message("VCToolsRedistVersion = ${VCToolsRedistVersion}")

#--- GameRuntime and Extension Libraries
set(_GDK_XBOX_ ON)
include("${SENTRY_TOOLCHAINS_DIR}/xbox/GDK-targets.cmake")

set(DurangoXdkInstallPath "${Console_SdkRoot}/${GDK_VERSION}")

message("Microsoft GDK = ${DurangoXdkInstallPath}")

#--- Windows SDK
if(EXISTS "${WINDOWS_SDK}/Include/${WINDOWS_SDK_VER}" )
    message("Windows SDK = v${WINDOWS_SDK_VER} in ${WINDOWS_SDK}")
else()
    message(FATAL_ERROR "ERROR: Cannot locate Windows SDK (${WINDOWS_SDK_VER})")
endif()

#--- Headers
set(Console_EndpointIncludeRoot
        "${DurangoXdkInstallPath}/GXDK/gameKit/Include"
        "${DurangoXdkInstallPath}/GXDK/gameKit/Include/Scarlett"
        "${DurangoXdkInstallPath}/GRDK/gameKit/Include")
set(Console_WindowsIncludeRoot ${WINDOWS_SDK}/Include/${WINDOWS_SDK_VER})
set(Console_SdkIncludeRoot
        "${Console_EndpointIncludeRoot}"
        "${Console_WindowsIncludeRoot}/um"
        "${Console_WindowsIncludeRoot}/shared"
        "${Console_WindowsIncludeRoot}/winrt"
        "${Console_WindowsIncludeRoot}/cppwinrt"
        "${Console_WindowsIncludeRoot}/ucrt")

#--- Libraries
# Don't link with onecore.lib, kernel32.lib, etc.
set(CMAKE_CXX_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES_INIT "")

# Need to link with "onecore" versions of Visual C++ libraries ("msvc_x64_x64" environment uses desktop libpath)
set(VC_OneCore_LibPath "${VCInstallDir}/Tools/MSVC/${VCToolsVersion}/lib/onecore/x64")
if(NOT EXISTS ${VC_OneCore_LibPath}/msvcrt.lib)
    message(FATAL_ERROR "ERROR: Cannot locate msvcrt.lib for the Visual C++ toolset (${VCToolsVersion})")
endif()

set(Console_LibRoot ${WINDOWS_SDK}/Lib/${WINDOWS_SDK_VER})
set(Console_EndpointLibRoot
        "${DurangoXdkInstallPath}/GXDK/gameKit/Lib/amd64"
        "${DurangoXdkInstallPath}/GXDK/gameKit/Lib/amd64/Scarlett"
        "${DurangoXdkInstallPath}/GRDK/gameKit/Lib/amd64")
set(Console_SdkLibPath
        "${Console_EndpointLibRoot}"
        "${Console_LibRoot}/ucrt/x64"
        "${Console_LibRoot}/um/x64")

set(Console_Libs pixevt.lib d3d12_xs.lib xgameplatform.lib xgameruntime.lib xmem.lib xg_xs.lib)

#--- Binaries
set(GameOSFilePath ${DurangoXdkInstallPath}/GXDK/sideload/gameos.xvd)

set(Console_UCRTRedistDebug ${WINDOWS_SDK}/bin/${WINDOWS_SDK_VER}/x64/ucrt)
if(NOT EXISTS ${Console_UCRTRedistDebug}/ucrtbased.dll)
    message(FATAL_ERROR "ERROR: Cannot locate ucrtbased.dll in the Windows SDK (${WINDOWS_SDK_VER})")
endif()

set(CRTPlatformToolset 143)
if (NOT EXISTS "${VCInstallDir}/redist/MSVC/${VCToolsRedistVersion}/onecore/x64/Microsoft.VC${CRTPlatformToolset}.CRT")
    set(CRTPlatformToolset 142)
endif()

message("CRT Platform Toolset = ${CRTPlatformToolset}")

set(CppRuntimeFilesPath "${VCInstallDir}/redist/MSVC/${VCToolsRedistVersion}/onecore/x64/Microsoft.VC${CRTPlatformToolset}.CRT")
set(OpenMPRuntimeFilesPath "${VCInstallDir}/redist/MSVC/${VCToolsRedistVersion}/onecore/x64/Microsoft.VC${CRTPlatformToolset}.OpenMP")
if(CMAKE_BUILD_TYPE MATCHES "Debug")
    set(DebugCppRuntimeFilesPath "${VCInstallDir}/redist/MSVC/${VCToolsRedistVersion}/onecore/Debug_NonRedist/x64/Microsoft.VC${CRTPlatformToolset}.DebugCRT")
    set(DebugOpenMPRuntimeFilesPath "${VCInstallDir}/redist/MSVC/${VCToolsRedistVersion}/onecore/Debug_NonRedist/x64/Microsoft.VC${CRTPlatformToolset}.DebugOpenMP")
endif()

#--- Tools
find_program(MAKEPKG_TOOL makepkg.exe
        REQUIRED NO_SYSTEM_ENVIRONMENT_PATH NO_CMAKE_SYSTEM_PATH NO_DEFAULT_PATH
        HINTS "${Console_SdkRoot}/bin")

message("MGC Tool = ${MAKEPKG_TOOL}")

find_program(DIRECTX_DXC_TOOL dxc.exe
        REQUIRED NO_SYSTEM_ENVIRONMENT_PATH NO_CMAKE_SYSTEM_PATH NO_DEFAULT_PATH
        HINTS "${DurangoXdkInstallPath}/GXDK/bin/Scarlett")

message("DXC Compiler = ${DIRECTX_DXC_TOOL}")

#--- Build options
# Required preprocessor defines
# WIN32
# _WINDOWS

# Standard Debug vs. Release preprocessor definitions
# (automatically defined by MSVC and MSVC-like compilers)
# _DEBUG (Debug)
# NDEBUG (Release without asserts)

# Build as Unicode (see UTF-8 Everywhere article's Win32 recommendations)
set(Console_Defines _UNICODE UNICODE)

# Game Core on Xbox preprocessor definitions
set(Console_Defines ${Console_Defines} WIN32_LEAN_AND_MEAN _GAMING_XBOX WINAPI_FAMILY=WINAPI_FAMILY_GAMES)

# Preprocessor definition for Xbox Series X|S
set(Console_Defines ${Console_Defines} _GAMING_XBOX_SCARLETT)

# Additional recommended preprocessor defines
set(Console_Defines ${Console_Defines} _CRT_USE_WINAPI_PARTITION_APP _UITHREADCTXT_SUPPORT=0 __WRL_CLASSIC_COM_STRICT__)

# Default library controls
set(Console_Defines ${Console_Defines} _ATL_NO_DEFAULT_LIBS __WRL_NO_DEFAULT_LIB__)

set(UnsupportedLibs advapi32.lib comctl32.lib comsupp.lib dbghelp.lib gdi32.lib gdiplus.lib guardcfw.lib kernel32.lib mmc.lib msimg32.lib msvcole.lib msvcoled.lib mswsock.lib ntstrsafe.lib ole2.lib ole2autd.lib ole2auto.lib ole2d.lib ole2ui.lib ole2uid.lib ole32.lib oleacc.lib oleaut32.lib oledlg.lib oledlgd.lib oldnames.lib runtimeobject.lib shell32.lib shlwapi.lib strsafe.lib urlmon.lib user32.lib userenv.lib wlmole.lib wlmoled.lib onecore.lib)

# Required compiler switches:
# /MD or /MDd (VC Runtime DLL)
# /O? or /Od (Optimize code)

# Required linker switches:
# /MACHINE:X64 /SUBSYSTEM:WINDOWS
# /DYNAMICBASE
# /NXCOMPAT
set(Console_LinkOptions /DYNAMICBASE /NXCOMPAT)

# Prevent accidental use of libraries that are not supported by Game Core on Xbox
foreach(arg ${UnsupportedLibs})
    list(APPEND Console_LinkOptions "/NODEFAULTLIB:${arg}")
endforeach()

if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    # /favor:AMD64
    # /arch:AVX2
    set(Console_ArchOptions /favor:AMD64 /arch:AVX2)

    # Scarlett titles should use this switch to optimize the vzeroupper codegen (requires VS 2019 16.3 or later)
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.23)
        set(Console_ArchOptions ${Console_ArchOptions} /d2vzeroupper)
        set(Console_ArchOptions_LTCG /d2:-vzeroupper)
    endif()

endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # -march=znver2 to target AMD Hercules CPU (requires clang v9; otherwise use znver1)
    set(Console_ArchOptions -march=$<IF:$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,9.0>,znver2,znver1>)
endif()

set(_GDK_SCARLETT_TOOLCHAIN_ ON)

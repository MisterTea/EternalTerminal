set(VTK_SHORT_VERSION 9.2)
if(NOT VCPKG_TARGET_IS_WINDOWS)
    message(WARNING "You will need to install Xorg dependencies to build vtk:\napt-get install libxt-dev\n")
endif()

set(VCPKG_POLICY_SKIP_ABSOLUTE_PATHS_CHECK enabled)

vcpkg_download_distfile(
    STRING_PATCH
    URLS https://gitlab.kitware.com/vtk/vtk/-/commit/bfa3e4c7621ddf5826755536eb07284c86db6474.diff?full_index=1
    FILENAME vtk-string-bfa3e4.diff
    SHA512 c5ccb1193e4e61cf78b63802f87ffb09349c5566ad8a4d51418133953f7acd6b4a206f8d41a426a9eb9be3cf1fd95242e6402973252d7979e5a9cb5e5e480d78
)

vcpkg_download_distfile(
    MPI4PY_PATCH_1
    URLS https://gitlab.kitware.com/vtk/vtk/-/commit/c938d30634a284fad026f6ae25c30bc84cadc07e.diff?full_index=1
    FILENAME vtk-mpi4py-update-part1-c938d3.diff
    SHA512 5704c1dd124075bd8f37b0734c5cebd48b470902c74bc23774fd4b69025dbc6bfddf48b7c4511520ed07f03bd666a444d6390569f02a0ab68b5d966ddde3a989
)

vcpkg_download_distfile(
    MPI4PY_PATCH_2
    URLS https://gitlab.kitware.com/vtk/vtk/-/commit/53e6ce92ae4591552e7e00344d69803117d56bfe.diff?full_index=1
    FILENAME vtk-mpi4py-update-part2-53e6ce.diff
    SHA512 794a25bff6168fda94d920a6837c3a690bd6d79284ec34dcd67666c55de78962cc7b73c0f074ce58ed78198bb149eab6bf59b2822f29cbfa792e2fe667c9327c
)

# =============================================================================
# Clone & patch
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Kitware/VTK
    REF 66143ef041b980a51e41ee470d053e67209150f8 # v9.2.x used by ParaView 5.11.0
    SHA512 70662670622082bb8d8b16765bbdf645cfbe62151e93b9673c6f94b356df66ca003e5c78b45e99385f1630aed39c3a8eddecd1d9f5bc0cfb92f5e7e8c06e4dbb
    HEAD_REF master
    PATCHES
        FindLZMA.patch
        FindLZ4.patch
        libproj.patch
        pegtl.patch
        pythonwrapper.patch # Required by ParaView to Wrap required classes
        NoUndefDebug.patch # Required to link against correct Python library depending on build type.
        fix-using-hdf5.patch
        # CHECK: module-name-mangling.patch
        # Last patch TODO: Patch out internal loguru
        FindExpat.patch # The find_library calls are taken care of by vcpkg-cmake-wrapper.cmake of expat
        # fix-gdal.patch TODO?
        cgns.patch
        vtkm.patch
        afxdll.patch
        vtkioss.patch
        jsoncpp.patch
        iotr.patch
        "${STRING_PATCH}"
        "${MPI4PY_PATCH_1}"
        "${MPI4PY_PATCH_2}"
        9690.diff
        missing-include-fixes.patch
)

# =============================================================================
#Overwrite outdated modules if they have not been patched:
file(COPY "${CURRENT_PORT_DIR}/FindHDF5.cmake" DESTINATION "${SOURCE_PATH}/CMake/patches/99") # due to usage of targets in netcdf-c
# =============================================================================

if(HDF5_WITH_PARALLEL AND NOT "mpi" IN_LIST FEATURES)
    message(WARNING "${HDF5_WITH_PARALLEL} Enabling MPI in vtk.")
    list(APPEND FEATURES "mpi")
endif()

# =============================================================================
# Options:
# Collect CMake options for optional components

if("atlmfc" IN_LIST FEATURES)
    list(APPEND ADDITIONAL_OPTIONS
        -DVTK_MODULE_ENABLE_VTK_GUISupportMFC=YES
    )
endif()
if("vtkm" IN_LIST FEATURES)
    list(APPEND ADDITIONAL_OPTIONS
        -DVTK_MODULE_ENABLE_VTK_AcceleratorsVTKmCore=YES
        -DVTK_MODULE_ENABLE_VTK_AcceleratorsVTKmDataModel=YES
        -DVTK_MODULE_ENABLE_VTK_AcceleratorsVTKmFilters=YES
        -DVTK_MODULE_ENABLE_VTK_vtkm=YES
    )
endif()

# TODO:
# - add loguru as a dependency requires #8682
vcpkg_check_features(OUT_FEATURE_OPTIONS VTK_FEATURE_OPTIONS
    FEATURES
        "qt"          VTK_GROUP_ENABLE_Qt
        "qt"          VTK_MODULE_ENABLE_VTK_GUISupportQt
        "qt"          VTK_MODULE_ENABLE_VTK_GUISupportQtSQL
        "qt"          VTK_MODULE_ENABLE_VTK_RenderingQt
        "qt"          VTK_MODULE_ENABLE_VTK_ViewsQt
        "atlmfc"      VTK_MODULE_ENABLE_VTK_GUISupportMFC
        "vtkm"        VTK_MODULE_ENABLE_VTK_AcceleratorsVTKmCore
        "vtkm"        VTK_MODULE_ENABLE_VTK_AcceleratorsVTKmDataModel
        "vtkm"        VTK_MODULE_ENABLE_VTK_AcceleratorsVTKmFilters
        "vtkm"        VTK_MODULE_ENABLE_VTK_vtkm
        "python"      VTK_MODULE_ENABLE_VTK_Python
        "python"      VTK_MODULE_ENABLE_VTK_PythonContext2D
        "python"      VTK_MODULE_ENABLE_VTK_PythonInterpreter
        "paraview"    VTK_MODULE_ENABLE_VTK_FiltersParallelStatistics
        "paraview"    VTK_MODULE_ENABLE_VTK_IOParallelExodus
        "paraview"    VTK_MODULE_ENABLE_VTK_RenderingParallel
        "paraview"    VTK_MODULE_ENABLE_VTK_RenderingVolumeAMR
        "paraview"    VTK_MODULE_ENABLE_VTK_IOXdmf2
        "paraview"    VTK_MODULE_ENABLE_VTK_IOH5part
        "paraview"    VTK_MODULE_ENABLE_VTK_IOParallelLSDyna
        "paraview"    VTK_MODULE_ENABLE_VTK_IOTRUCHAS
        "paraview"    VTK_MODULE_ENABLE_VTK_IOVPIC
        "paraview"    VTK_MODULE_ENABLE_VTK_RenderingAnnotation
        "paraview"    VTK_MODULE_ENABLE_VTK_DomainsChemistry
        "paraview"    VTK_MODULE_ENABLE_VTK_FiltersParallelDIY2
        "paraview"    VTK_MODULE_ENABLE_VTK_cli11
        "mpi"         VTK_GROUP_ENABLE_MPI
        "opengl"      VTK_MODULE_ENABLE_VTK_ImagingOpenGL2
        "opengl"      VTK_MODULE_ENABLE_VTK_RenderingGL2PSOpenGL2
        "opengl"      VTK_MODULE_ENABLE_VTK_RenderingOpenGL2
        "opengl"      VTK_MODULE_ENABLE_VTK_RenderingVolumeOpenGL2
        "opengl"      VTK_MODULE_ENABLE_VTK_opengl
        "openvr"      VTK_MODULE_ENABLE_VTK_RenderingOpenVR
        "gdal"        VTK_MODULE_ENABLE_VTK_IOGDAL
        "geojson"     VTK_MODULE_ENABLE_VTK_IOGeoJSON
)

# Replace common value to vtk value
list(TRANSFORM VTK_FEATURE_OPTIONS REPLACE "=ON" "=YES")
list(TRANSFORM VTK_FEATURE_OPTIONS REPLACE "=OFF" "=DONT_WANT")

if("qt" IN_LIST FEATURES AND NOT EXISTS "${CURRENT_HOST_INSTALLED_DIR}/tools/Qt5/bin/qmlplugindump${VCPKG_HOST_EXECUTABLE_SUFFIX}")
    list(APPEND VTK_FEATURE_OPTIONS -DVTK_MODULE_ENABLE_VTK_GUISupportQtQuick=NO)
endif()
if("qt" IN_LIST FEATURES)
    file(READ "${CURRENT_INSTALLED_DIR}/share/qtbase/vcpkg_abi_info.txt" qtbase_abi_info)
    if(qtbase_abi_info MATCHES "(^|;)gles2(;|$)")
        message(FATAL_ERROR "VTK assumes qt to be build with desktop opengl. As such trying to build vtk with qt using GLES will fail.") 
        # This should really be a configure error but using this approach doesn't require patching. 
    endif()
endif()

if("python" IN_LIST FEATURES)
    set(python_ver "")
    if(NOT VCPKG_TARGET_IS_WINDOWS)
        file(GLOB _py3_include_path "${CURRENT_HOST_INSTALLED_DIR}/include/python3*")
        string(REGEX MATCH "python3\\.([0-9]+)" _python_version_tmp ${_py3_include_path})
        set(PYTHON_VERSION_MINOR "${CMAKE_MATCH_1}")
        set(python_ver "3.${PYTHON_VERSION_MINOR}")
    endif()
    list(APPEND ADDITIONAL_OPTIONS
        -DVTK_WRAP_PYTHON=ON
        -DVTK_PYTHON_VERSION=3
        -DPython3_FIND_REGISTRY=NEVER
        "-DPython3_EXECUTABLE:PATH=${CURRENT_HOST_INSTALLED_DIR}/tools/python3/python${python_ver}${VCPKG_EXECUTABLE_SUFFIX}"
        -DVTK_MODULE_ENABLE_VTK_Python=YES
        -DVTK_MODULE_ENABLE_VTK_PythonContext2D=YES # TODO: recheck
        -DVTK_MODULE_ENABLE_VTK_PythonInterpreter=YES
    )
    #VTK_PYTHON_SITE_PACKAGES_SUFFIX should be set to the install dir of the site-packages
endif()

if ("paraview" IN_LIST FEATURES OR "opengl" IN_LIST FEATURES)
    list(APPEND ADDITIONAL_OPTIONS
        -DVTK_MODULE_ENABLE_VTK_RenderingContextOpenGL2=YES
        -DVTK_MODULE_ENABLE_VTK_RenderingLICOpenGL2=YES
        -DVTK_MODULE_ENABLE_VTK_RenderingAnnotation=YES
        -DVTK_MODULE_ENABLE_VTK_DomainsChemistryOpenGL2=YES
        -DVTK_MODULE_ENABLE_VTK_FiltersParallelDIY2=YES
    )
endif()

if ("paraview" IN_LIST FEATURES AND "python" IN_LIST FEATURES)
    list(APPEND ADDITIONAL_OPTIONS
        -DVTK_MODULE_ENABLE_VTK_WebCore=YES
    )
endif()

if("paraview" IN_LIST FEATURES AND "mpi" IN_LIST FEATURES)
    list(APPEND ADDITIONAL_OPTIONS
        -DVTK_MODULE_ENABLE_VTK_FiltersParallelFlowPaths=YES
    )
endif()

if("mpi" IN_LIST FEATURES AND "python" IN_LIST FEATURES)
    list(APPEND ADDITIONAL_OPTIONS
        -DVTK_MODULE_USE_EXTERNAL_VTK_mpi4py=OFF
    )
endif()

if("cuda" IN_LIST FEATURES AND CMAKE_HOST_WIN32)
    vcpkg_add_to_path("$ENV{CUDA_PATH}/bin")
endif()

if("utf8" IN_LIST FEATURES)
    list(APPEND ADDITIONAL_OPTIONS
        -DKWSYS_ENCODING_DEFAULT_CODEPAGE=CP_UTF8
    )
endif()

if("all" IN_LIST FEATURES)
    list(APPEND ADDITIONAL_OPTIONS
        -DVTK_USE_TK=OFF # TCL/TK currently not included in vcpkg
        -DVTK_FORBID_DOWNLOADS=OFF
    )
else()
    list(APPEND ADDITIONAL_OPTIONS
        -DVTK_FORBID_DOWNLOADS=ON
    )
endif()

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
    "cuda"         VTK_USE_CUDA
    "mpi"          VTK_USE_MPI
    "all"          VTK_BUILD_ALL_MODULES
)

# =============================================================================
# Configure & Install



# We set all libraries to "system" and explicitly list the ones that should use embedded copies
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        ${VTK_FEATURE_OPTIONS}
        -DBUILD_TESTING=OFF
        -DVTK_BUILD_TESTING=OFF
        -DVTK_BUILD_EXAMPLES=OFF
        -DVTK_ENABLE_REMOTE_MODULES=OFF
        # VTK groups to enable
        -DVTK_GROUP_ENABLE_StandAlone=YES
        -DVTK_GROUP_ENABLE_Rendering=YES
        -DVTK_GROUP_ENABLE_Views=YES
        # Disable deps not in VCPKG
        -DVTK_USE_TK=OFF # TCL/TK currently not included in vcpkg
        # Select modules / groups to install
        -DVTK_USE_EXTERNAL:BOOL=ON
        -DVTK_MODULE_USE_EXTERNAL_VTK_gl2ps:BOOL=OFF # Not yet in VCPKG
        #-DVTK_MODULE_ENABLE_VTK_jsoncpp=YES
        ${ADDITIONAL_OPTIONS}
        -DVTK_DEBUG_MODULE_ALL=ON
        -DVTK_DEBUG_MODULE=ON
        -DVTK_QT_VERSION=6
        -DCMAKE_INSTALL_QMLDIR:PATH=qml
        -DVCPKG_HOST_TRIPLET=${_HOST_TRIPLET}
        -DCMAKE_FIND_PACKAGE_TARGETS_GLOBAL=ON # Due to Qt6::Platform not being found on Linux platform
    MAYBE_UNUSED_VARIABLES
        VTK_MODULE_ENABLE_VTK_PythonContext2D # Guarded by a conditional
        VTK_MODULE_ENABLE_VTK_GUISupportMFC # only windows
        VTK_QT_VERSION # Only with Qt
        CMAKE_INSTALL_QMLDIR
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

# =============================================================================
# Fixup target files
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/vtk-${VTK_SHORT_VERSION})

# =============================================================================
# Clean-up other directories

# Delete the debug binary TOOL_NAME that is not required
function(_vtk_remove_debug_tool TOOL_NAME)
    set(filename "${CURRENT_PACKAGES_DIR}/debug/bin/${TOOL_NAME}${VCPKG_TARGET_EXECUTABLE_SUFFIX}")
    if(EXISTS "${filename}")
        file(REMOVE "${filename}")
    endif()
    set(filename "${CURRENT_PACKAGES_DIR}/debug/bin/${TOOL_NAME}d${VCPKG_TARGET_EXECUTABLE_SUFFIX}")
    if(EXISTS "${filename}")
        file(REMOVE "${filename}")
    endif()
    if (NOT VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL debug)
        # we also have to bend the lines referencing the tools in VTKTargets-debug.cmake
        # to make them point to the release version of the tools
        file(READ "${CURRENT_PACKAGES_DIR}/share/vtk/VTK-targets-debug.cmake" VTK_TARGETS_CONTENT_DEBUG)
        string(REPLACE "debug/bin/${TOOL_NAME}" "tools/vtk/${TOOL_NAME}" VTK_TARGETS_CONTENT_DEBUG "${VTK_TARGETS_CONTENT_DEBUG}")
        string(REPLACE "tools/vtk/${TOOL_NAME}d" "tools/vtk/${TOOL_NAME}" VTK_TARGETS_CONTENT_DEBUG "${VTK_TARGETS_CONTENT_DEBUG}")
        file(WRITE "${CURRENT_PACKAGES_DIR}/share/vtk/VTK-targets-debug.cmake" "${VTK_TARGETS_CONTENT_DEBUG}")
    endif()
endfunction()

# Move the release binary TOOL_NAME from bin to tools
function(_vtk_move_release_tool TOOL_NAME)
    set(old_filename "${CURRENT_PACKAGES_DIR}/bin/${TOOL_NAME}${VCPKG_TARGET_EXECUTABLE_SUFFIX}")
    if(EXISTS "${old_filename}")
        file(INSTALL "${old_filename}" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/vtk" USE_SOURCE_PERMISSIONS)
        file(REMOVE "${old_filename}")
    endif()

    if (NOT VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL release)
        # we also have to bend the lines referencing the tools in VTKTargets-release.cmake
        # to make them point to the tool folder
        file(READ "${CURRENT_PACKAGES_DIR}/share/vtk/VTK-targets-release.cmake" VTK_TARGETS_CONTENT_RELEASE)
        string(REPLACE "bin/${TOOL_NAME}" "tools/vtk/${TOOL_NAME}" VTK_TARGETS_CONTENT_RELEASE "${VTK_TARGETS_CONTENT_RELEASE}")
        file(WRITE "${CURRENT_PACKAGES_DIR}/share/vtk/VTK-targets-release.cmake" "${VTK_TARGETS_CONTENT_RELEASE}")
    endif()
endfunction()

set(VTK_TOOLS
    vtkEncodeString-${VTK_SHORT_VERSION}
    vtkHashSource-${VTK_SHORT_VERSION}
    vtkWrapTclInit-${VTK_SHORT_VERSION}
    vtkWrapTcl-${VTK_SHORT_VERSION}
    vtkWrapPythonInit-${VTK_SHORT_VERSION}
    vtkWrapPython-${VTK_SHORT_VERSION}
    vtkWrapJava-${VTK_SHORT_VERSION}
    vtkWrapHierarchy-${VTK_SHORT_VERSION}
    vtkParseJava-${VTK_SHORT_VERSION}
    vtkParseOGLExt-${VTK_SHORT_VERSION}
    vtkProbeOpenGLVersion-${VTK_SHORT_VERSION}
    vtkTestOpenGLVersion-${VTK_SHORT_VERSION}
    vtkpython
    pvtkpython
)
# TODO: Replace with vcpkg_copy_tools if known which tools are built with which feature
# or add and option to vcpkg_copy_tools which does not require the tool to be present
foreach(TOOL_NAME IN LISTS VTK_TOOLS)
    _vtk_remove_debug_tool("${TOOL_NAME}")
    _vtk_move_release_tool("${TOOL_NAME}")
endforeach()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin"
                        "${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_copy_tool_dependencies("${CURRENT_PACKAGES_DIR}/tools/vtk")

## Files Modules needed by ParaView
if("paraview" IN_LIST FEATURES)
    set(VTK_CMAKE_NEEDED vtkCompilerChecks vtkCompilerPlatformFlags vtkCompilerExtraFlags vtkInitializeBuildType
                         vtkSupportMacros vtkVersion FindPythonModules vtkModuleDebugging vtkExternalData)
    foreach(module ${VTK_CMAKE_NEEDED})
        file(INSTALL "${SOURCE_PATH}/CMake/${module}.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/vtk")
    endforeach()

    ## Check List on UPDATE !!
    file(INSTALL "${SOURCE_PATH}/CMake/vtkRequireLargeFilesSupport.cxx" DESTINATION "${CURRENT_PACKAGES_DIR}/share/vtk")
    file(INSTALL "${SOURCE_PATH}/Rendering/Volume/vtkBlockSortHelper.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include/vtk-${VTK_SHORT_VERSION}") # this should get installed by VTK
    file(INSTALL "${SOURCE_PATH}/Filters/ParallelDIY2/vtkDIYKdTreeUtilities.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include/vtk-${VTK_SHORT_VERSION}")

    file(INSTALL "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/Rendering/Parallel/vtkCompositeZPassFS.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include/vtk-${VTK_SHORT_VERSION}")
    file(INSTALL "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/Rendering/OpenGL2/vtkTextureObjectVS.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include/vtk-${VTK_SHORT_VERSION}")

endif()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    if(EXISTS "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/CMakeFiles/vtkpythonmodules/static_python") #python headers
        file(GLOB_RECURSE STATIC_PYTHON_FILES "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/CMakeFiles/*/static_python/*.h")
        file(INSTALL ${STATIC_PYTHON_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/include/vtk-${VTK_SHORT_VERSION}")
    endif()
endif()

#remove one get_filename_component(_vtk_module_import_prefix "${_vtk_module_import_prefix}" DIRECTORY) from vtk-prefix.cmake and VTK-vtk-module-properties and vtk-python.cmake
set(filenames_fix_prefix vtk-prefix VTK-vtk-module-properties vtk-python)
foreach(name IN LISTS filenames_fix_prefix)
if(EXISTS "${CURRENT_PACKAGES_DIR}/share/vtk/${name}.cmake")
    file(READ "${CURRENT_PACKAGES_DIR}/share/vtk/${name}.cmake" _contents)
    string(REPLACE
[[set(_vtk_module_import_prefix "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(_vtk_module_import_prefix "${_vtk_module_import_prefix}" DIRECTORY)]]
[[set(_vtk_module_import_prefix "${CMAKE_CURRENT_LIST_DIR}")]] _contents "${_contents}")
    file(WRITE "${CURRENT_PACKAGES_DIR}/share/vtk/${name}.cmake" "${_contents}")
else()
    debug_message("FILE:${CURRENT_PACKAGES_DIR}/share/vtk/${name}.cmake does not exist! No prefix correction!")
endif()
endforeach()

# Use vcpkg provided find method
file(REMOVE "${CURRENT_PACKAGES_DIR}/share/${PORT}/FindEXPAT.cmake")

file(RENAME "${CURRENT_PACKAGES_DIR}/share/licenses" "${CURRENT_PACKAGES_DIR}/share/${PORT}/licenses")

if(EXISTS "${CURRENT_PACKAGES_DIR}/include/vtk-${VTK_SHORT_VERSION}/vtkChemistryConfigure.h")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/vtk-${VTK_SHORT_VERSION}/vtkChemistryConfigure.h" "${SOURCE_PATH}" "not/existing")
endif()
# =============================================================================
# Usage
configure_file("${CMAKE_CURRENT_LIST_DIR}/usage" "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" COPYONLY)
# Handle copyright
file(INSTALL "${SOURCE_PATH}/Copyright.txt" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME "copyright")

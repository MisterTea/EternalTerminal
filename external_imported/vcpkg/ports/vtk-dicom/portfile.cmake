vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO dgobbi/vtk-dicom
    REF 6c781948c2df3e3a7f7163731d4f65a23e19288f # v0.8.14
    SHA512 9032194ab72b1aa3b4c6a05a7f4de0575dc73b3eb713e9b88b3e3ce332dd9bb906fd456bcb96672b3a4450faa890194afa9ea6bd4c5f9685c9c2610e861b13a6
    HEAD_REF master
)

if ("gdcm" IN_LIST FEATURES)
    set(USE_GDCM                      ON )
else()
    set(USE_GDCM                      OFF )
endif()

if(USE_GDCM)
    list(APPEND ADDITIONAL_OPTIONS
        -DUSE_GDCM=ON
        -DUSE_DCMTK=OFF
    )
endif()

set(python_ver "")
if(NOT VCPKG_TARGET_IS_WINDOWS)
    file(GLOB _py3_include_path "${CURRENT_HOST_INSTALLED_DIR}/include/python3*")
    string(REGEX MATCH "python3\\.([0-9]+)" _python_version_tmp ${_py3_include_path})
    set(PYTHON_VERSION_MINOR "${CMAKE_MATCH_1}")
    set(python_ver "3.${PYTHON_VERSION_MINOR}")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_PROGRAMS=OFF
        -DBUILD_EXAMPLES=OFF
        "-DPython3_EXECUTABLE:PATH=${CURRENT_HOST_INSTALLED_DIR}/tools/python3/python${python_ver}${VCPKG_EXECUTABLE_SUFFIX}"
        ${ADDITIONAL_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/Copyright.txt")


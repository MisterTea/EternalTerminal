vcpkg_fail_port_install(ON_TARGET "Linux" "OSX" "UWP" ON_ARCH "arm" "arm64")

vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

# ngspice produces self-contained DLLs
set(VCPKG_CRT_LINKAGE static)

vcpkg_from_sourceforge(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ngspice/ng-spice-rework
    REF 35
    FILENAME "ngspice-35.tar.gz"
    SHA512 2f9b0f951e3ca8d52692beadb895b352311f67b8760f99d0e2f4718fce4b497dd68e2b933029eeacb4ed57551e959bc6e3747e64feb4722a4f841e734f5a664b
    PATCHES
        use-winbison-sharedspice.patch
        use-winbison-vngspice.patch
        remove-post-build.patch
        remove-64-in-codemodel-name.patch

)

vcpkg_find_acquire_program(BISON)

get_filename_component(BISON_DIR "${BISON}" DIRECTORY)
vcpkg_add_to_path(PREPEND "${BISON_DIR}")

# Sadly, vcpkg globs .libs inside install_msbuild and whines that the 47 year old SPICE format isn't a MSVC lib ;)
# We need to kill them off first before the source tree is copied to a tmp location by install_msbuild

file(REMOVE_RECURSE ${SOURCE_PATH}/contrib)
file(REMOVE_RECURSE ${SOURCE_PATH}/examples)
file(REMOVE_RECURSE ${SOURCE_PATH}/man)
file(REMOVE_RECURSE ${SOURCE_PATH}/tests)

# this builds the main dll
vcpkg_install_msbuild(
    SOURCE_PATH ${SOURCE_PATH}
    INCLUDES_SUBPATH /src/include
    LICENSE_SUBPATH COPYING
    # install_msbuild swaps x86 for win32(bad) if we dont force our own setting
    PLATFORM ${TRIPLET_SYSTEM_ARCH}
    PROJECT_SUBPATH visualc/sharedspice.sln
    TARGET Build
)

if("codemodels" IN_LIST FEATURES)
    # vngspice generates "codemodels" to enhance simulation capabilities
    # we cannot use install_msbuild as they output with ".cm" extensions on purpose
    set(BUILDTREE_PATH ${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET})
    file(REMOVE_RECURSE ${BUILDTREE_PATH})
    file(COPY ${SOURCE_PATH}/ DESTINATION ${BUILDTREE_PATH})

    vcpkg_build_msbuild(
        PROJECT_PATH ${BUILDTREE_PATH}/visualc/vngspice.sln
        # build_msbuild swaps x86 for win32(bad) if we dont force our own setting
        PLATFORM ${TRIPLET_SYSTEM_ARCH}
        TARGET Build
    )

    # ngspice oddly has solution configs of x64 and x86 but
    # output folders of x64 and win32
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL x64)
        set(OUT_ARCH  x64)
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL x86)
        set(OUT_ARCH  Win32)
    else()
        message(FATAL_ERROR "Unsupported target architecture")
    endif()
        
    #put the code models in the intended location
    file(GLOB NGSPICE_CODEMODELS_DEBUG
        ${BUILDTREE_PATH}/visualc/codemodels/${OUT_ARCH}/Debug/*.cm
    )
    file(COPY ${NGSPICE_CODEMODELS_DEBUG} DESTINATION ${CURRENT_PACKAGES_DIR}/debug/lib/ngspice)
    
    file(GLOB NGSPICE_CODEMODELS_RELEASE
        ${BUILDTREE_PATH}/visualc/codemodels/${OUT_ARCH}/Release/*.cm
    )
    file(COPY ${NGSPICE_CODEMODELS_RELEASE} DESTINATION ${CURRENT_PACKAGES_DIR}/lib/ngspice)
    
    
    # copy over spinit (spice init)
    file(RENAME ${BUILDTREE_PATH}/visualc/spinit_all ${BUILDTREE_PATH}/visualc/spinit)
    file(COPY ${BUILDTREE_PATH}/visualc/spinit DESTINATION ${CURRENT_PACKAGES_DIR}/share/ngspice)
endif()

vcpkg_copy_pdbs()

# Unforunately install_msbuild isn't able to dual include directories that effectively layer
file(GLOB NGSPICE_INCLUDES ${SOURCE_PATH}/visualc/src/include/ngspice/*)
file(COPY ${NGSPICE_INCLUDES} DESTINATION ${CURRENT_PACKAGES_DIR}/include/ngspice)

# This gets copied by install_msbuild but should not be shared
file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/include/cppduals)

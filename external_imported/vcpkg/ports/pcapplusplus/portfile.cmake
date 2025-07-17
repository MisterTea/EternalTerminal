vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO seladb/PcapPlusPlus
    REF v21.11
    SHA512 ad10034950c0c3e6a4638e8b314c8983ce42609948d7d8d40ad0ff678820a2469807bd29aff77e657a150008602475b50cea84a0766ad87ea203985519cb38ac
    HEAD_REF master
)

if(VCPKG_TARGET_IS_WINDOWS)
    if(VCPKG_PLATFORM_TOOLSET STREQUAL "v140")
        set(VS_VERSION "vs2015")
    elseif(VCPKG_PLATFORM_TOOLSET STREQUAL "v141")
        set(VS_VERSION "vs2017")
    elseif(VCPKG_PLATFORM_TOOLSET STREQUAL "v142")
        set(VS_VERSION "vs2019")
    else()
        message(FATAL_ERROR "Unsupported visual studio version")
    endif()

    vcpkg_execute_required_process(
        COMMAND configure-windows-visual-studio.bat -v ${VS_VERSION} -w . -p .
        WORKING_DIRECTORY ${SOURCE_PATH}
    )

    vcpkg_install_msbuild(
        SOURCE_PATH "${SOURCE_PATH}"
        PROJECT_SUBPATH "mk/${VS_VERSION}/Common++.vcxproj"
        PLATFORM ${TRIPLET_SYSTEM_ARCH}
        INCLUDES_SUBPATH Dist/header
        ALLOW_ROOT_INCLUDES
        USE_VCPKG_INTEGRATION
    )
    vcpkg_install_msbuild(
        SOURCE_PATH "${SOURCE_PATH}"
        PROJECT_SUBPATH "mk/${VS_VERSION}/Packet++.vcxproj"
        PLATFORM ${TRIPLET_SYSTEM_ARCH}
        INCLUDES_SUBPATH Dist/header
        ALLOW_ROOT_INCLUDES
        USE_VCPKG_INTEGRATION
    )
    vcpkg_install_msbuild(
        SOURCE_PATH "${SOURCE_PATH}"
        PROJECT_SUBPATH "mk/${VS_VERSION}/Pcap++.vcxproj"
        PLATFORM ${TRIPLET_SYSTEM_ARCH}
        USE_VCPKG_INTEGRATION
        INCLUDES_SUBPATH Dist/header
        ALLOW_ROOT_INCLUDES
        LICENSE_SUBPATH LICENSE
    )

    # Lib
    file(GLOB LIB_FILES_RELEASE "${SOURCE_PATH}/Dist/**/Release/*")
    file(
        INSTALL ${LIB_FILES_RELEASE}
        DESTINATION ${CURRENT_PACKAGES_DIR}/lib
    )
    file(GLOB LIB_FILES_RELEASE "${SOURCE_PATH}/Dist/**/Debug/*")
    file(
        INSTALL ${LIB_FILES_RELEASE}
        DESTINATION ${CURRENT_PACKAGES_DIR}/debug/lib
    )

    file(
        REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin" "${CURRENT_PACKAGES_DIR}/tools" "${CURRENT_PACKAGES_DIR}/lib/LightPcapNg.lib" "${CURRENT_PACKAGES_DIR}/debug/lib/LightPcapNg.lib"
    )
else()
    if(VCPKG_TARGET_IS_LINUX)
        set(CONFIG_CMD "./configure-linux.sh")
    elseif(VCPKG_TARGET_IS_OSX)
        set(CONFIG_CMD "./configure-mac_os_x.sh")
    else()
        message(FATAL_ERROR "Unsupported platform")
    endif()

    vcpkg_execute_required_process(
	COMMAND ${SOURCE_PATH}/${CONFIG_CMD} --libpcap-include-dir ${CURRENT_INSTALLED_DIR}/include
        WORKING_DIRECTORY ${SOURCE_PATH}
    )

    vcpkg_execute_build_process(
	COMMAND make libs
	WORKING_DIRECTORY ${SOURCE_PATH}
    )

    # Lib
    file(GLOB LIB_FILES "${SOURCE_PATH}/Dist/*.a")
    file(
        INSTALL ${LIB_FILES}
	    DESTINATION ${CURRENT_PACKAGES_DIR}/lib
    )

    # Include
    file(GLOB HEADER_FILES "${SOURCE_PATH}/Dist/header/*.h")
    file(
        INSTALL ${HEADER_FILES}
        DESTINATION ${CURRENT_PACKAGES_DIR}/include
    )

    # Copyright
    file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)
endif()

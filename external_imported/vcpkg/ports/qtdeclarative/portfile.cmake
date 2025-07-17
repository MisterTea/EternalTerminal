set(SCRIPT_PATH "${CURRENT_INSTALLED_DIR}/share/qtbase")
include("${SCRIPT_PATH}/qt_install_submodule.cmake")

vcpkg_buildpath_length_warning(44)

 set(TOOL_NAMES 
        qml
        qmlcachegen
        qmleasing
        qmlformat
        qmlimportscanner
        qmllint
        qmlplugindump
        qmlpreview
        qmlprofiler
        qmlscene
        qmltestrunner
        qmltime
        qmltyperegistrar
        qmldom
        qmltc
        qmlls
    )

qt_install_submodule(PATCHES    ${${PORT}_PATCHES}
                     TOOL_NAMES ${TOOL_NAMES}
                     CONFIGURE_OPTIONS
                     CONFIGURE_OPTIONS_RELEASE
                     CONFIGURE_OPTIONS_DEBUG
                    )

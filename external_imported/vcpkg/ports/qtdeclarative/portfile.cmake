set(SCRIPT_PATH "${CURRENT_INSTALLED_DIR}/share/qtbase")
include("${SCRIPT_PATH}/qt_install_submodule.cmake")

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
    )

qt_install_submodule(PATCHES    ${${PORT}_PATCHES}
                     TOOL_NAMES ${TOOL_NAMES}
                     CONFIGURE_OPTIONS
                     CONFIGURE_OPTIONS_RELEASE
                     CONFIGURE_OPTIONS_DEBUG
                    )

# remove `${SOURCE_PATH}` from the front of `#line` directives
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/Qt6/QtQml/6.2.2/QtQml/private/qqmljsparser_p.h" "${SOURCE_PATH}" "")

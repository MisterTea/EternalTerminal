# - Try to find the SELinux library
# Once done this will define
#
#  SELINUX_FOUND - System has selinux
#  SELINUX_INCLUDE_DIR - The selinux include directory
#  SELINUX_LIBRARIES - The libraries needed to use selinux
#  SELINUX_DEFINITIONS - Compiler switches required for using selinux

IF (UNIX)
  # use pkg-config to get the directories
  FIND_PACKAGE (PkgConfig)
  PKG_CHECK_MODULES (PC_SELINUX libselinux)
  SET (SELINUX_DEFINITIONS ${PC_SELINUX_CFLAGS_OTHER})

  FIND_PATH (SELINUX_INCLUDE_DIR selinux/selinux.h
    HINTS
    ${PC_SELINUX_INCLUDEDIR}
    ${PC_SELINUX_INCLUDE_DIRS}
    )

  FIND_LIBRARY (SELINUX_LIBRARIES selinux libselinux
    HINTS
    ${PC_SELINUX_LIBDIR}
    ${PC_SELINUX_LIBRARY_DIRS}
    )

  INCLUDE (FindPackageHandleStandardArgs)

  # handle the QUIETLY and REQUIRED arguments and set SELINUX_FOUND
  # to TRUE if all listed variables are TRUE
  FIND_PACKAGE_HANDLE_STANDARD_ARGS (SELinux DEFAULT_MSG
    SELINUX_INCLUDE_DIR
    SELINUX_LIBRARIES
    )

  MARK_AS_ADVANCED (SELINUX_INCLUDE_DIR SELINUX_LIBRARIES)

ENDIF (UNIX)

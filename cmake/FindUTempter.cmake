# - Try to find the UTEMPTER directory notification library
# Once done this will define
#
#  UTEMPTER_FOUND - system has UTEMPTER
#  UTEMPTER_INCLUDE_DIR - the UTEMPTER include directory
#  UTEMPTER_LIBRARIES - The libraries needed to use UTEMPTER

# Copyright (c) 2015, Hrvoje Senjan, <hrvoje.senjan@gmail.org>
#
# Redistribution and use is allowed according to the terms of the BSD license.
# For details see the accompanying COPYING-CMAKE-SCRIPTS file.


find_path (UTEMPTER_INCLUDE_DIR utempter.h)

find_library (UTEMPTER_LIBRARIES NAMES utempter )

include (FindPackageHandleStandardArgs)
find_package_handle_standard_args (UTempter DEFAULT_MSG UTEMPTER_INCLUDE_DIR UTEMPTER_LIBRARIES)

mark_as_advanced (UTEMPTER_INCLUDE_DIR UTEMPTER_LIBRARIES)

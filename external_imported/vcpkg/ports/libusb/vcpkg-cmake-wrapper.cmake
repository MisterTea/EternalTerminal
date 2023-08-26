include(SelectLibraryConfigurations)

find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0)
find_library(LIBUSB_LIBRARY_DEBUG NAMES libusb-1.0 usb-1.0 NAMES_PER_DIR PATH_SUFFIXES lib PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug" NO_DEFAULT_PATH)
find_library(LIBUSB_LIBRARY_RELEASE NAMES libusb-1.0 usb-1.0 NAMES_PER_DIR PATH_SUFFIXES lib PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}" NO_DEFAULT_PATH)

select_library_configurations(LIBUSB)

set(LIBUSB_INCLUDE_DIRS "${LIBUSB_INCLUDE_DIR}")

if (@VCPKG_TARGET_IS_LINUX@)
    list(APPEND LIBUSB_LIBRARIES udev)
endif()

if (@VCPKG_TARGET_IS_OSX@)
    list(APPEND LIBUSB_LIBRARIES "-framework Cocoa")
    list(APPEND LIBUSB_LIBRARIES "-framework IOKit")
    list(APPEND LIBUSB_LIBRARIES "-framework Security")
endif()

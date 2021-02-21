if(MSVC)
    add_library(crashpad_getopt STATIC
        getopt.cc
        getopt.h
    )
    target_include_directories(crashpad_getopt PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    )
    target_link_libraries(crashpad_getopt PRIVATE
        $<BUILD_INTERFACE:crashpad_interface>
    )
else()
    add_library(crashpad_getopt INTERFACE)
endif()

set_property(TARGET crashpad_getopt PROPERTY EXPORT_NAME getopt)
add_library(crashpad::getopt ALIAS crashpad_getopt)

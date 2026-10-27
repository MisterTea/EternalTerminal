add_library(crashpad_mpack STATIC
    mpack.cc
    mpack.h
)
target_include_directories(crashpad_mpack PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(crashpad_mpack PRIVATE
    $<BUILD_INTERFACE:crashpad_interface>
)
crashpad_install_target(crashpad_mpack)

set_property(TARGET crashpad_mpack PROPERTY EXPORT_NAME mpack)
add_library(crashpad::mpack ALIAS crashpad_mpack)

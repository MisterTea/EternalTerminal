add_library(crashpad_client STATIC
    annotation.cc
    annotation.h
    annotation_list.cc
    annotation_list.h
    crash_report_database.cc
    crash_report_database.h
    crashpad_client.h
    crashpad_info.cc
    crashpad_info.h
    prune_crash_reports.cc
    prune_crash_reports.h
    settings.cc
    settings.h
    simple_address_range_bag.h
    simple_string_dictionary.h
    simulate_crash.h
)

if(APPLE AND NOT IOS)
    target_sources(crashpad_client PRIVATE
        crash_report_database_mac.mm
        crashpad_client_mac.cc
        simulate_crash_mac.cc
        simulate_crash_mac.h
    )
endif()

if(IOS)
    target_sources(crashpad_client PRIVATE
        crash_report_database_mac.mm
        crashpad_client_ios.cc
    )
endif()

if(LINUX OR ANDROID)
    target_sources(crashpad_client PRIVATE
        crashpad_client_linux.cc
        simulate_crash_linux.h
        client_argv_handling.cc
        client_argv_handling.h
        crashpad_info_note.S
        crash_report_database_generic.cc
    )
endif()

if(WIN32)
    target_sources(crashpad_client PRIVATE
        crash_report_database_win.cc
        crashpad_client_win.cc
        simulate_crash_win.h
    )
endif()

target_include_directories(crashpad_client INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:include/crashpad/client>"
)
target_link_libraries(crashpad_client
    PRIVATE
        $<BUILD_INTERFACE:crashpad_interface>
    PUBLIC
        crashpad_compat
        crashpad_util
        mini_chromium
)
target_compile_features(crashpad_client PUBLIC cxx_std_14)

set_property(TARGET crashpad_client PROPERTY EXPORT_NAME client)
add_library(crashpad::client ALIAS crashpad_client)

if(WIN32)
    target_link_libraries(crashpad_client PRIVATE rpcrt4)
    if(MSVC)
        target_compile_options(crashpad_client PRIVATE "/wd4201")
    elseif(MINGW)
        target_compile_options(crashpad_client PUBLIC
            "-municode"
        )
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(crashpad_client PRIVATE
            "-Wno-attributes"
        )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(crashpad_client PRIVATE
            "-Wno-unknown-attributes"
            "-Wno-unknown-pragmas"
        )
    endif()
endif()

if(IOS)
    target_link_libraries(crashpad_client
        PUBLIC
            crashpad_minidump
            crashpad_snapshot
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(crashpad_client PRIVATE
        "-Wno-multichar"
    )
endif()

crashpad_install_target(crashpad_client)
crashpad_install_dev(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/crashpad/client"
    FILES_MATCHING PATTERN "*.h"
)

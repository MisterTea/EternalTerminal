# The list of files is drived from: breakpad/Makefile.am

set(BREAKPAD_SOURCES_COMMON
    breakpad/src/common/convert_UTF.cc
    breakpad/src/common/convert_UTF.h
    breakpad/src/common/md5.cc
    breakpad/src/common/md5.h
    breakpad/src/common/string_conversion.cc
    breakpad/src/common/string_conversion.h
)

set(BREAKPAD_SOURCES_COMMON_LINUX
    breakpad/src/common/linux/elf_core_dump.cc
    breakpad/src/common/linux/elfutils.cc
    breakpad/src/common/linux/elfutils.h
    breakpad/src/common/linux/file_id.cc
    breakpad/src/common/linux/file_id.h
    breakpad/src/common/linux/guid_creator.cc
    breakpad/src/common/linux/guid_creator.h
    breakpad/src/common/linux/linux_libc_support.cc
    breakpad/src/common/linux/memory_mapped_file.cc
    breakpad/src/common/linux/safe_readlink.cc
)

set(BREAKPAD_SOURCES_COMMON_LINUX_GETCONTEXT
    breakpad/src/common/linux/breakpad_getcontext.S
)

set(BREAKPAD_SOURCES_COMMON_ANDROID
    breakpad/src/common/android/include/sys/procfs.h
)

set(BREAKPAD_SOURCES_COMMON_WINDOWS
    breakpad/src/common/windows/guid_string.cc
    breakpad/src/common/windows/guid_string.h
)

set(BREAKPAD_SOURCES_COMMON_APPLE
    breakpad/src/common/mac/file_id.cc
    breakpad/src/common/mac/file_id.h
    breakpad/src/common/mac/macho_id.cc
    breakpad/src/common/mac/macho_id.h
    breakpad/src/common/mac/macho_utilities.cc
    breakpad/src/common/mac/macho_utilities.h
    breakpad/src/common/mac/macho_walker.cc
    breakpad/src/common/mac/macho_walker.h
    breakpad/src/common/mac/string_utilities.cc
    breakpad/src/common/mac/string_utilities.h
)

set(BREAKPAD_SOURCES_COMMON_MAC
    breakpad/src/common/mac/MachIPC.mm
    breakpad/src/common/mac/bootstrap_compat.cc
    breakpad/src/common/mac/bootstrap_compat.h
)

set(BREAKPAD_SOURCES_CLIENT_LINUX
    breakpad/src/client/minidump_file_writer-inl.h
    breakpad/src/client/minidump_file_writer.cc
    breakpad/src/client/minidump_file_writer.h
    breakpad/src/client/linux/crash_generation/crash_generation_client.cc
    breakpad/src/client/linux/crash_generation/crash_generation_server.cc
    breakpad/src/client/linux/dump_writer_common/thread_info.cc
    breakpad/src/client/linux/dump_writer_common/ucontext_reader.cc
    breakpad/src/client/linux/handler/exception_handler.cc
    breakpad/src/client/linux/handler/exception_handler.h
    breakpad/src/client/linux/handler/minidump_descriptor.cc
    breakpad/src/client/linux/handler/minidump_descriptor.h
    breakpad/src/client/linux/log/log.cc
    breakpad/src/client/linux/log/log.h
    breakpad/src/client/linux/microdump_writer/microdump_writer.cc
    breakpad/src/client/linux/microdump_writer/microdump_writer.h
    breakpad/src/client/linux/minidump_writer/linux_core_dumper.cc
    breakpad/src/client/linux/minidump_writer/linux_dumper.cc
    breakpad/src/client/linux/minidump_writer/linux_ptrace_dumper.cc
    breakpad/src/client/linux/minidump_writer/minidump_writer.cc
)

set(BREAKPAD_SOURCES_CLIENT_WINDOWS
    breakpad/src/client/windows/crash_generation/crash_generation_client.cc
    breakpad/src/client/windows/crash_generation/crash_generation_client.h
    breakpad/src/client/windows/handler/exception_handler.cc
    breakpad/src/client/windows/handler/exception_handler.h
)

set(BREAKPAD_SOURCES_CLIENT_APPLE
    breakpad/src/client/minidump_file_writer-inl.h
    breakpad/src/client/minidump_file_writer.cc
    breakpad/src/client/minidump_file_writer.h
    breakpad/src/client/mac/handler/breakpad_nlist_64.cc
    breakpad/src/client/mac/handler/breakpad_nlist_64.h
    breakpad/src/client/mac/handler/dynamic_images.cc
    breakpad/src/client/mac/handler/dynamic_images.h
    breakpad/src/client/mac/handler/exception_handler.cc
    breakpad/src/client/mac/handler/exception_handler.h
    breakpad/src/client/mac/handler/minidump_generator.cc
    breakpad/src/client/mac/handler/minidump_generator.h
)

set(BREAKPAD_SOURCES_CLIENT_MAC
    breakpad/src/client/mac/crash_generation/crash_generation_client.cc
    breakpad/src/client/mac/crash_generation/crash_generation_client.h
)

set(BREAKPAD_SOURCES_CLIENT_IOS
    breakpad/src/client/ios/exception_handler_no_mach.cc
    breakpad/src/client/ios/exception_handler_no_mach.h
    breakpad/src/client/ios/handler/ios_exception_minidump_generator.h
    breakpad/src/client/ios/handler/ios_exception_minidump_generator.mm
    breakpad/src/client/mac/crash_generation/ConfigFile.h
    breakpad/src/client/mac/crash_generation/ConfigFile.mm
    breakpad/src/client/mac/handler/mach_vm_compat.h
    breakpad/src/client/mac/handler/protected_memory_allocator.cc
    breakpad/src/client/mac/handler/protected_memory_allocator.h
    breakpad/src/client/mac/handler/ucontext_compat.h
)


add_library(breakpad_client STATIC)
target_sources(breakpad_client PRIVATE ${BREAKPAD_SOURCES_COMMON})

if(LINUX OR ANDROID)
    target_sources(breakpad_client PRIVATE ${BREAKPAD_SOURCES_COMMON_LINUX} ${BREAKPAD_SOURCES_CLIENT_LINUX})
    if(ANDROID)
        target_sources(breakpad_client PRIVATE ${BREAKPAD_SOURCES_COMMON_ANDROID})
        target_include_directories(breakpad_client PRIVATE breakpad/src/common/android/include)
    endif(ANDROID)

    include(CheckFunctionExists)
    check_function_exists(getcontext HAVE_GETCONTEXT)
    if(HAVE_GETCONTEXT)
        target_compile_definitions(breakpad_client PRIVATE HAVE_GETCONTEXT)
    else()
        target_sources(breakpad_client PRIVATE ${BREAKPAD_SOURCES_COMMON_LINUX_GETCONTEXT})
    endif()

    set_property(TARGET breakpad_client PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

if(APPLE)
    target_sources(breakpad_client PRIVATE
        ${BREAKPAD_SOURCES_COMMON_APPLE}
        ${BREAKPAD_SOURCES_CLIENT_APPLE})
    if(NOT IOS)
        target_sources(breakpad_client PRIVATE
            ${BREAKPAD_SOURCES_COMMON_MAC}
            ${BREAKPAD_SOURCES_CLIENT_MAC})
    else()
        target_sources(breakpad_client PRIVATE
            ${BREAKPAD_SOURCES_CLIENT_IOS})
    endif()

    target_link_libraries(breakpad_client PRIVATE "-framework CoreFoundation")
endif()

if(WIN32)
    target_sources(breakpad_client PRIVATE ${BREAKPAD_SOURCES_COMMON_WINDOWS} ${BREAKPAD_SOURCES_CLIENT_WINDOWS})
    target_compile_definitions(breakpad_client PRIVATE _UNICODE UNICODE)

    # set static runtime if enabled
    if(SENTRY_BUILD_RUNTIMESTATIC AND MSVC)
        set_property(TARGET breakpad_client PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# breakpad has includes directly to `third_party/lss/...`,
# which are being resolved correctly when we add the current directory to
# the include directories. A giant hack, yes, but it works
target_include_directories(breakpad_client
	PRIVATE
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/breakpad/src/>"
	PUBLIC
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)

add_library(mini_chromium STATIC)
function(mc_append_sources)
    list(TRANSFORM ARGN PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/mini_chromium/base/")
    target_sources(mini_chromium PRIVATE ${ARGN})
endfunction()

target_sources(mini_chromium PRIVATE build/chromeos_buildflags.h)
mc_append_sources(
    ../build/build_config.h
    atomicops.h
    atomicops_internals_atomicword_compat.h
    atomicops_internals_portable.h
    auto_reset.h
    bit_cast.h
    check.h
    check_op.h
    compiler_specific.h
    containers/span.h
    debug/alias.cc
    debug/alias.h
    files/file_path.cc
    files/file_path.h
    files/file_util.h
    files/scoped_file.cc
    files/scoped_file.h
    format_macros.h
    logging.cc
    logging.h
    memory/free_deleter.h
    memory/page_size.h
    memory/scoped_policy.h
    metrics/histogram_functions.h
    metrics/histogram_macros.h
    metrics/persistent_histogram_allocator.h
    notreached.h
    numerics/checked_math.h
    numerics/checked_math_impl.h
    numerics/clamped_math.h
    numerics/clamped_math_impl.h
    numerics/safe_conversions.h
    numerics/safe_conversions_arm_impl.h
    numerics/safe_conversions_impl.h
    numerics/safe_math.h
    numerics/safe_math_arm_impl.h
    numerics/safe_math_clang_gcc_impl.h
    numerics/safe_math_shared_impl.h
    process/memory.cc
    process/memory.h
    rand_util.cc
    rand_util.h
    scoped_clear_last_error.h
    scoped_generic.h
    strings/pattern.cc
    strings/pattern.h
    strings/strcat.cc
    strings/strcat.h
    strings/strcat_internal.h
    strings/string_number_conversions.cc
    strings/string_number_conversions.h
    strings/string_piece.h
    strings/string_util.h
    strings/stringprintf.cc
    strings/stringprintf.h
    strings/sys_string_conversions.h
    strings/utf_string_conversion_utils.h
    strings/utf_string_conversions.cc
    strings/utf_string_conversions.h
    synchronization/condition_variable.h
    synchronization/lock.cc
    synchronization/lock.h
    synchronization/lock_impl.h
    sys_byteorder.h
    template_util.h
    third_party/icu/icu_utf.cc
    third_party/icu/icu_utf.h
    threading/thread_local_storage.cc
    threading/thread_local_storage.h
    types/cxx23_to_underlying.h
)

if(NOT MINGW)
    mc_append_sources(
        strings/utf_string_conversion_utils.cc
    )
else()
    mc_append_sources(
        ../../utf_string_conversion_utils.mingw.cc
    )
endif()

if(APPLE)
    mc_append_sources(
        apple/bridging.h
        apple/foundation_util.h
        apple/foundation_util.mm
        apple/mach_logging.cc
        apple/mach_logging.h
        apple/scoped_cftyperef.h
        apple/scoped_mach_port.cc
        apple/scoped_mach_port.h
        apple/scoped_mach_vm.cc
        apple/scoped_mach_vm.h
        apple/scoped_nsautorelease_pool.h
        apple/scoped_nsautorelease_pool.mm
        apple/scoped_typeref.h
        strings/sys_string_conversions_mac.mm
    )
    if (NOT IOS)
        mc_append_sources(
            mac/close_nocancel.cc
            mac/scoped_ioobject.h
            mac/scoped_launch_data.h
        )
    endif()
endif()

if(WIN32)
    mc_append_sources(
        memory/page_size_win.cc
        scoped_clear_last_error_win.cc
        strings/string_util_win.cc
        strings/string_util_win.h
        synchronization/lock_impl_win.cc
        threading/thread_local_storage_win.cc
    )
else()
    mc_append_sources(
        files/file_util_posix.cc
        memory/page_size_posix.cc
        posix/eintr_wrapper.h
        posix/safe_strerror.cc
        posix/safe_strerror.h
        strings/string_util_posix.h
        synchronization/condition_variable_posix.cc
        synchronization/lock_impl_posix.cc
        threading/thread_local_storage_posix.cc
    )
endif()

if(APPLE)
    target_compile_options(mini_chromium PUBLIC -fobjc-arc -fno-objc-arc-exceptions)
endif()
if(APPLE AND NOT IOS)
    target_link_libraries(mini_chromium PUBLIC
        "-framework ApplicationServices"
        "-framework CoreFoundation"
        "-framework Foundation"
        "-framework IOKit"
        "-framework Security"
    )
elseif(IOS)
    target_link_libraries(mini_chromium PUBLIC
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework Foundation"
        "-framework Security"
    )
endif()

if(LINUX)
    target_link_libraries(mini_chromium PRIVATE pthread)
endif()

target_include_directories(mini_chromium PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/mini_chromium>"
    $<INSTALL_INTERFACE:include/crashpad/mini_chromium>
)
target_include_directories(mini_chromium PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)
target_link_libraries(mini_chromium
    PRIVATE
        $<BUILD_INTERFACE:crashpad_interface>
)

if(WIN32)
    target_link_libraries(mini_chromium PRIVATE advapi32 kernel32)
    if(MSVC)
        target_compile_options(mini_chromium PRIVATE
            $<$<COMPILE_LANGUAGE:C,CXX>:/wd4201> # nonstandard extension used : nameless struct/union.
            $<$<COMPILE_LANGUAGE:C,CXX>:/wd4244> # conversion from '__int64' to 'int32_t', possible loss of data.
            $<$<COMPILE_LANGUAGE:C,CXX>:/wd4996> # 'X' was declared deprecated.
        )
        target_compile_definitions(mini_chromium PRIVATE
            NOMINMAX
            UNICODE
            WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS
            _HAS_EXCEPTIONS=0
            _UNICODE
        )
    elseif(MINGW)
        target_compile_options(mini_chromium PRIVATE
            "-municode"
            "-Wno-format"
            "-Wno-unknown-pragmas"
        )
    endif()
endif()

add_library(crashpad::mini_chromium ALIAS mini_chromium)

crashpad_install_target(mini_chromium)
crashpad_install_dev(DIRECTORY mini_chromium
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/crashpad"
    FILES_MATCHING PATTERN "*.h"
)
crashpad_install_dev(DIRECTORY build
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/crashpad/mini_chromium"
    FILES_MATCHING PATTERN "*.h"
)

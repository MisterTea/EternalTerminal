#[===[.md:
# vcpkg_fail_port_install

Checks common requirements and fails the current portfile with a (default) error message

## Usage
```cmake
vcpkg_fail_port_install(
    [ALWAYS]
    [MESSAGE <"Reason for failure">]
    [ON_TARGET <Windows> [<OSX> ...]]
    [ON_ARCH <x64> [<arm> ...]]
    [ON_CRT_LINKAGE <static> [<dynamic> ...]])
    [ON_LIBRARY_LINKAGE <static> [<dynamic> ...]]
)
```

## Parameters
### MESSAGE
Additional failure message. If none is given, a default message will be displayed depending on the failure condition.

### ALWAYS
Will always fail early

### ON_TARGET
Targets for which the build should fail early. Valid targets are `<target>` from `VCPKG_IS_TARGET_<target>` (see `vcpkg_common_definitions.cmake`).

### ON_ARCH
Architecture for which the build should fail early.

### ON_CRT_LINKAGE
CRT linkage for which the build should fail early.

### ON_LIBRARY_LINKAGE
Library linkage for which the build should fail early.

## Examples

* [aws-lambda-cpp](https://github.com/Microsoft/vcpkg/blob/master/ports/aws-lambda-cpp/portfile.cmake)
#]===]

function(vcpkg_fail_port_install)
    set(multi_args "ON_TARGET;ON_ARCH;ON_CRT_LINKAGE;ON_LIBRARY_LINKAGE")
    cmake_parse_arguments(PARSE_ARGV 0 "arg" "ALWAYS" "MESSAGE" "${multi_args}")
    if(DEFINED arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "vcpkg_fail_port_install was passed extra arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()

    if(arg_ALWAYS)
        vcpkg_list(SET extra_args)
        foreach(arg IN LISTS multi_args)
            if(DEFINED "arg_${arg}")
                vcpkg_list(APPEND extra_args "${arg}" "${arg_${arg}}")
            endif()
        endforeach()
        if(NOT "${extra_args}" STREQUAL "")
            message(WARNING "vcpkg_fail_port_install set to fail both unconditionally and conditionally on ${extra_args}. This is likely to be an error.")
        endif()
        if(NOT DEFINED arg_MESSAGE)
            message(FATAL_ERROR "vcpkg_fail_port_install(ALWAYS) was called without a specific MESSAGE.")
        endif()

        message(FATAL_ERROR "${arg_MESSAGE}")
    endif()

    if(DEFINED arg_MESSAGE)
        string(APPEND arg_MESSAGE "\n")
    else()
        set(arg_MESSAGE "")
    endif()

    set(fail_port OFF)
    # Target fail check
    if(DEFINED arg_ON_TARGET)
        foreach(target IN LISTS arg_ON_TARGET)
            string(TOUPPER "${target}" target_upper)
            if(VCPKG_TARGET_IS_${target_upper})
                set(fail_port ON)
                string(APPEND arg_MESSAGE "Target '${target}' not supported by ${PORT}!\n")
            endif()
        endforeach()
    endif()

    # Architecture fail check
    if(DEFINED arg_ON_ARCH)
        foreach(arch IN LISTS arg_ON_ARCH)
            if(VCPKG_TARGET_ARCHITECTURE STREQUAL arch)
                set(fail_port ON)
                string(APPEND arg_MESSAGE "Architecture '${arch}' not supported by ${PORT}!\n")
            endif()
        endforeach()
    endif()

    # CRT linkage fail check
    if(DEFINED arg_ON_CRT_LINKAGE)
        foreach(crt_linkage IN LISTS arg_ON_CRT_LINKAGE)
            if(VCPKG_CRT_LINKAGE STREQUAL crt_linkage)
                set(fail_port ON)
                string(APPEND arg_MESSAGE "CRT linkage '${VCPKG_CRT_LINKAGE}' not supported by ${PORT}!\n")
            endif()
        endforeach()
    endif()

    # Library linkage fail check
    if(DEFINED arg_ON_LIBRARY_LINKAGE)
        foreach(library_linkage IN LISTS arg_ON_LIBRARY_LINKAGE)
            if(VCPKG_LIBRARY_LINKAGE STREQUAL library_linkage)
                set(fail_port ON)
                string(APPEND arg_MESSAGE "Library linkage '${VCPKG_LIBRARY_LINKAGE}' not supported by ${PORT}!\n")
            endif()
        endforeach()
    endif()

    if(fail_port)
        message(FATAL_ERROR "${arg_MESSAGE}")
    endif()
endfunction()

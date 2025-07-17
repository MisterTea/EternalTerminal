function(vcpkg_from_git)
    cmake_parse_arguments(PARSE_ARGV 0 "arg"
        ""
        "OUT_SOURCE_PATH;URL;REF;FETCH_REF;HEAD_REF;TAG;LFS"
        "PATCHES"
    )

    if(DEFINED arg_UNPARSED_ARGUMENTS)
        message(WARNING "vcpkg_from_git was passed extra arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(DEFINED arg_TAG)
        message(WARNING "The TAG argument to vcpkg_from_git has been deprecated and has no effect.")
    endif()

    if(NOT DEFINED arg_OUT_SOURCE_PATH)
        message(FATAL_ERROR "OUT_SOURCE_PATH must be specified")
    endif()
    if(NOT DEFINED arg_URL)
        message(FATAL_ERROR "URL must be specified")
    endif()
    if(NOT DEFINED arg_REF AND NOT DEFINED arg_HEAD_REF)
        message(FATAL_ERROR "At least one of REF or HEAD_REF must be specified")
    endif()
    if(DEFINED arg_FETCH_REF AND NOT DEFINED arg_REF)
        message(FATAL_ERROR "REF must be specified if FETCH_REF is specified")
    endif()
    if(NOT DEFINED arg_LFS AND "LFS" IN_LIST arg_KEYWORDS_MISSING_VALUES)
        set(arg_LFS "${arg_URL}")
    endif()

    vcpkg_list(SET git_fetch_shallow_param --depth 1)
    vcpkg_list(SET extract_working_directory_param)
    vcpkg_list(SET skip_patch_check_param)
    set(git_working_directory "${DOWNLOADS}/git-tmp")
    set(do_download OFF)

    if(VCPKG_USE_HEAD_VERSION AND DEFINED arg_HEAD_REF)
        vcpkg_list(SET working_directory_param "WORKING_DIRECTORY" "${CURRENT_BUILDTREES_DIR}/src/head")
        vcpkg_list(SET git_fetch_shallow_param --depth 1)
        vcpkg_list(SET skip_patch_check_param SKIP_PATCH_CHECK)
        set(ref_to_fetch "${arg_HEAD_REF}")
        set(git_working_directory "${CURRENT_BUILDTREES_DIR}/src/git-tmp")
        string(REPLACE "/" "_-" sanitized_ref "${arg_HEAD_REF}")

        if(NOT _VCPKG_NO_DOWNLOADS)
            set(do_download ON)
        endif()
    else()
        if(NOT DEFINED arg_REF)
            message(FATAL_ERROR "Package does not specify REF. It must be built using --head.")
        endif()
        if(VCPKG_USE_HEAD_VERSION)
            message(STATUS "Package does not specify HEAD_REF. Falling back to non-HEAD version.")
        endif()

        if(DEFINED arg_FETCH_REF)
            set(ref_to_fetch "${arg_FETCH_REF}")
            vcpkg_list(SET git_fetch_shallow_param)
        else()
            set(ref_to_fetch "${arg_REF}")
        endif()
        string(REPLACE "/" "_-" sanitized_ref "${arg_REF}")
    endif()

    set(temp_archive "${DOWNLOADS}/temp/${PORT}-${sanitized_ref}.tar.gz")
    set(archive "${DOWNLOADS}/${PORT}-${sanitized_ref}.tar.gz")

    if(NOT EXISTS "${archive}")
        if(_VCPKG_NO_DOWNLOADS)
            message(FATAL_ERROR "Downloads are disabled, but '${archive}' does not exist.")
        endif()
        set(do_download ON)
    endif()

    if(do_download)
        message(STATUS "Fetching ${arg_URL} ${ref_to_fetch}...")
        find_program(GIT NAMES git git.cmd)
        file(MAKE_DIRECTORY "${DOWNLOADS}")
        # Note: git init is safe to run multiple times
        vcpkg_execute_required_process(
            ALLOW_IN_DOWNLOAD_MODE
            COMMAND "${GIT}" init "${git_working_directory}"
            WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}"
            LOGNAME "git-init-${TARGET_TRIPLET}"
        )
        vcpkg_execute_required_process(
            ALLOW_IN_DOWNLOAD_MODE
            COMMAND "${GIT}" fetch "${arg_URL}" "${ref_to_fetch}" ${git_fetch_shallow_param} -n
            WORKING_DIRECTORY "${git_working_directory}"
            LOGNAME "git-fetch-${TARGET_TRIPLET}"
        )
        if(arg_LFS)
            # Running "git lfs" searches for "git-lfs[.exe]" on the path
            vcpkg_execute_in_download_mode(
                COMMAND "${GIT}" lfs --version
                OUTPUT_VARIABLE lfs_version_output
                ERROR_VARIABLE lfs_version_error
                RESULT_VARIABLE lfs_version_result
                WORKING_DIRECTORY "${git_working_directory}"
            )
            if(lfs_version_result)
                message(FATAL_ERROR "Git LFS is required for ${PORT}")
            endif()

            vcpkg_execute_required_process(
                ALLOW_IN_DOWNLOAD_MODE
                COMMAND ${GIT} lfs install --local --force
                WORKING_DIRECTORY "${git_working_directory}"
                LOGNAME "git-lfs-install-${TARGET_TRIPLET}"
            )
            vcpkg_execute_required_process(
                ALLOW_IN_DOWNLOAD_MODE
                COMMAND ${GIT} lfs fetch "${arg_LFS}" "${ref_to_fetch}"
                WORKING_DIRECTORY "${git_working_directory}"
                LOGNAME "git-lfs-fetch-${TARGET_TRIPLET}"
            )
        endif()

        if(VCPKG_USE_HEAD_VERSION)
            vcpkg_execute_in_download_mode(
                COMMAND "${GIT}" rev-parse FETCH_HEAD
                OUTPUT_VARIABLE rev_parse_ref
                ERROR_VARIABLE rev_parse_ref
                RESULT_VARIABLE error_code
                WORKING_DIRECTORY "${git_working_directory}"
            )
            if(error_code)
                message(FATAL_ERROR "unable to determine FETCH_HEAD after fetching git repository")
            endif()
            string(STRIP "${rev_parse_ref}" rev_parse_ref)
            set(VCPKG_HEAD_VERSION "${rev_parse_ref}" PARENT_SCOPE)
        else()
            vcpkg_execute_in_download_mode(
                COMMAND "${GIT}" rev-parse "${arg_REF}"
                OUTPUT_VARIABLE rev_parse_ref
                ERROR_VARIABLE rev_parse_ref
                RESULT_VARIABLE error_code
                WORKING_DIRECTORY "${git_working_directory}"
            )
            if(error_code)
                message(FATAL_ERROR "unable to rev-parse ${arg_REF} after fetching git repository")
            endif()
            string(STRIP "${rev_parse_ref}" rev_parse_ref)
            if(NOT "${rev_parse_ref}" STREQUAL "${arg_REF}")
                message(FATAL_ERROR "REF (${arg_REF}) does not match rev-parse'd reference (${rev_parse_ref})
        [Expected : ( ${arg_REF} )])
        [  Actual : ( ${rev_parse_ref} )]"
                )
            endif()
        endif()

        file(MAKE_DIRECTORY "${DOWNLOADS}/temp")
        vcpkg_execute_required_process(
            ALLOW_IN_DOWNLOAD_MODE
            COMMAND "${GIT}" -c core.autocrlf=false archive "${rev_parse_ref}" -o "${temp_archive}"
            WORKING_DIRECTORY "${git_working_directory}"
            LOGNAME git-archive
        )
        file(RENAME "${temp_archive}" "${archive}")
    else()
        message(STATUS "Using cached ${archive}")
    endif()

    vcpkg_extract_source_archive_ex(
        OUT_SOURCE_PATH SOURCE_PATH
        ARCHIVE "${archive}"
        REF "${sanitized_ref}"
        PATCHES ${arg_PATCHES}
        NO_REMOVE_ONE_LEVEL
        ${extract_working_directory_param}
        ${skip_patch_check_param}
    )

    set("${arg_OUT_SOURCE_PATH}" "${SOURCE_PATH}" PARENT_SCOPE)
endfunction()

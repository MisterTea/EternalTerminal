FIND_PACKAGE (GTest REQUIRED)
FIND_PACKAGE (ZLIB REQUIRED)
FIND_PACKAGE (Threads REQUIRED)

INCLUDE_DIRECTORIES (
    ${GTEST_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
)

SET (tests_C
    buffer_c.cpp
    fixint_c.cpp
    msgpack_c.cpp
    pack_unpack_c.cpp
    streaming_c.cpp
)

IF (NOT MSGPACK_CXX_ONLY)
    LIST (APPEND check_PROGRAMS
        ${tests_C}
    )
ENDIF ()

IF (MSGPACK_ENABLE_CXX)
    LIST (APPEND check_PROGRAMS
        array_ref.cpp
        buffer.cpp
        carray.cpp
        cases.cpp
        convert.cpp
        fixint.cpp
        inc_adaptor_define.cpp
        json.cpp
        limit.cpp
        msgpack_basic.cpp
        msgpack_container.cpp
        msgpack_stream.cpp
        msgpack_tuple.cpp
        msgpack_vref.cpp
        object.cpp
        object_with_zone.cpp
        pack_unpack.cpp
        raw.cpp
        reference.cpp
        size_equal_only.cpp
        streaming.cpp
        user_class.cpp
        version.cpp
        visitor.cpp
        zone.cpp
    )

    IF (MSGPACK_BOOST)
        LIST (APPEND check_PROGRAMS
            boost_fusion.cpp
            boost_variant.cpp
            boost_optional.cpp
            boost_string_ref.cpp
            boost_string_view.cpp
        )
    ENDIF ()

    IF (MSGPACK_USE_X3_PARSE)
        LIST (APPEND check_PROGRAMS
            msgpack_x3_parse.cpp
        )
    ENDIF ()

    IF (MSGPACK_CXX11 OR MSGPACK_CXX17)
        LIST (APPEND check_PROGRAMS
            iterator_cpp11.cpp
            msgpack_cpp11.cpp
            reference_cpp11.cpp
            reference_wrapper_cpp11.cpp
            shared_ptr_cpp11.cpp
            unique_ptr_cpp11.cpp

            # fuzzers are cpp11 only
            fuzz_unpack_pack_fuzzer_cpp11.cpp
        )
    ENDIF ()

    IF (MSGPACK_CXX17)
        LIST (APPEND check_PROGRAMS
            msgpack_cpp17.cpp
        )
    ENDIF ()
ENDIF (MSGPACK_ENABLE_CXX)

FOREACH (source_file ${check_PROGRAMS})
    GET_FILENAME_COMPONENT (source_file_we ${source_file} NAME_WE)
    ADD_EXECUTABLE (
        ${source_file_we}
        ${source_file}
    )

    LIST (FIND tests_C ${source_file} idx)
    IF (idx GREATER -1)
        SET (link_target msgpackc)
    ELSE ()
        SET (link_target msgpackc-cxx)
    ENDIF ()

    TARGET_LINK_LIBRARIES (${source_file_we}
        ${link_target}
        ${GTEST_BOTH_LIBRARIES}
        ${ZLIB_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
    )
    ADD_TEST (${source_file_we} ${source_file_we})
    IF ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        SET_PROPERTY (TARGET ${source_file_we} APPEND_STRING PROPERTY COMPILE_FLAGS " -Wall -Wextra -Wconversion")
    ENDIF ()
    IF ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
        SET_PROPERTY (TARGET ${source_file_we} APPEND_STRING PROPERTY COMPILE_FLAGS " -Wno-mismatched-tags")
    ENDIF ()

    IF ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
        IF (CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
            STRING(REGEX REPLACE "/W[0-4]" "/W3 /WX" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        ELSE ()
            SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /WX")
        ENDIF ()
    ENDIF ()
ENDFOREACH ()

IF (MSGPACK_ENABLE_CXX)
    ADD_EXECUTABLE (
        multi_file
        multi_file1.cpp multi_file2.cpp
    )
    TARGET_INCLUDE_DIRECTORIES (multi_file
        PRIVATE
            $<TARGET_PROPERTY:msgpackc-cxx,INTERFACE_INCLUDE_DIRECTORIES>
    )
    ADD_TEST (multi_file multi_file)
ENDIF ()

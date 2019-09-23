IF (MSGPACK_BOOST)
    LIST (APPEND exec_PROGRAMS
        msgpack_variant_capitalize.cpp
        msgpack_variant_mapbased.cpp
    )
    IF (MSGPACK_CXX11 OR MSGPACK_CXX17)
        FIND_PACKAGE (Threads REQUIRED)
        LIST (APPEND exec_PROGRAMS
            asio_send_recv.cpp
        )
        IF (ZLIB_FOUND)
            INCLUDE_DIRECTORIES (
                ${ZLIB_INCLUDE_DIRS}
            )
            LIST (APPEND exec_PROGRAMS
                asio_send_recv_zlib.cpp
            )
        ENDIF ()
    ENDIF ()
ENDIF ()

FOREACH (source_file ${exec_PROGRAMS})
    GET_FILENAME_COMPONENT (source_file_we ${source_file} NAME_WE)
    ADD_EXECUTABLE (
        ${source_file_we}
        ${source_file}
    )
    TARGET_INCLUDE_DIRECTORIES (${source_file_we}
        PRIVATE
            $<TARGET_PROPERTY:msgpackc,INTERFACE_INCLUDE_DIRECTORIES>
    )
    TARGET_LINK_LIBRARIES (${source_file_we}
        ${Boost_SYSTEM_LIBRARY}
        ${CMAKE_THREAD_LIBS_INIT}
    )
    IF (ZLIB_FOUND)
        TARGET_LINK_LIBRARIES (${source_file_we}
            ${ZLIB_LIBRARIES}
        )
    ENDIF()
    IF ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        SET_PROPERTY (TARGET ${source_file_we} APPEND_STRING PROPERTY COMPILE_FLAGS " -Wall -Wextra")
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

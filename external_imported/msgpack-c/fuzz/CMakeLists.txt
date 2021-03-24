FIND_PACKAGE (GTest REQUIRED)
FIND_PACKAGE (ZLIB REQUIRED)
FIND_PACKAGE (Boost REQUIRED COMPONENTS system filesystem)

INCLUDE_DIRECTORIES (
  ${GTEST_INCLUDE_DIRS}
  ${ZLIB_INCLUDE_DIRS}
  ${Boost_INCLUDE_DIRS}
)

LIST (APPEND check_PROGRAMS
    regression_runner.cpp
)

LINK_DIRECTORIES (
  ${Boost_LIBRARY_DIRS}
)

FOREACH (source_file ${check_PROGRAMS})
    GET_FILENAME_COMPONENT (source_file_we ${source_file} NAME_WE)
    ADD_EXECUTABLE (
        ${source_file_we}
        ${source_file}
    )
    TARGET_LINK_LIBRARIES (${source_file_we}
        msgpackc
        ${GTEST_BOTH_LIBRARIES}
        ${ZLIB_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ${Boost_LIBRARIES}
    )
    ADD_TEST (${source_file_we} ${source_file_we})
    IF ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
      SET_PROPERTY (TARGET ${source_file_we} APPEND_STRING PROPERTY COMPILE_FLAGS "-Wall -Wextra -Wno-mismatched-tags -g")
        IF ("${MSGPACK_SAN}" STREQUAL "ASAN")
            SET (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
            SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
        ELSEIF ("${MSGPACK_SAN}" STREQUAL "UBSAN")
            SET (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=undefined")
            SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=undefined")
        ENDIF()
    ENDIF()
ENDFOREACH ()

cmake_minimum_required (VERSION 3.0)
project (example)

if(EXAMPLE_MSGPACK_EMBEDDED)
    add_subdirectory(msgpack-c)
    set(msgpack_DIR ${CMAKE_CURRENT_BINARY_DIR}/msgpack-c)
endif()

find_package(msgpack REQUIRED)

add_executable (${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/../c/simple_c.c)
target_link_libraries(${PROJECT_NAME} msgpackc)

if(TARGET msgpackc-static)
    add_executable (${PROJECT_NAME}-static ${CMAKE_CURRENT_LIST_DIR}/../c/simple_c.c)
    target_link_libraries(${PROJECT_NAME}-static msgpackc-static)
endif()

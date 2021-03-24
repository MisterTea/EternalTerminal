cmake_minimum_required(VERSION 2.6)

project(Base64)

add_subdirectory(tests)

install(FILES base64.h
    DESTINATION include)
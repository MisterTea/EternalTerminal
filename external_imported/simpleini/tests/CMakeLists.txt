if(SIMPLEINI_USE_SYSTEM_GTEST)
	find_package(GTest REQUIRED)
else()
	include(FetchContent)
    # do not install GoogleTest
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
	# Force GoogleTest to use the same runtime library as our tests (MSVC)
	set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
	FetchContent_Declare(
		googletest
		DOWNLOAD_EXTRACT_TIMESTAMP ON
		URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
		URL_HASH SHA1=0ac421f2ec11af38b0fff0f1992184032731a8bc
		)
	FetchContent_MakeAvailable(googletest)

	# GoogleTest v1.14.0+ requires C++17
	# Set C++ standard on GoogleTest targets to match our tests
	set_target_properties(gtest gtest_main PROPERTIES
		CXX_STANDARD 17
		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF
	)
endif()

set(TEST_SOURCES
	ts-bugfix.cpp
	ts-noconvert.cpp
	ts-quotes.cpp
	ts-roundtrip.cpp
	ts-snippets.cpp
	ts-utf8.cpp
	ts-numeric.cpp
	ts-boolean.cpp
	ts-sections.cpp
	ts-deletion.cpp
	ts-edgecases.cpp
	ts-multiline.cpp
	ts-casesensitivity.cpp
	ts-generic.cpp
	ts-utf8-conversion.cpp
	ts-regressions.cpp
	ts-iostream.cpp
)

# ts-wchar.cpp uses wchar_t which is primarily for Windows
if(WIN32)
	list(APPEND TEST_SOURCES ts-wchar.cpp)
endif()

add_executable(tests ${TEST_SOURCES})

set_target_properties(tests PROPERTIES
	CXX_STANDARD 17
	CXX_STANDARD_REQUIRED ON
	CXX_EXTENSIONS OFF
)
if(MSVC)
	target_compile_options(tests PRIVATE /utf-8 /W4 /WX)
else()
	target_compile_options(tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME tests COMMAND tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(tests PRIVATE ${PROJECT_NAME} GTest::gtest_main)
if(APPLE)
	target_link_libraries(tests PRIVATE iconv)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_options(tests PRIVATE "-Wl,--wrap=malloc")
endif()

add_custom_command(
	TARGET tests POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${CMAKE_CURRENT_SOURCE_DIR}/data
		${CMAKE_CURRENT_BINARY_DIR}/data
	COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_SOURCE_DIR}/example.ini
		${CMAKE_CURRENT_SOURCE_DIR}/tests.ini
		${CMAKE_CURRENT_BINARY_DIR})

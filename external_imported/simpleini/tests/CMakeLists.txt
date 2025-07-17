if(SIMPLEINI_USE_SYSTEM_GTEST)
	find_package(GTest REQUIRED)
else()
	include(FetchContent)
	FetchContent_Declare(
		googletest
		URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
		URL_HASH SHA1=0ac421f2ec11af38b0fff0f1992184032731a8bc
		DOWNLOAD_EXTRACT_TIMESTAMP ON)
	FetchContent_MakeAvailable(googletest)
endif()

add_executable(tests
	ts-bugfix.cpp
	ts-noconvert.cpp
	ts-quotes.cpp
	ts-roundtrip.cpp
	ts-snippets.cpp
	ts-utf8.cpp)

add_test(NAME tests COMMAND tests)
target_link_libraries(tests PRIVATE ${PROJECT_NAME} GTest::gtest_main)

add_custom_command(
	TARGET tests POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_SOURCE_DIR}/example.ini
		${CMAKE_CURRENT_SOURCE_DIR}/tests.ini
		${CMAKE_CURRENT_BINARY_DIR})

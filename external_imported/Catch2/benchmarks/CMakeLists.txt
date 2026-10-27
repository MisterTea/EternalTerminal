include(CatchMiscFunctions)

add_executable(AssertionsFastPath
  runtime_assertion_benches.cpp
)

add_executable(AssertionsSlowPath
  runtime_assertion_benches.cpp
  assertion_listener.cpp
)

add_executable(EmptyExecutable
  only_include.cpp
)

target_link_libraries(AssertionsFastPath PRIVATE Catch2::Catch2WithMain)
target_link_libraries(AssertionsSlowPath PRIVATE Catch2::Catch2WithMain)
target_link_libraries(EmptyExecutable PRIVATE Catch2::Catch2WithMain)

list(APPEND CATCH_TEST_TARGETS AssertionsFastPath AssertionsSlowPath EmptyExecutable)
set(CATCH_TEST_TARGETS ${CATCH_TEST_TARGETS} PARENT_SCOPE)

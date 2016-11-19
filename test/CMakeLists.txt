add_executable (TestTransfer TestTransfer.cpp)
target_link_libraries (
TestTransfer
LINK_PUBLIC
EternalTCP
${Boost_THREAD_LIBRARY}
${Boost_SYSTEM_LIBRARY}
)
add_test(TestTransfer TestTransfer)

add_executable (TestFakeSocketHandler TestFakeSocketHandler.cpp)
target_link_libraries (
TestFakeSocketHandler
LINK_PUBLIC
EternalTCP
${Boost_THREAD_LIBRARY}
${Boost_SYSTEM_LIBRARY}
)
add_test(TestFakeSocketHandler TestFakeSocketHandler)

add_executable (TestBacked TestBacked.cpp)
target_link_libraries (
TestBacked
LINK_PUBLIC
EternalTCP
${Boost_THREAD_LIBRARY}
${Boost_SYSTEM_LIBRARY}
)
add_test(TestBacked TestBacked)

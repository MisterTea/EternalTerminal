add_executable (TestTransfer TestTransfer.cpp)
target_link_libraries (
TestTransfer
LINK_PUBLIC
${PROJECT_NAME}-static
${Boost_SYSTEM_LIBRARY}
${CMAKE_THREAD_LIBS_INIT}
gcrypt
glog
gflags
${COREFOUNDATION_LIBRARY}
)
add_test(TestTransfer TestTransfer)

add_executable (TestFakeSocketHandler TestFakeSocketHandler.cpp)
target_link_libraries (
TestFakeSocketHandler
LINK_PUBLIC
${PROJECT_NAME}-static
${Boost_SYSTEM_LIBRARY}
${CMAKE_THREAD_LIBS_INIT}
gcrypt
glog
gflags
${COREFOUNDATION_LIBRARY}
)
add_test(TestFakeSocketHandler TestFakeSocketHandler)

add_executable (TestBacked TestBacked.cpp)
target_link_libraries (
TestBacked
LINK_PUBLIC
${PROJECT_NAME}-static
${Boost_SYSTEM_LIBRARY}
${CMAKE_THREAD_LIBS_INIT}
gcrypt
glog
gflags
${COREFOUNDATION_LIBRARY}
)
add_test(TestBacked TestBacked)

add_executable (TestConnection TestConnection.cpp)
target_link_libraries (
TestConnection
LINK_PUBLIC
EternalTCP-static
${Boost_THREAD_LIBRARY}
${Boost_SYSTEM_LIBRARY}
${CMAKE_THREAD_LIBS_INIT}
gcrypt
glog
gflags
${COREFOUNDATION_LIBRARY}
)
add_test(TestConnection TestConnection)

add_executable (TestFlakyConnection TestFlakyConnection.cpp)
target_link_libraries (
TestFlakyConnection
LINK_PUBLIC
EternalTCP-static
${Boost_THREAD_LIBRARY}
${Boost_SYSTEM_LIBRARY}
${CMAKE_THREAD_LIBS_INIT}
gcrypt
glog
gflags
${COREFOUNDATION_LIBRARY}
)
add_test(TestFlakyConnection TestFlakyConnection)

add_executable (Terminal Terminal.cpp)
target_link_libraries (
Terminal
LINK_PUBLIC
EternalTCP-static
${Boost_THREAD_LIBRARY}
${Boost_SYSTEM_LIBRARY}
${CMAKE_THREAD_LIBS_INIT}
gcrypt
glog
gflags
${COREFOUNDATION_LIBRARY}
util
)

add_executable (FlakyTerminal FlakyTerminal.cpp)
target_link_libraries (
FlakyTerminal
LINK_PUBLIC
EternalTCP-static
${Boost_THREAD_LIBRARY}
${Boost_SYSTEM_LIBRARY}
${CMAKE_THREAD_LIBS_INIT}
gcrypt
glog
gflags
${COREFOUNDATION_LIBRARY}
)

add_library(
TerminalCommon
STATIC

ConsoleUtils.cpp
${PROTO_SRCS} ${PROTO_HDRS}
)

add_executable (Terminal Terminal.cpp)
target_link_libraries (
Terminal
LINK_PUBLIC
TerminalCommon
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
TerminalCommon
EternalTCP-static
${Boost_THREAD_LIBRARY}
${Boost_SYSTEM_LIBRARY}
${CMAKE_THREAD_LIBS_INIT}
gcrypt
glog
gflags
${COREFOUNDATION_LIBRARY}
)

add_executable (TerminalServer TerminalServer.cpp)
target_link_libraries (
TerminalServer
LINK_PUBLIC
TerminalCommon
EternalTCP-static
${Boost_THREAD_LIBRARY}
${Boost_SYSTEM_LIBRARY}
${CMAKE_THREAD_LIBS_INIT}
${PROTOBUF_LIBRARIES}
gcrypt
glog
gflags
${COREFOUNDATION_LIBRARY}
util
)

add_executable (TerminalClient TerminalClient.cpp)
target_link_libraries (
TerminalClient
LINK_PUBLIC
TerminalCommon
EternalTCP-static
${Boost_THREAD_LIBRARY}
${Boost_SYSTEM_LIBRARY}
${CMAKE_THREAD_LIBS_INIT}
${PROTOBUF_LIBRARIES}
gcrypt
glog
gflags
${COREFOUNDATION_LIBRARY}
util
)

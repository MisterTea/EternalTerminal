
set(SANDBOX_PATH ${PROJECT_SOURCE_DIR}/sandbox)

set (SANDBOX_SRC 
  TestTransfer.cpp
  TestFakeSocketHandler.cpp
  TestConnection.cpp
  TestFlakyConnection.cpp
  )


foreach(SRC_LINE ${SANDBOX_SRC})
  string(REPLACE ".cpp" "" FILE_LINE ${SRC_LINE})
  message(STATUS ${FILE_LINE})
  message(STATUS ${SRC_LINE})

  add_executable(${FILE_LINE} ${SRC_LINE})
  target_link_libraries(
    ${FILE_LINE}
    LINK_PUBLIC
    ${PROJECT_NAME}-static
    ${Boost_SYSTEM_LIBRARY}
    ${CMAKE_THREAD_LIBS_INIT}
    gcrypt
    glog
    gflags
    ${COREFOUNDATION_LIBRARY}
    )
    add_test(${FILE_LINE} ${FILE_LINE})
endforeach()


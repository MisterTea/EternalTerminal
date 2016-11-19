# Create a library called "Hello" which includes the source file "hello.cxx".
# The extension is already found. Any number of sources could be listed here.
add_library (
EternalTCP
STATIC

EternalTCP.cpp
)

# Make sure the compiler can find include files for our Hello library
# when other libraries or executables link to Hello
target_include_directories (EternalTCP PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS EternalTCP DESTINATION lib)
install(FILES EternalTCP.h DESTINATION include)

add_executable (TestTransfer TestTransfer.cpp)

# Link the executable to the Hello library. Since the Hello library has
# public include directories we will use those link directories when building
# helloDemo
target_link_libraries (TestTransfer LINK_PUBLIC EternalTCP)


add_test(TestTransfer TestTransfer)

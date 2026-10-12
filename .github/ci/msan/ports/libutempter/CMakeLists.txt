cmake_minimum_required(VERSION 3.16)
project(MSanUtempter LANGUAGES C)
# The library is instrumented; the already-installed privileged helper remains
# a separate executable with its distro ownership and permissions.
add_library(utempter STATIC iface.c)
target_compile_definitions(utempter PRIVATE LIBEXECDIR="${HELPER_LIBEXEC_DIR}")
set_target_properties(utempter PROPERTIES C_STANDARD 99)
install(TARGETS utempter ARCHIVE DESTINATION lib)
install(FILES utempter.h DESTINATION include)

get_filename_component(GDK_DIR "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\GDK;InstallPath]" ABSOLUTE CACHE)

if(NOT GDK_VERSION)
    FILE(GLOB subfolders RELATIVE "${GDK_DIR}" "${GDK_DIR}/*")
    list(FILTER subfolders INCLUDE REGEX "[0-9]+")
    list(SORT subfolders COMPARE NATURAL) # sort by version number
    list(GET subfolders -1 GDK_VERSION) # GET -1 is last element, i.e. highest version
endif()

message(STATUS "Using GDK version ${GDK_VERSION} at ${GDK_DIR}")
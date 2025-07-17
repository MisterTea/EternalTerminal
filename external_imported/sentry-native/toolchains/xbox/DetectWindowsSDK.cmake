get_filename_component(WINDOWS_SDK "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE CACHE)

if(NOT WINDOWS_SDK_VER)
    file(GLOB subfolders RELATIVE "${WINDOWS_SDK}/Lib" "${WINDOWS_SDK}/Lib/*")
    list(SORT subfolders COMPARE NATURAL) # sort by version number
    list(GET subfolders -1 WINDOWS_SDK_VER) # GET -1 is last element, i.e. highest version
endif ()

message(STATUS "Using Windows SDK version ${WINDOWS_SDK_VER} at ${WINDOWS_SDK}")
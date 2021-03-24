project(unwindstack LANGUAGES C CXX ASM)
cmake_minimum_required(VERSION 3.10)

set(UNWINDSTACK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../)

include_directories(${UNWINDSTACK_ROOT} ${UNWINDSTACK_ROOT}/include)

set(ANDROID_SOURCES
    ${UNWINDSTACK_ROOT}/android-base/errno_restorer.h
    ${UNWINDSTACK_ROOT}/android-base/file.cpp
    ${UNWINDSTACK_ROOT}/android-base/file.h
    ${UNWINDSTACK_ROOT}/android-base/log_main.h
    ${UNWINDSTACK_ROOT}/android-base/logging.h
    ${UNWINDSTACK_ROOT}/android-base/macros.h
    ${UNWINDSTACK_ROOT}/android-base/off64_t.h
    ${UNWINDSTACK_ROOT}/android-base/parseint.h
    ${UNWINDSTACK_ROOT}/android-base/stringprintf.cpp
    ${UNWINDSTACK_ROOT}/android-base/stringprintf.h
    ${UNWINDSTACK_ROOT}/android-base/strings.cpp
    ${UNWINDSTACK_ROOT}/android-base/strings.h
    ${UNWINDSTACK_ROOT}/android-base/threads.h
    ${UNWINDSTACK_ROOT}/android-base/unique_fd.h
    ${UNWINDSTACK_ROOT}/android-base/utf8.h
    ${UNWINDSTACK_ROOT}/procinfo/process_map.h
    ${UNWINDSTACK_ROOT}/procinfo/process.h
)

set(UNWINDSTACK_SOURCES
    ${UNWINDSTACK_ROOT}/ArmExidx.cpp
    ${UNWINDSTACK_ROOT}/ArmExidx.h
    ${UNWINDSTACK_ROOT}/Check.h
    #${UNWINDSTACK_ROOT}/DexFile.cpp
    #${UNWINDSTACK_ROOT}/DexFile.h
    ${UNWINDSTACK_ROOT}/DexFiles.cpp
    ${UNWINDSTACK_ROOT}/DwarfCfa.cpp
    ${UNWINDSTACK_ROOT}/DwarfCfa.h
    ${UNWINDSTACK_ROOT}/DwarfDebugFrame.h
    ${UNWINDSTACK_ROOT}/DwarfEhFrame.h
    ${UNWINDSTACK_ROOT}/DwarfEhFrameWithHdr.cpp
    ${UNWINDSTACK_ROOT}/DwarfEhFrameWithHdr.h
    ${UNWINDSTACK_ROOT}/DwarfEncoding.h
    ${UNWINDSTACK_ROOT}/DwarfMemory.cpp
    ${UNWINDSTACK_ROOT}/DwarfOp.cpp
    ${UNWINDSTACK_ROOT}/DwarfOp.h
    ${UNWINDSTACK_ROOT}/DwarfSection.cpp
    ${UNWINDSTACK_ROOT}/Elf.cpp
    ${UNWINDSTACK_ROOT}/ElfInterface.cpp
    ${UNWINDSTACK_ROOT}/ElfInterfaceArm.cpp
    ${UNWINDSTACK_ROOT}/ElfInterfaceArm.h
    ${UNWINDSTACK_ROOT}/Global.cpp
    ${UNWINDSTACK_ROOT}/JitDebug.cpp
    ${UNWINDSTACK_ROOT}/LocalUnwinder.cpp
    ${UNWINDSTACK_ROOT}/Log.cpp
    ${UNWINDSTACK_ROOT}/MapInfo.cpp
    ${UNWINDSTACK_ROOT}/Maps.cpp
    ${UNWINDSTACK_ROOT}/Memory.cpp
    ${UNWINDSTACK_ROOT}/MemoryBuffer.h
    ${UNWINDSTACK_ROOT}/MemoryCache.h
    ${UNWINDSTACK_ROOT}/MemoryFileAtOffset.h
    ${UNWINDSTACK_ROOT}/MemoryLocal.h
    ${UNWINDSTACK_ROOT}/MemoryMte.cpp
    ${UNWINDSTACK_ROOT}/MemoryOffline.h
    ${UNWINDSTACK_ROOT}/MemoryOfflineBuffer.h
    ${UNWINDSTACK_ROOT}/MemoryRange.h
    ${UNWINDSTACK_ROOT}/MemoryRemote.h
    ${UNWINDSTACK_ROOT}/Regs.cpp
    ${UNWINDSTACK_ROOT}/RegsArm.cpp
    ${UNWINDSTACK_ROOT}/RegsArm64.cpp
    ${UNWINDSTACK_ROOT}/RegsInfo.h
    ${UNWINDSTACK_ROOT}/RegsX86_64.cpp
    ${UNWINDSTACK_ROOT}/RegsX86.cpp
    ${UNWINDSTACK_ROOT}/Symbols.cpp
    ${UNWINDSTACK_ROOT}/Symbols.h
    ${UNWINDSTACK_ROOT}/ThreadEntry.cpp
    ${UNWINDSTACK_ROOT}/ThreadEntry.h
    ${UNWINDSTACK_ROOT}/ThreadUnwinder.cpp
    ${UNWINDSTACK_ROOT}/unistdfix.h
    ${UNWINDSTACK_ROOT}/Unwinder.cpp
)

if(${CMAKE_SYSTEM_PROCESSOR} MATCHES arm)
elseif(${CMAKE_SYSTEM_PROCESSOR} MATCHES "amd64.*|x86_64.*|AMD64.*")
    set(UNWINDSTACK_SOURCES_GETREG
        ${UNWINDSTACK_ROOT}/AsmGetRegsX86_64.S
    )
elseif(${CMAKE_SYSTEM_PROCESSOR} MATCHES "i686.*|i386.*|x86.*")
    set(UNWINDSTACK_SOURCES_GETREG
        ${UNWINDSTACK_ROOT}/AsmGetRegsX86.S
    )
else()
    add_definitions(-DEM_ARM=40)
endif()

add_library(unwindstack STATIC
    ${ANDROID_SOURCES}
    ${UNWINDSTACK_SOURCES}
    ${UNWINDSTACK_SOURCES_GETREG}
)
target_link_libraries(unwindstack log)
set_property(TARGET unwindstack PROPERTY CXX_STANDARD 17)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(unwindstack PRIVATE $<BUILD_INTERFACE:-Wno-unknown-attributes>)
endif()

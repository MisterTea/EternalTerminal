# CMakeLists.txt for vendored libunwind (Linux only; x86_64, aarch64, x86, arm)
# Builds the local unwinding library (libunwind.a equivalent).
# When LIBUNWIND_BUILD_REMOTE is set, also builds unwind_remote with
# generic (G-prefix) sources and ptrace accessors for remote unwinding.

# Vendored libunwind - built as a subdirectory of the main project.
# Enable ASM for the .S sources without a nested project() call.
include(CheckLanguage)
check_language(ASM)
if(NOT CMAKE_ASM_COMPILER)
    message(FATAL_ERROR "ASM required for vendored libunwind's context code, but no assembler found.")
endif()
enable_language(ASM)

set(LIBUNWIND_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(LIBUNWIND_INC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Detect architecture
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(UNWIND_ARCH "x86_64")
    set(UNWIND_ELF "elf64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
    set(UNWIND_ARCH "aarch64")
    set(UNWIND_ELF "elf64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86|x86")
    set(UNWIND_ARCH "x86")
    set(UNWIND_ELF "elf32")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)")
    set(UNWIND_ARCH "arm")
    set(UNWIND_ELF "elf32")
else()
    message(FATAL_ERROR "Unsupported architecture for vendored libunwind: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# Common sources (libunwind_la_SOURCES_common + os sources for Linux)
set(UNWIND_COMMON_SOURCES
    ${LIBUNWIND_SRC}/os-linux.c
    ${LIBUNWIND_SRC}/dl-iterate-phdr.c
    ${LIBUNWIND_SRC}/mi/init.c
    ${LIBUNWIND_SRC}/mi/flush_cache.c
    ${LIBUNWIND_SRC}/mi/mempool.c
    ${LIBUNWIND_SRC}/mi/strerror.c
)

# Local sources (libunwind_la_SOURCES_local_nounwind + os_local)
set(UNWIND_LOCAL_SOURCES
    # local nounwind
    ${LIBUNWIND_SRC}/mi/backtrace.c
    ${LIBUNWIND_SRC}/mi/dyn-cancel.c
    ${LIBUNWIND_SRC}/mi/dyn-info-list.c
    ${LIBUNWIND_SRC}/mi/dyn-register.c
    ${LIBUNWIND_SRC}/mi/Laddress_validator.c
    ${LIBUNWIND_SRC}/mi/Ldestroy_addr_space.c
    ${LIBUNWIND_SRC}/mi/Ldyn-extract.c
    ${LIBUNWIND_SRC}/mi/Lfind_dynamic_proc_info.c
    ${LIBUNWIND_SRC}/mi/Lget_accessors.c
    ${LIBUNWIND_SRC}/mi/Lget_fpreg.c
    ${LIBUNWIND_SRC}/mi/Lset_fpreg.c
    ${LIBUNWIND_SRC}/mi/Lget_proc_info_by_ip.c
    ${LIBUNWIND_SRC}/mi/Lget_proc_name.c
    ${LIBUNWIND_SRC}/mi/Lget_reg.c
    ${LIBUNWIND_SRC}/mi/Lis_plt_entry.c
    ${LIBUNWIND_SRC}/mi/Lput_dynamic_unwind_info.c
    ${LIBUNWIND_SRC}/mi/Lset_cache_size.c
    ${LIBUNWIND_SRC}/mi/Lset_caching_policy.c
    ${LIBUNWIND_SRC}/mi/Lset_iterate_phdr_function.c
    ${LIBUNWIND_SRC}/mi/Lset_reg.c
    ${LIBUNWIND_SRC}/mi/Lget_elf_filename.c
    # NOTE: The src/unwind/ sources (C++ exception ABI: _Unwind_Resume,
    # _Unwind_RaiseException, etc.) are intentionally excluded. They conflict
    # with libgcc_eh.a when linking with -static-libgcc, and sentry-native
    # only needs the local unwinding API (_UL*_init_local, _UL*_step, etc.).
)

# DWARF sources (local)
set(UNWIND_DWARF_LOCAL_SOURCES
    ${LIBUNWIND_SRC}/dwarf/global.c
    ${LIBUNWIND_SRC}/dwarf/Lexpr.c
    ${LIBUNWIND_SRC}/dwarf/Lfde.c
    ${LIBUNWIND_SRC}/dwarf/Lfind_proc_info-lsb.c
    ${LIBUNWIND_SRC}/dwarf/Lfind_unwind_table.c
    ${LIBUNWIND_SRC}/dwarf/Lget_proc_info_in_range.c
    ${LIBUNWIND_SRC}/dwarf/Lparser.c
    ${LIBUNWIND_SRC}/dwarf/Lpe.c
)

# ELF sources
set(UNWIND_ELF_SOURCES
    ${LIBUNWIND_SRC}/${UNWIND_ELF}.c
)

# Architecture-specific sources
if(UNWIND_ARCH STREQUAL "x86_64")
    set(UNWIND_ARCH_SOURCES
        # common
        ${LIBUNWIND_SRC}/x86_64/is_fpreg.c
        ${LIBUNWIND_SRC}/x86_64/regname.c
        # os_local (Linux)
        ${LIBUNWIND_SRC}/x86_64/Los-linux.c
        # arch-specific local
        ${LIBUNWIND_SRC}/x86_64/getcontext.S
        ${LIBUNWIND_SRC}/x86_64/Lapply_reg_state.c
        ${LIBUNWIND_SRC}/x86_64/Lcreate_addr_space.c
        ${LIBUNWIND_SRC}/x86_64/Lget_proc_info.c
        ${LIBUNWIND_SRC}/x86_64/Lget_save_loc.c
        ${LIBUNWIND_SRC}/x86_64/Lglobal.c
        ${LIBUNWIND_SRC}/x86_64/Linit.c
        ${LIBUNWIND_SRC}/x86_64/Linit_local.c
        ${LIBUNWIND_SRC}/x86_64/Linit_remote.c
        ${LIBUNWIND_SRC}/x86_64/Lregs.c
        ${LIBUNWIND_SRC}/x86_64/Lreg_states_iterate.c
        ${LIBUNWIND_SRC}/x86_64/Lresume.c
        ${LIBUNWIND_SRC}/x86_64/Lstash_frame.c
        ${LIBUNWIND_SRC}/x86_64/Lstep.c
        ${LIBUNWIND_SRC}/x86_64/Ltrace.c
        ${LIBUNWIND_SRC}/x86_64/setcontext.S
    )
elseif(UNWIND_ARCH STREQUAL "aarch64")
    set(UNWIND_ARCH_SOURCES
        # common
        ${LIBUNWIND_SRC}/aarch64/is_fpreg.c
        ${LIBUNWIND_SRC}/aarch64/regname.c
        # os_local (Linux)
        ${LIBUNWIND_SRC}/aarch64/Los-linux.c
        # arch-specific local
        ${LIBUNWIND_SRC}/aarch64/getcontext.S
        ${LIBUNWIND_SRC}/aarch64/Lapply_reg_state.c
        ${LIBUNWIND_SRC}/aarch64/Lcreate_addr_space.c
        ${LIBUNWIND_SRC}/aarch64/Lget_proc_info.c
        ${LIBUNWIND_SRC}/aarch64/Lget_save_loc.c
        ${LIBUNWIND_SRC}/aarch64/Lglobal.c
        ${LIBUNWIND_SRC}/aarch64/Linit.c
        ${LIBUNWIND_SRC}/aarch64/Linit_local.c
        ${LIBUNWIND_SRC}/aarch64/Linit_remote.c
        ${LIBUNWIND_SRC}/aarch64/Lis_signal_frame.c
        ${LIBUNWIND_SRC}/aarch64/Lregs.c
        ${LIBUNWIND_SRC}/aarch64/Lreg_states_iterate.c
        ${LIBUNWIND_SRC}/aarch64/Lresume.c
        ${LIBUNWIND_SRC}/aarch64/Lstash_frame.c
        ${LIBUNWIND_SRC}/aarch64/Lstep.c
        ${LIBUNWIND_SRC}/aarch64/Ltrace.c
        ${LIBUNWIND_SRC}/aarch64/Lstrip_ptrauth_insn_mask.c
    )
elseif(UNWIND_ARCH STREQUAL "x86")
    set(UNWIND_ARCH_SOURCES
        # common
        ${LIBUNWIND_SRC}/x86/is_fpreg.c
        ${LIBUNWIND_SRC}/x86/regname.c
        # os_local (Linux)
        ${LIBUNWIND_SRC}/x86/Los-linux.c
        ${LIBUNWIND_SRC}/x86/getcontext-linux.S
        # arch-specific local
        ${LIBUNWIND_SRC}/x86/Lapply_reg_state.c
        ${LIBUNWIND_SRC}/x86/Lcreate_addr_space.c
        ${LIBUNWIND_SRC}/x86/Lget_proc_info.c
        ${LIBUNWIND_SRC}/x86/Lget_save_loc.c
        ${LIBUNWIND_SRC}/x86/Lglobal.c
        ${LIBUNWIND_SRC}/x86/Linit.c
        ${LIBUNWIND_SRC}/x86/Linit_local.c
        ${LIBUNWIND_SRC}/x86/Linit_remote.c
        ${LIBUNWIND_SRC}/x86/Lregs.c
        ${LIBUNWIND_SRC}/x86/Lreg_states_iterate.c
        ${LIBUNWIND_SRC}/x86/Lresume.c
        ${LIBUNWIND_SRC}/x86/Lstep.c
    )
elseif(UNWIND_ARCH STREQUAL "arm")
    set(UNWIND_ARCH_SOURCES
        # common
        ${LIBUNWIND_SRC}/arm/is_fpreg.c
        ${LIBUNWIND_SRC}/arm/regname.c
        # os_local (Linux)
        ${LIBUNWIND_SRC}/arm/Los-linux.c
        # arch-specific local
        ${LIBUNWIND_SRC}/arm/getcontext.S
        ${LIBUNWIND_SRC}/arm/Lapply_reg_state.c
        ${LIBUNWIND_SRC}/arm/Lcreate_addr_space.c
        ${LIBUNWIND_SRC}/arm/Lex_tables.c
        ${LIBUNWIND_SRC}/arm/Lget_proc_info.c
        ${LIBUNWIND_SRC}/arm/Lget_save_loc.c
        ${LIBUNWIND_SRC}/arm/Lglobal.c
        ${LIBUNWIND_SRC}/arm/Linit.c
        ${LIBUNWIND_SRC}/arm/Linit_local.c
        ${LIBUNWIND_SRC}/arm/Linit_remote.c
        ${LIBUNWIND_SRC}/arm/Lregs.c
        ${LIBUNWIND_SRC}/arm/Lreg_states_iterate.c
        ${LIBUNWIND_SRC}/arm/Lresume.c
        ${LIBUNWIND_SRC}/arm/Lstash_frame.c
        ${LIBUNWIND_SRC}/arm/Lstep.c
        ${LIBUNWIND_SRC}/arm/Ltrace.c
    )
endif()

add_library(unwind STATIC
    ${UNWIND_COMMON_SOURCES}
    ${UNWIND_LOCAL_SOURCES}
    ${UNWIND_DWARF_LOCAL_SOURCES}
    ${UNWIND_ELF_SOURCES}
    ${UNWIND_ARCH_SOURCES}
)

# Generate config.h
include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckStructHasMember)
include(CheckFunctionExists)

check_include_file("asm/ptrace.h" HAVE_ASM_PTRACE_H)
check_include_file("byteswap.h" HAVE_BYTESWAP_H)
check_include_file("elf.h" HAVE_ELF_H)
check_include_file("endian.h" HAVE_ENDIAN_H)
check_include_file("execinfo.h" HAVE_EXECINFO_H)
check_include_file("link.h" HAVE_LINK_H)
check_include_file("signal.h" HAVE_SIGNAL_H)
check_include_file("sys/ptrace.h" HAVE_SYS_PTRACE_H)
check_include_file("sys/syscall.h" HAVE_SYS_SYSCALL_H)
check_include_file("sys/types.h" HAVE_SYS_TYPES_H)
check_include_file("sys/stat.h" HAVE_SYS_STAT_H)
check_include_file("sys/param.h" HAVE_SYS_PARAM_H)
check_include_file("sys/procfs.h" HAVE_SYS_PROCFS_H)
check_function_exists(dl_iterate_phdr HAVE_DL_ITERATE_PHDR)
check_function_exists(mincore HAVE_MINCORE)
check_function_exists(pipe2 HAVE_PIPE2)
check_function_exists(sigaltstack HAVE_SIGALTSTACK)

check_struct_has_member("struct dl_phdr_info" dlpi_subs "link.h" HAVE_STRUCT_DL_PHDR_INFO_DLPI_SUBS)

# Check for PTRACE declarations
include(CheckCSourceCompiles)
foreach(_ptrace_sym PTRACE_CONT PTRACE_POKEDATA PTRACE_POKEUSER PTRACE_SETREGSET PTRACE_SINGLESTEP PTRACE_SYSCALL PTRACE_TRACEME)
    check_c_source_compiles("
        #include <sys/ptrace.h>
        int main() { int x = ${_ptrace_sym}; return x; }
    " HAVE_DECL_${_ptrace_sym}_VAL)
    if(HAVE_DECL_${_ptrace_sym}_VAL)
        set(HAVE_DECL_${_ptrace_sym} 1)
    else()
        set(HAVE_DECL_${_ptrace_sym} 0)
    endif()
endforeach()

# Check for PT declarations (BSD)
foreach(_pt_sym PT_CONTINUE PT_GETFPREGS PT_GETREGS PT_IO PT_STEP PT_SYSCALL PT_TRACE_ME)
    check_c_source_compiles("
        #include <sys/ptrace.h>
        int main() { int x = ${_pt_sym}; return x; }
    " HAVE_DECL_${_pt_sym}_VAL)
    if(HAVE_DECL_${_pt_sym}_VAL)
        set(HAVE_DECL_${_pt_sym} 1)
    else()
        set(HAVE_DECL_${_pt_sym} 0)
    endif()
endforeach()

# Configure config.h
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/config.h"
)

# Configure libunwind-common.h
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libunwind-common.h.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/libunwind-common.h"
)

target_include_directories(unwind
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/include
        ${LIBUNWIND_INC}
        ${LIBUNWIND_SRC}
    PUBLIC
        $<BUILD_INTERFACE:${LIBUNWIND_INC}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
)

# tdep symlinks/includes - libunwind uses tdep-<arch> as tdep
if(UNWIND_ARCH STREQUAL "x86_64")
    target_include_directories(unwind PRIVATE ${LIBUNWIND_INC}/tdep-x86_64)
    target_compile_definitions(unwind PRIVATE UNW_TARGET_X86_64=1)
elseif(UNWIND_ARCH STREQUAL "aarch64")
    target_include_directories(unwind PRIVATE ${LIBUNWIND_INC}/tdep-aarch64)
    target_compile_definitions(unwind PRIVATE UNW_TARGET_AARCH64=1)
elseif(UNWIND_ARCH STREQUAL "x86")
    target_include_directories(unwind PRIVATE ${LIBUNWIND_INC}/tdep-x86)
    target_compile_definitions(unwind PRIVATE UNW_TARGET_X86=1)
elseif(UNWIND_ARCH STREQUAL "arm")
    target_include_directories(unwind PRIVATE ${LIBUNWIND_INC}/tdep-arm)
    target_compile_definitions(unwind PRIVATE UNW_TARGET_ARM=1)
endif()

target_compile_definitions(unwind PRIVATE
    HAVE_CONFIG_H
    _GNU_SOURCE
)

set_target_properties(unwind PROPERTIES
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON
)

# Suppress warnings in vendored code
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(unwind PRIVATE -w)
endif()

target_link_libraries(unwind PRIVATE ${CMAKE_DL_LIBS} pthread)

# --- Remote unwinding support (optional, for native backend daemon) ---
if(LIBUNWIND_BUILD_REMOTE)

# Generic (G-prefix) sources — compiled WITHOUT UNW_LOCAL_ONLY.
# Produce _U<arch>_ symbols that coexist with the _UL<arch>_ local symbols.
set(UNWIND_GENERIC_MI_SOURCES
    ${LIBUNWIND_SRC}/mi/Gaddress_validator.c
    ${LIBUNWIND_SRC}/mi/Gdestroy_addr_space.c
    ${LIBUNWIND_SRC}/mi/Gdyn-extract.c
    ${LIBUNWIND_SRC}/mi/Gdyn-remote.c
    ${LIBUNWIND_SRC}/mi/Gfind_dynamic_proc_info.c
    ${LIBUNWIND_SRC}/mi/Gget_accessors.c
    ${LIBUNWIND_SRC}/mi/Gget_elf_filename.c
    ${LIBUNWIND_SRC}/mi/Gget_fpreg.c
    ${LIBUNWIND_SRC}/mi/Gget_proc_info_by_ip.c
    ${LIBUNWIND_SRC}/mi/Gget_proc_name.c
    ${LIBUNWIND_SRC}/mi/Gget_reg.c
    ${LIBUNWIND_SRC}/mi/Gis_plt_entry.c
    ${LIBUNWIND_SRC}/mi/Gput_dynamic_unwind_info.c
    ${LIBUNWIND_SRC}/mi/Gset_cache_size.c
    ${LIBUNWIND_SRC}/mi/Gset_caching_policy.c
    ${LIBUNWIND_SRC}/mi/Gset_fpreg.c
    ${LIBUNWIND_SRC}/mi/Gset_iterate_phdr_function.c
    ${LIBUNWIND_SRC}/mi/Gset_reg.c
)

set(UNWIND_GENERIC_DWARF_SOURCES
    ${LIBUNWIND_SRC}/dwarf/Gexpr.c
    ${LIBUNWIND_SRC}/dwarf/Gfde.c
    ${LIBUNWIND_SRC}/dwarf/Gfind_proc_info-lsb.c
    ${LIBUNWIND_SRC}/dwarf/Gfind_unwind_table.c
    ${LIBUNWIND_SRC}/dwarf/Gget_proc_info_in_range.c
    ${LIBUNWIND_SRC}/dwarf/Gparser.c
    ${LIBUNWIND_SRC}/dwarf/Gpe.c
)

if(UNWIND_ARCH STREQUAL "x86_64")
    set(UNWIND_GENERIC_ARCH_SOURCES
        ${LIBUNWIND_SRC}/x86_64/Gos-linux.c
        ${LIBUNWIND_SRC}/x86_64/Gapply_reg_state.c
        ${LIBUNWIND_SRC}/x86_64/Gcreate_addr_space.c
        ${LIBUNWIND_SRC}/x86_64/Gget_proc_info.c
        ${LIBUNWIND_SRC}/x86_64/Gget_save_loc.c
        ${LIBUNWIND_SRC}/x86_64/Gglobal.c
        ${LIBUNWIND_SRC}/x86_64/Ginit.c
        ${LIBUNWIND_SRC}/x86_64/Ginit_local.c
        ${LIBUNWIND_SRC}/x86_64/Ginit_remote.c
        ${LIBUNWIND_SRC}/x86_64/Gregs.c
        ${LIBUNWIND_SRC}/x86_64/Greg_states_iterate.c
        ${LIBUNWIND_SRC}/x86_64/Gresume.c
        ${LIBUNWIND_SRC}/x86_64/Gstash_frame.c
        ${LIBUNWIND_SRC}/x86_64/Gstep.c
        ${LIBUNWIND_SRC}/x86_64/Gtrace.c
    )
elseif(UNWIND_ARCH STREQUAL "aarch64")
    set(UNWIND_GENERIC_ARCH_SOURCES
        ${LIBUNWIND_SRC}/aarch64/Gos-linux.c
        ${LIBUNWIND_SRC}/aarch64/Gapply_reg_state.c
        ${LIBUNWIND_SRC}/aarch64/Gcreate_addr_space.c
        ${LIBUNWIND_SRC}/aarch64/Gget_proc_info.c
        ${LIBUNWIND_SRC}/aarch64/Gget_save_loc.c
        ${LIBUNWIND_SRC}/aarch64/Gglobal.c
        ${LIBUNWIND_SRC}/aarch64/Ginit.c
        ${LIBUNWIND_SRC}/aarch64/Ginit_local.c
        ${LIBUNWIND_SRC}/aarch64/Ginit_remote.c
        ${LIBUNWIND_SRC}/aarch64/Gis_signal_frame.c
        ${LIBUNWIND_SRC}/aarch64/Gregs.c
        ${LIBUNWIND_SRC}/aarch64/Greg_states_iterate.c
        ${LIBUNWIND_SRC}/aarch64/Gresume.c
        ${LIBUNWIND_SRC}/aarch64/Gstash_frame.c
        ${LIBUNWIND_SRC}/aarch64/Gstep.c
        ${LIBUNWIND_SRC}/aarch64/Gtrace.c
        ${LIBUNWIND_SRC}/aarch64/Gstrip_ptrauth_insn_mask.c
    )
elseif(UNWIND_ARCH STREQUAL "x86")
    set(UNWIND_GENERIC_ARCH_SOURCES
        ${LIBUNWIND_SRC}/x86/Gos-linux.c
        ${LIBUNWIND_SRC}/x86/Gapply_reg_state.c
        ${LIBUNWIND_SRC}/x86/Gcreate_addr_space.c
        ${LIBUNWIND_SRC}/x86/Gget_proc_info.c
        ${LIBUNWIND_SRC}/x86/Gget_save_loc.c
        ${LIBUNWIND_SRC}/x86/Gglobal.c
        ${LIBUNWIND_SRC}/x86/Ginit.c
        ${LIBUNWIND_SRC}/x86/Ginit_local.c
        ${LIBUNWIND_SRC}/x86/Ginit_remote.c
        ${LIBUNWIND_SRC}/x86/Gregs.c
        ${LIBUNWIND_SRC}/x86/Greg_states_iterate.c
        ${LIBUNWIND_SRC}/x86/Gresume.c
        ${LIBUNWIND_SRC}/x86/Gstep.c
    )
elseif(UNWIND_ARCH STREQUAL "arm")
    set(UNWIND_GENERIC_ARCH_SOURCES
        ${LIBUNWIND_SRC}/arm/Gos-linux.c
        ${LIBUNWIND_SRC}/arm/Gapply_reg_state.c
        ${LIBUNWIND_SRC}/arm/Gcreate_addr_space.c
        ${LIBUNWIND_SRC}/arm/Gex_tables.c
        ${LIBUNWIND_SRC}/arm/Gget_proc_info.c
        ${LIBUNWIND_SRC}/arm/Gget_save_loc.c
        ${LIBUNWIND_SRC}/arm/Gglobal.c
        ${LIBUNWIND_SRC}/arm/Ginit.c
        ${LIBUNWIND_SRC}/arm/Ginit_local.c
        ${LIBUNWIND_SRC}/arm/Ginit_remote.c
        ${LIBUNWIND_SRC}/arm/Gregs.c
        ${LIBUNWIND_SRC}/arm/Greg_states_iterate.c
        ${LIBUNWIND_SRC}/arm/Gresume.c
        ${LIBUNWIND_SRC}/arm/Gstash_frame.c
        ${LIBUNWIND_SRC}/arm/Gstep.c
        ${LIBUNWIND_SRC}/arm/Gtrace.c
    )
endif()

# Ptrace accessor library (_UPT_* functions for remote unwinding via ptrace)
set(UNWIND_PTRACE_SOURCES
    ${LIBUNWIND_SRC}/ptrace/_UPT_accessors.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_access_fpreg.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_access_mem.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_access_reg.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_create.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_destroy.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_elf.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_find_proc_info.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_get_dyn_info_list_addr.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_get_elf_filename.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_get_proc_name.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_ptrauth_insn_mask.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_put_unwind_info.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_reg_offset.c
    ${LIBUNWIND_SRC}/ptrace/_UPT_resume.c
)

# unwind_remote: superset of unwind (local + generic + ptrace)
add_library(unwind_remote STATIC
    ${UNWIND_COMMON_SOURCES}
    ${UNWIND_LOCAL_SOURCES}
    ${UNWIND_DWARF_LOCAL_SOURCES}
    ${UNWIND_ELF_SOURCES}
    ${UNWIND_ARCH_SOURCES}
    ${UNWIND_GENERIC_MI_SOURCES}
    ${UNWIND_GENERIC_DWARF_SOURCES}
    ${UNWIND_GENERIC_ARCH_SOURCES}
    ${UNWIND_PTRACE_SOURCES}
)

target_include_directories(unwind_remote
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/include
        ${LIBUNWIND_INC}
        ${LIBUNWIND_SRC}
    PUBLIC
        $<BUILD_INTERFACE:${LIBUNWIND_INC}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
)

if(UNWIND_ARCH STREQUAL "x86_64")
    target_include_directories(unwind_remote PRIVATE ${LIBUNWIND_INC}/tdep-x86_64)
    target_compile_definitions(unwind_remote PRIVATE UNW_TARGET_X86_64=1)
elseif(UNWIND_ARCH STREQUAL "aarch64")
    target_include_directories(unwind_remote PRIVATE ${LIBUNWIND_INC}/tdep-aarch64)
    target_compile_definitions(unwind_remote PRIVATE UNW_TARGET_AARCH64=1)
elseif(UNWIND_ARCH STREQUAL "x86")
    target_include_directories(unwind_remote PRIVATE ${LIBUNWIND_INC}/tdep-x86)
    target_compile_definitions(unwind_remote PRIVATE UNW_TARGET_X86=1)
elseif(UNWIND_ARCH STREQUAL "arm")
    target_include_directories(unwind_remote PRIVATE ${LIBUNWIND_INC}/tdep-arm)
    target_compile_definitions(unwind_remote PRIVATE UNW_TARGET_ARM=1)
endif()

target_compile_definitions(unwind_remote PRIVATE
    HAVE_CONFIG_H
    _GNU_SOURCE
)

set_target_properties(unwind_remote PROPERTIES
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(unwind_remote PRIVATE -w)
endif()

target_link_libraries(unwind_remote PRIVATE ${CMAKE_DL_LIBS} pthread)

endif() # LIBUNWIND_BUILD_REMOTE

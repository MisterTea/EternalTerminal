# Copyright 2006 Google LLC
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google LLC nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


AC_PREREQ([2.71])

AC_INIT([breakpad],[0.1],[google-breakpad-dev@googlegroups.com])
dnl Sanity check: the argument is just a file that should exist.
AC_CONFIG_SRCDIR(README.md)
AC_CONFIG_AUX_DIR(autotools)
AC_CONFIG_MACRO_DIR([m4])
AC_CANONICAL_HOST

AM_INIT_AUTOMAKE(subdir-objects tar-ustar 1.13)
AC_CONFIG_HEADERS(src/config.h)
AM_MAINTAINER_MODE

AM_PROG_AR
AM_PROG_AS
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_CPP
AC_PROG_CXX
AC_PROG_RANLIB

dnl This must come before all the feature tests below.
AC_ARG_ENABLE(m32,
              AS_HELP_STRING([--enable-m32],
                             [Compile/build with -m32]
                             [(default is no)]),,
              [enable_m32=no])
if test "x$enable_m32" = xyes; then
  CFLAGS="${CFLAGS} -m32"
  CXXFLAGS="${CXXFLAGS} -m32"
fi

AC_SYS_LARGEFILE
AX_PTHREAD
AC_CHECK_HEADERS([a.out.h sys/mman.h sys/random.h])
AC_CHECK_FUNCS([arc4random getcontext getrandom memfd_create])
AM_CONDITIONAL([HAVE_GETCONTEXT], [test "x$ac_cv_func_getcontext" = xyes])
AM_CONDITIONAL([HAVE_MEMFD_CREATE], [test "x$ac_cv_func_memfd_create" = xyes])

AX_CXX_COMPILE_STDCXX(17, , mandatory)

dnl Test supported warning flags.
WARN_CXXFLAGS=
dnl This warning flag is used by clang.  Its default behavior is to warn when
dnl given an unknown flag rather than error out.
AC_LANG_PUSH([C++])
AX_CHECK_COMPILE_FLAG([-Werror=unknown-warning-option],[
  ax_compiler_flags_test="-Werror=unknown-warning-option"
],[
  ax_compiler_flags_test=""
])
AX_APPEND_COMPILE_FLAGS(m4_flatten([
  -Wmissing-braces
  -Wnon-virtual-dtor
  -Woverloaded-virtual
  -Wreorder
  -Wsign-compare
  -Wunused-local-typedefs
  -Wunused-variable
  -Wvla
]), [WARN_CXXFLAGS], [${ax_compiler_flags_test}])
AS_VAR_APPEND([WARN_CXXFLAGS], " -Werror")
AC_LANG_POP([C++])
AC_SUBST([WARN_CXXFLAGS])

dnl Test support for O_CLOEXEC
AX_CHECK_DEFINE([fcntl.h], [O_CLOEXEC], [],
                [AC_DEFINE([O_CLOEXEC], [0], [Fallback definition for old systems])])

# Only build Linux client libs when compiling for Linux
case $host in
  *-*-linux* | *-android* )
    LINUX_HOST=true
    ;;
esac
AM_CONDITIONAL(LINUX_HOST, test x$LINUX_HOST = xtrue)

# Only use Android support headers when compiling for Android
case $host in
  *-android*)
    ANDROID_HOST=true
    ;;
esac
AM_CONDITIONAL(ANDROID_HOST, test x$ANDROID_HOST = xtrue)

# Some tools (like mac ones) only support x86 currently.
case $host_cpu in
  i?86|x86_64)
    X86_HOST=true
    ;;
esac
AM_CONDITIONAL(X86_HOST, test x$X86_HOST = xtrue)

AC_ARG_ENABLE(processor,
              AS_HELP_STRING([--disable-processor],
                             [Don't build processor library]
                             [(default is no)]),,
              [enable_processor=yes])
AM_CONDITIONAL(DISABLE_PROCESSOR, test "x$enable_processor" != xyes)

AC_ARG_ENABLE(tools,
              AS_HELP_STRING([--disable-tools],
                             [Don't build tool binaries]
                             [(default is no)]),,
              [enable_tools=yes])
AM_CONDITIONAL(DISABLE_TOOLS, test "x$enable_tools" != xyes)

if test x$LINUX_HOST = xfalse -a "x$enable_processor" != xyes -a "x$enable_tools" != xyes; then
  AC_MSG_ERROR([--disable-processor and --disable-tools were specified, and not building for Linux. Nothing to build!])
fi

AC_ARG_ENABLE(system-test-libs,
              AS_HELP_STRING([--enable-system-test-libs],
                             [Use gtest/gmock/etc... from the system instead ]
                             [of the local copies (default is local)]),,
              [enable_system_test_libs=no])
AM_CONDITIONAL(SYSTEM_TEST_LIBS, test "x$enable_system_test_libs" = xyes)

AC_ARG_VAR([GMOCK_CFLAGS], [Compiler flags for gmock])
AC_ARG_VAR([GMOCK_LIBS], [Linker flags for gmock])
AC_ARG_VAR([GTEST_CFLAGS], [Compiler flags for gtest])
AC_ARG_VAR([GTEST_LIBS], [Linker flags for gtest])
if test "x$enable_system_test_libs" = xyes; then
  : "${GMOCK_CFLAGS:=-pthread}"
  : "${GMOCK_LIBS:=-lgmock -lgtest -pthread -lpthread}"
  : "${GTEST_CFLAGS:=-pthread}"
  : "${GTEST_LIBS:=-lgtest -pthread -lpthread}"
fi

AC_ARG_ENABLE(selftest,
              AS_HELP_STRING([--enable-selftest],
                             [Run extra tests with "make check" ]
                             [(may conflict with optimizations) ]
                             [(default is no)]),,
              [enable_selftest=no])
AM_CONDITIONAL(SELFTEST, test "x$enable_selftest" = xyes)

AC_ARG_WITH(rustc-demangle,
            AS_HELP_STRING([--with-rustc-demangle=/path/to/rustc-demangle],
                             [Link against the rustc-demangle library]
                             [to demangle Rust language symbols during]
                             [symbol dumping (default is no)]
                             [Pass the path to the crate root.]),,
            [with_rustc_demangle=no])

RUSTC_DEMANGLE_BASE_CFLAGS="-DHAVE_RUSTC_DEMANGLE"
RUSTC_DEMANGLE_BASE_LIBS="-lrustc_demangle -lpthread -ldl"

if test "x${with_rustc_demangle}" != xno; then
  if ! test -f "${with_rustc_demangle}/Cargo.toml"; then
    AC_MSG_ERROR(You must pass the path to the rustc-demangle crate for --with-rustc-demangle)
  fi
  RUSTC_DEMANGLE_CFLAGS="-I${with_rustc_demangle}/crates/capi/include ${RUSTC_DEMANGLE_BASE_CFLAGS}"
  RUSTC_DEMANGLE_LIBS="-L${with_rustc_demangle}/target/release ${RUSTC_DEMANGLE_BASE_LIBS}"
fi

AC_ARG_ENABLE(system-rustc-demangle,
              AS_HELP_STRING([--enable-system-rustc-demangle],
                             [Link against the rustc-demangle library]
                             [to demangle Rust language symbols during]
                             [symbol dumping (default is no). This assumes]
                             [that rustc-demangle is installed in your sysroot,]
                             [and all headers from it are available in your]
                             [standard include path]
                             ),,
               [enable_system_rustc_demangle=no])

if test "x${enable_system_rustc_demangle}" != xno; then
  if test "x${with_rustc_demangle}" != xno; then
    AC_MSG_ERROR([--enable-system-rustc-demangle and --with-rustc-demangle are mutually exclusive.])
  fi

  RUSTC_DEMANGLE_CFLAGS="${RUSTC_DEMANGLE_BASE_CFLAGS}"
  RUSTC_DEMANGLE_LIBS="${RUSTC_DEMANGLE_BASE_LIBS}"

  AC_CHECK_LIB([rustc_demangle], [rustc_demangle], [],
               [AC_MSG_ERROR(librustc_demangle.a must be present when --enable-system-rustc-demangle is specified)],
               [$RUSTC_DEMANGLE_LIBS])
  AC_CHECK_HEADERS(rustc_demangle.h, [],
                   [AC_MSG_ERROR(rustc_demangle.h must be present when --enable-system-rustc-demangle is specified)])
fi

AC_ARG_VAR([RUSTC_DEMANGLE_CFLAGS], [Compiler flags for rustc-demangle])
AC_ARG_VAR([RUSTC_DEMANGLE_LIBS], [Linker flags for rustc-demangle])

AC_ARG_ENABLE(zstd,
              AS_HELP_STRING([--enable-zstd],
                             [Enable decompression of ELF sections with zstd]),,
              [enable_zstd=no])
if test "x${enable_zstd}" != xno; then
  AC_CHECK_LIB(zstd, ZSTD_decompress, [],
               [AC_MSG_ERROR([zstd library not found.])])
  AC_CHECK_HEADER(zstd.h, [],
                  [AC_MSG_ERROR([zstd header not found.])])
fi

AC_ARG_WITH(tests-as-root,
            AS_HELP_STRING([--with-tests-as-root],
                           [Run the tests as root. Use this on platforms]
                           [like travis-ci.org that require root privileges]
                           [to use ptrace (default is no)]),,
            [with_tests_as_root=no])
AM_CONDITIONAL(TESTS_AS_ROOT, test "x$with_tests_as_root" = xyes)

AC_CONFIG_FILES(m4_flatten([
  breakpad.pc
  breakpad-client.pc
  Makefile
]))

AC_OUTPUT

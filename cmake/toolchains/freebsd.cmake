# SPDX-License-Identifier: BSD-3-Clause
# cmake/toolchains/freebsd.cmake — the part freebsd-amd64.cmake and
# freebsd-aarch64.cmake share: cross-compile budyk for FreeBSD on a Linux
# host with clang and lld, against a sysroot freebsd-sysroot.sh made.
#
# FREEBSD_SYSROOT (-D or the environment) names the sysroot; the FreeBSD
# version the target triple carries comes from its sys/param.h. clang and
# clang++ are the default compilers (-DCMAKE_C_COMPILER=clang-19 etc. for
# another), llvm-ar and llvm-ranlib the archivers when they are on PATH.

set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR ${BUDYK_FREEBSD_PROCESSOR})

if(NOT FREEBSD_SYSROOT AND DEFINED ENV{FREEBSD_SYSROOT})
    # Cached, so a later re-run of cmake does not need the environment.
    set(FREEBSD_SYSROOT "$ENV{FREEBSD_SYSROOT}" CACHE PATH "The FreeBSD sysroot")
endif()
if(NOT FREEBSD_SYSROOT OR NOT EXISTS "${FREEBSD_SYSROOT}/usr/include/sys/param.h")
    message(FATAL_ERROR
        "FREEBSD_SYSROOT='${FREEBSD_SYSROOT}' is not a FreeBSD sysroot (no usr/include/sys/param.h). "
        "Make one with cmake/toolchains/freebsd-sysroot.sh VERSION ${BUDYK_FREEBSD_ARCH} DIR "
        "and pass -DFREEBSD_SYSROOT=DIR.")
endif()
# try_compile() projects read this file again, without the cache.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES FREEBSD_SYSROOT)

# __FreeBSD_version 1404000 is 14.4; clang takes __FreeBSD__ from the triple.
file(STRINGS "${FREEBSD_SYSROOT}/usr/include/sys/param.h" _budyk_fbsd_version
     REGEX "^#define[ \t]+__FreeBSD_version[ \t]+[0-9]+")
string(REGEX MATCH "[0-9]+$" _budyk_fbsd_version "${_budyk_fbsd_version}")
math(EXPR _budyk_fbsd_major "${_budyk_fbsd_version} / 100000")
math(EXPR _budyk_fbsd_minor "${_budyk_fbsd_version} / 1000 % 100")
set(_budyk_triple
    "${BUDYK_FREEBSD_TRIPLE_ARCH}-unknown-freebsd${_budyk_fbsd_major}.${_budyk_fbsd_minor}")

set(CMAKE_SYSROOT "${FREEBSD_SYSROOT}")
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++)
endif()
set(CMAKE_C_COMPILER_TARGET "${_budyk_triple}")
set(CMAKE_CXX_COMPILER_TARGET "${_budyk_triple}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
find_program(BUDYK_LLVM_AR llvm-ar)
find_program(BUDYK_LLVM_RANLIB llvm-ranlib)
if(BUDYK_LLVM_AR AND BUDYK_LLVM_RANLIB)
    set(CMAKE_AR "${BUDYK_LLVM_AR}" CACHE FILEPATH "Archiver")
    set(CMAKE_RANLIB "${BUDYK_LLVM_RANLIB}" CACHE FILEPATH "Ranlib")
endif()

# Headers and libraries from the sysroot only; programs (xxd) from the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

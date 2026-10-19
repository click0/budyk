# SPDX-License-Identifier: BSD-3-Clause
# cmake/platform.cmake — detect the target OS and set platform flags.
#
# The target is CMAKE_SYSTEM_NAME: the host, or the system a toolchain file
# names (cmake/toolchains/freebsd-*.cmake cross-compiles for FreeBSD).
# -DBUDYK_PLATFORM=linux|freebsd is accepted (CI and the docs pass it) but
# only checked against that: a mismatch is an error rather than a silently
# ignored flag.

if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(_budyk_host "freebsd")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_budyk_host "linux")
else()
    set(_budyk_host "unknown")
endif()

if(DEFINED BUDYK_PLATFORM AND NOT BUDYK_PLATFORM STREQUAL _budyk_host)
    message(FATAL_ERROR
        "BUDYK_PLATFORM=${BUDYK_PLATFORM} but this build is for ${_budyk_host} "
        "(${CMAKE_SYSTEM_NAME}). Drop the flag or pass -DBUDYK_PLATFORM=${_budyk_host}; "
        "for FreeBSD on a Linux host, --toolchain cmake/toolchains/freebsd-amd64.cmake "
        "(or freebsd-aarch64.cmake).")
endif()
set(BUDYK_PLATFORM "${_budyk_host}" CACHE STRING
    "Target platform (from CMAKE_SYSTEM_NAME; linux or freebsd)" FORCE)

if(BUDYK_PLATFORM STREQUAL "freebsd")
    add_definitions(-DBUDYK_FREEBSD)
    # __FreeBSD_version comes from the system headers.
    set(BUDYK_PLATFORM_LIBS kvm devstat util)
elseif(BUDYK_PLATFORM STREQUAL "linux")
    add_definitions(-DBUDYK_LINUX)
    set(BUDYK_PLATFORM_LIBS "")
else()
    message(WARNING "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

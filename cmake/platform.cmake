# SPDX-License-Identifier: BSD-3-Clause
# cmake/platform.cmake — detect the host OS and set platform flags.
#
# budyk builds for the host it is built on; there is no cross-compilation.
# -DBUDYK_PLATFORM=linux|freebsd is accepted (CI and the docs pass it) but
# only checked against the detected host: a mismatch is an error rather
# than a silently ignored flag.

if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(_budyk_host "freebsd")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_budyk_host "linux")
else()
    set(_budyk_host "unknown")
endif()

if(DEFINED BUDYK_PLATFORM AND NOT BUDYK_PLATFORM STREQUAL _budyk_host)
    message(FATAL_ERROR
        "BUDYK_PLATFORM=${BUDYK_PLATFORM} but this is a ${_budyk_host} host "
        "(${CMAKE_SYSTEM_NAME}); budyk does not cross-compile. "
        "Drop the flag or pass -DBUDYK_PLATFORM=${_budyk_host}.")
endif()
set(BUDYK_PLATFORM "${_budyk_host}" CACHE STRING
    "Target platform (detected from the host; linux or freebsd)" FORCE)

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

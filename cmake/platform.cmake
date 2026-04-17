# SPDX-License-Identifier: BSD-3-Clause
# cmake/platform.cmake — detect OS and set platform-specific flags

if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(BUDYK_PLATFORM "freebsd")
    add_definitions(-DBUDYK_FREEBSD)
    # FreeBSD version macro for API compat (#ifdef branching)
    # __FreeBSD_version is set by the system headers
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BUDYK_PLATFORM "linux")
    add_definitions(-DBUDYK_LINUX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    set(BUDYK_PLATFORM "netbsd")
    add_definitions(-DBUDYK_NETBSD)
elseif(CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    set(BUDYK_PLATFORM "openbsd")
    add_definitions(-DBUDYK_OPENBSD)
else()
    message(WARNING "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
    set(BUDYK_PLATFORM "unknown")
endif()

# Platform-specific libraries
if(BUDYK_PLATFORM STREQUAL "freebsd")
    set(BUDYK_PLATFORM_LIBS kvm devstat util)
elseif(BUDYK_PLATFORM STREQUAL "linux")
    set(BUDYK_PLATFORM_LIBS "")
endif()

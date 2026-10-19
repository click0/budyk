# SPDX-License-Identifier: BSD-3-Clause
# cmake/toolchains/freebsd-amd64.cmake — cross-compile for FreeBSD amd64:
#
#   cmake/toolchains/freebsd-sysroot.sh 14.4 amd64 /opt/freebsd-14.4-amd64
#   cmake -B build-freebsd --toolchain cmake/toolchains/freebsd-amd64.cmake \
#         -DFREEBSD_SYSROOT=/opt/freebsd-14.4-amd64
#   cmake --build build-freebsd

set(BUDYK_FREEBSD_ARCH amd64)
set(BUDYK_FREEBSD_PROCESSOR amd64)
set(BUDYK_FREEBSD_TRIPLE_ARCH x86_64)
include("${CMAKE_CURRENT_LIST_DIR}/freebsd.cmake")

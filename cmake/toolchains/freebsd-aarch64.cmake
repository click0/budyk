# SPDX-License-Identifier: BSD-3-Clause
# cmake/toolchains/freebsd-aarch64.cmake — cross-compile for FreeBSD
# arm64 (aarch64):
#
#   cmake/toolchains/freebsd-sysroot.sh 14.4 aarch64 /opt/freebsd-14.4-aarch64
#   cmake -B build-freebsd --toolchain cmake/toolchains/freebsd-aarch64.cmake \
#         -DFREEBSD_SYSROOT=/opt/freebsd-14.4-aarch64
#   cmake --build build-freebsd

set(BUDYK_FREEBSD_ARCH aarch64)
set(BUDYK_FREEBSD_PROCESSOR aarch64)
set(BUDYK_FREEBSD_TRIPLE_ARCH aarch64)
include("${CMAKE_CURRENT_LIST_DIR}/freebsd.cmake")

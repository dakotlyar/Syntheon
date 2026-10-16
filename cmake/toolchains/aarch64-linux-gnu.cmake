# SPDX-License-Identifier: GPL-3.0-or-later
# Linux ARM64 cross build (design 8, P26): Ubuntu 22.04's aarch64 GCC, the
# arm64 sysroot (packaging/linux/make-sysroot.sh) and the arm64 Qt.
#
#   cmake --preset linux-arm64-cross -DSYNBENCH_SYSROOT=<sysroot> -DQT_HOST_PATH=<host Qt> \
#         -DSYNBENCH_TARGET_QT=<target Qt>
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
# CMake's runtime-dependency scan must read aarch64 ELF files.
set(CMAKE_OBJDUMP aarch64-linux-gnu-objdump)
set(CMAKE_STRIP aarch64-linux-gnu-strip)

if(NOT SYNBENCH_SYSROOT)
    set(SYNBENCH_SYSROOT "$ENV{SYNBENCH_SYSROOT}")
endif()
if(NOT SYNBENCH_TARGET_QT)
    set(SYNBENCH_TARGET_QT "$ENV{SYNBENCH_TARGET_QT}")
endif()
set(CMAKE_SYSROOT "${SYNBENCH_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${SYNBENCH_SYSROOT};${SYNBENCH_TARGET_QT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${SYNBENCH_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${SYNBENCH_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${SYNBENCH_SYSROOT}/usr/lib/pkgconfig:${SYNBENCH_SYSROOT}/usr/share/pkgconfig")

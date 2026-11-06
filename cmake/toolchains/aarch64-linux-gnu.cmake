# SPDX-License-Identifier: GPL-3.0-or-later
# Linux ARM64 cross build (design 8, P26): Ubuntu 24.04's aarch64 GCC 13,
# the noble arm64 sysroot (packaging/linux/make-sysroot.sh: glibc 2.39, its
# libstdc++-13-dev matches the compiler) and the official arm64 Qt, which needs
# glibc 2.38 / libstdc++ 3.4.32 and so cannot link against anything older.
#
#   cmake --preset linux-arm64-cross -DSYNTHEON_SYSROOT=<sysroot> -DQT_HOST_PATH=<host Qt> \
#         -DSYNTHEON_TARGET_QT=<target Qt>
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
# CMake's runtime-dependency scan must read aarch64 ELF files.
set(CMAKE_OBJDUMP aarch64-linux-gnu-objdump)
set(CMAKE_STRIP aarch64-linux-gnu-strip)

if(NOT SYNTHEON_SYSROOT)
    set(SYNTHEON_SYSROOT "$ENV{SYNTHEON_SYSROOT}")
endif()
if(NOT SYNTHEON_TARGET_QT)
    set(SYNTHEON_TARGET_QT "$ENV{SYNTHEON_TARGET_QT}")
endif()
set(CMAKE_SYSROOT "${SYNTHEON_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${SYNTHEON_SYSROOT};${SYNTHEON_TARGET_QT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${SYNTHEON_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${SYNTHEON_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${SYNTHEON_SYSROOT}/usr/lib/pkgconfig:${SYNTHEON_SYSROOT}/usr/share/pkgconfig")

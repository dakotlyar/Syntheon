# SPDX-License-Identifier: GPL-3.0-or-later
# Windows ARM64 cross build (design 8, D13): MSVC's x64 -> ARM64 cross tools
# (an `amd64_arm64` developer environment) and the arm64 Qt, with the x64 Qt as
# QT_HOST_PATH for moc/rcc/qmlcachegen.
#
#   cmake --preset windows-arm64-cross -DQT_HOST_PATH=<host Qt> -DCMAKE_PREFIX_PATH=<arm64 Qt>
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)
# MSVC's cl for C and C++ (the C compiler is needed for the third-party
# libraries). Named explicitly: with Visual Studio's LLVM folder on PATH (the
# dev environment adds it, and OpenSSL needs its clang-cl as an assembler),
# CMake would otherwise pick clang.exe, a GNU-style driver that targets x64.
set(CMAKE_C_COMPILER cl)
set(CMAKE_CXX_COMPILER cl)

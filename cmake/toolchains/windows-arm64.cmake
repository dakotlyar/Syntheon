# SPDX-License-Identifier: GPL-3.0-or-later
# Windows ARM64 cross build (design 8, D13): MSVC's x64 -> ARM64 cross tools
# (an `amd64_arm64` developer environment) and the arm64 Qt, with the x64 Qt as
# QT_HOST_PATH for moc/rcc/qmlcachegen.
#
#   cmake --preset windows-arm64-cross -DQT_HOST_PATH=<host Qt> -DCMAKE_PREFIX_PATH=<arm64 Qt>
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)

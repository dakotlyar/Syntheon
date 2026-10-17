# SPDX-License-Identifier: GPL-3.0-or-later
# See plain.cpp. MSVC only; never installed.
add_executable(pe_planted_noflag plain.cpp)
set_target_properties(pe_planted_noflag PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-tools")

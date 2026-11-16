# SPDX-License-Identifier: GPL-3.0-or-later
# The NPU test's vendor + runtime detection (Phase 4c design §3). The common
# vendor-selection + taxonomy lives in detect.cpp; the per-OS runtime probe is
# one of the two files below (P-WIN: detect_win.cpp is Windows-only, A-W
# syntax-checked on the Linux box).
target_sources(syntheon_test_npu PRIVATE
    detect.h detect.cpp
)
if(WIN32)
    target_sources(syntheon_test_npu PRIVATE detect_win.cpp)
else()
    target_sources(syntheon_test_npu PRIVATE detect_linux.cpp)
endif()

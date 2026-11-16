# SPDX-License-Identifier: GPL-3.0-or-later
# The three NPU workloads (Phase 4c design §2): vendor-agnostic orchestration
# over INpuEngine — a 3×window loop, the median, and the correctness sentinel.
target_sources(syntheon_test_npu PRIVATE
    workloads.h workloads.cpp
    classify.h classify.cpp
    detect.h detect.cpp
    llm.h llm.cpp
)

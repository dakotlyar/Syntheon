// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The small-LLM workload (Phase 4c design §2): ~1–1.5 B params, INT4, scored as
// generation tokens/s (TTFT is reported via WorkloadResult::ttftMs, not scored).
// A thin wrapper over the shared 3-window loop.

#include "tests/npu/workloads/workloads.h"

namespace sb::npu::workloads {

WorkloadResult runLlm(INpuEngine &engine, const LoadedModel &model, const std::function<bool()> &cancelled,
                      const std::function<void(int window)> &report = {});

} // namespace sb::npu::workloads

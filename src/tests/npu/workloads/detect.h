// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The object-detection workload (Phase 4c design §2): YOLOv8n / SSD class (INT8),
// scored as images/s. A thin wrapper over the shared 3-window loop.

#include "tests/npu/workloads/workloads.h"

namespace sb::npu::workloads {

WorkloadResult runDetect(INpuEngine &engine, const LoadedModel &model, const std::function<bool()> &cancelled,
                         const std::function<void(int window)> &report = {});

} // namespace sb::npu::workloads

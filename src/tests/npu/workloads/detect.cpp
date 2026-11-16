// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/workloads/detect.h"

namespace sb::npu::workloads {

WorkloadResult runDetect(INpuEngine &engine, const LoadedModel &model, const std::function<bool()> &cancelled,
                         const std::function<void(int)> &report)
{
    return runWindows(QStringLiteral("detect"), engine, model, cancelled, report);
}

} // namespace sb::npu::workloads

// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests/npu/ids.h"

namespace sb::npu {

namespace {

QString workloadIdString(Workload w)
{
    switch (w) {
    case Workload::Classify:
        return QStringLiteral("classify");
    case Workload::Detect:
        return QStringLiteral("detect");
    case Workload::Llm:
        return QStringLiteral("llm");
    }
    return {};
}

QString workloadLabelString(Workload w)
{
    switch (w) {
    case Workload::Classify:
        return QStringLiteral("Classify");
    case Workload::Detect:
        return QStringLiteral("Detect");
    case Workload::Llm:
        return QStringLiteral("LLM");
    }
    return {};
}

QString fieldSuffix(Field f)
{
    switch (f) {
    case Field::Imgs:
        return QStringLiteral("imgs");
    case Field::Tps:
        return QStringLiteral("tps");
    case Field::Ttft:
        return QStringLiteral("ttft");
    case Field::Img0:
        return QStringLiteral("img0");
    case Field::Img1:
        return QStringLiteral("img1");
    case Field::Img2:
        return QStringLiteral("img2");
    case Field::Tps0:
        return QStringLiteral("tps0");
    case Field::Tps1:
        return QStringLiteral("tps1");
    case Field::Tps2:
        return QStringLiteral("tps2");
    }
    return {};
}

} // namespace

QString workloadId(Workload w)
{
    return workloadIdString(w);
}

QString workloadLabel(Workload w)
{
    return workloadLabelString(w);
}

QString key(Workload w)
{
    // The headline metric, one per workload (design §9): the scored throughput.
    switch (w) {
    case Workload::Classify:
        return QStringLiteral("classify.imgs");
    case Workload::Detect:
        return QStringLiteral("detect.imgs");
    case Workload::Llm:
        return QStringLiteral("llm.tps");
    }
    return {};
}

QString key(Workload w, Field f)
{
    return workloadIdString(w) + QLatin1Char('.') + fieldSuffix(f);
}

QString label(Workload w, Field f)
{
    switch (f) {
    case Field::Imgs:
        return w == Workload::Detect ? QStringLiteral("Detection throughput") : QStringLiteral("Classification throughput");
    case Field::Tps:
        return QStringLiteral("Generation throughput");
    case Field::Ttft:
        return QStringLiteral("Time to first token");
    case Field::Img0:
    case Field::Img1:
    case Field::Img2:
        return QStringLiteral("per-window images/s (raw)");
    case Field::Tps0:
    case Field::Tps1:
    case Field::Tps2:
        return QStringLiteral("per-window tokens/s (raw)");
    }
    return {};
}

QString unit(Workload, Field f)
{
    switch (f) {
    case Field::Imgs:
    case Field::Img0:
    case Field::Img1:
    case Field::Img2:
        return QStringLiteral("img/s");
    case Field::Tps:
    case Field::Tps0:
    case Field::Tps1:
    case Field::Tps2:
        return QStringLiteral("tok/s");
    case Field::Ttft:
        return QStringLiteral("ms");
    }
    return {};
}

bool lowerIsBetter(Workload w, Field f)
{
    return w == Workload::Llm && f == Field::Ttft;
}

std::optional<Workload> workloadFromId(QStringView id)
{
    for (Workload w : kWorkloads)
        if (id == workloadIdString(w))
            return w;
    return std::nullopt;
}

QString vendorId(NpuVendor v)
{
    switch (v) {
    case NpuVendor::Intel:
        return QStringLiteral("intel");
    case NpuVendor::Amd:
        return QStringLiteral("amd");
    case NpuVendor::Qualcomm:
        return QStringLiteral("qualcomm");
    case NpuVendor::Null:
        return QStringLiteral("null");
    }
    return {};
}

QString vendorName(NpuVendor v)
{
    switch (v) {
    case NpuVendor::Intel:
        return QStringLiteral("Intel");
    case NpuVendor::Amd:
        return QStringLiteral("AMD");
    case NpuVendor::Qualcomm:
        return QStringLiteral("Qualcomm");
    case NpuVendor::Null:
        return QStringLiteral("Null");
    }
    return {};
}

} // namespace sb::npu

#include "tests/common/skeleton_test.h"

namespace sb::tests {

std::chrono::seconds SkeletonTest::timeLimit(const OptionValues &) const
{
    // 0 = no limit; that's Battery's contract (it runs until shutdown).
    return m_info.exclusivity == Exclusivity::RunsAlone ? std::chrono::seconds(0) : std::chrono::seconds(60);
}

TestResult SkeletonTest::run(RunContext &ctx, const OptionValues &)
{
    ctx.progress(1.0, QStringLiteral("Done"));
    TestResult r;
    r.status = Status::Ok;
    return r;
}

} // namespace sb::tests

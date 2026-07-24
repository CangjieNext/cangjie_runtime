#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "src/Concurrency/ProcessorScalePolicy.h"

namespace {

using MapleRuntime::ResolveProcessorLimit;

void ExpectEq(size_t actual, size_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%zu actual=%zu\n", message, expected, actual);
        std::abort();
    }
}

void TestMarkSweepUsesTwentyProcessorsPerLogicalCPU()
{
    ExpectEq(ResolveProcessorLimit(8, true), 160, "8-core MSGC processor limit");
    ExpectEq(ResolveProcessorLimit(128, true), 2560, "128-core MSGC processor limit");
}

void TestCopyGCKeepsExistingTwoProcessorsPerLogicalCPU()
{
    ExpectEq(ResolveProcessorLimit(8, false), 16, "8-core CopyGC processor limit");
    ExpectEq(ResolveProcessorLimit(128, false), 256, "128-core CopyGC processor limit");
}

void TestProcessorLimitSaturatesInsteadOfOverflowing()
{
    const size_t max = std::numeric_limits<size_t>::max();
    ExpectEq(ResolveProcessorLimit(max, true), max, "MSGC processor limit overflow");
    ExpectEq(ResolveProcessorLimit(max, false), max, "CopyGC processor limit overflow");
}

} // namespace

int main()
{
    TestMarkSweepUsesTwentyProcessorsPerLogicalCPU();
    TestCopyGCKeepsExistingTwoProcessorsPerLogicalCPU();
    TestProcessorLimitSaturatesInsteadOfOverflowing();
    return 0;
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "src/Heap/MCollector/MSAssistPacing.h"

namespace {

using MapleRuntime::ResolveMSMarkBaseAssistRatioQ16;

void ExpectEqual(uint64_t actual, uint64_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected %llu, got %llu\n", message,
                     static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual));
        std::abort();
    }
}

void TestDiagnosticDisableOverrideZerosAssistRatio()
{
    ExpectEqual(ResolveMSMarkBaseAssistRatioQ16(12345, true), 0,
                "disabled assist ratio");
}

void TestDiagnosticDisableOverridePreservesEnabledRatio()
{
    ExpectEqual(ResolveMSMarkBaseAssistRatioQ16(12345, false), 12345 / 4,
                "enabled assist must start at the normal-pressure base rate");
}

} // namespace

int main()
{
    TestDiagnosticDisableOverrideZerosAssistRatio();
    TestDiagnosticDisableOverridePreservesEnabledRatio();
    return 0;
}

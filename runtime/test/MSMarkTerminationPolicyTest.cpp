// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "src/Heap/MCollector/MSMarkTerminationPolicy.h"

#include <cstdio>
#include <cstdlib>

namespace {

using MapleRuntime::ComputeMSMarkTerminationDrainHelpers;

void ExpectEqual(size_t actual, size_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%zu actual=%zu\n", message, expected, actual);
        std::abort();
    }
}

void TestTerminationDrainStartsOnlyForLargeResidual()
{
    ExpectEqual(ComputeMSMarkTerminationDrainHelpers(63, 255, 8), 0,
                "small residual stays serial");
    ExpectEqual(ComputeMSMarkTerminationDrainHelpers(64, 0, 8), 1,
                "64 residual tasks enable one helper");
    ExpectEqual(ComputeMSMarkTerminationDrainHelpers(0, 256, 8), 1,
                "256 residual bitmap words enable one helper");
}

void TestTerminationDrainHelperLadderDoublesAndClamps()
{
    ExpectEqual(ComputeMSMarkTerminationDrainHelpers(127, 511, 8), 1,
                "first termination helper band");
    ExpectEqual(ComputeMSMarkTerminationDrainHelpers(128, 0, 8), 2,
                "128 residual tasks enable two helpers");
    ExpectEqual(ComputeMSMarkTerminationDrainHelpers(0, 1024, 8), 3,
                "1024 residual bitmap words enable three helpers");
    ExpectEqual(ComputeMSMarkTerminationDrainHelpers(4096, 0, 2), 2,
                "termination helpers respect configured maximum");
    ExpectEqual(ComputeMSMarkTerminationDrainHelpers(4096, 4096, 0), 0,
                "no available helper keeps termination serial");
}

} // namespace

int main()
{
    TestTerminationDrainStartsOnlyForLargeResidual();
    TestTerminationDrainHelperLadderDoublesAndClamps();
    return 0;
}

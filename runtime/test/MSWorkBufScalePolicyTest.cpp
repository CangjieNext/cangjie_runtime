// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "src/Heap/MCollector/MSWorkBufScalePolicy.h"

#include <cstdio>
#include <cstdlib>

namespace {

using MapleRuntime::IsValidMSWorkBufCapacity;
using MapleRuntime::ResolveMSWorkBufCapacity;
using MapleRuntime::ResolveMSWorkBufScaleConfig;

void ExpectEqual(size_t actual, size_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%zu actual=%zu\n",
                     message, expected, actual);
        std::abort();
    }
}

void ExpectTrue(bool actual, const char* message)
{
    if (!actual) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void TestProcessorBoundaries()
{
    ExpectEqual(ResolveMSWorkBufCapacity(1, 0), 1024, "P1");
    ExpectEqual(ResolveMSWorkBufCapacity(16, 0), 1024, "P16");
    ExpectEqual(ResolveMSWorkBufCapacity(17, 0), 512, "P17");
    ExpectEqual(ResolveMSWorkBufCapacity(32, 0), 512, "P32");
    ExpectEqual(ResolveMSWorkBufCapacity(33, 0), 256, "P33");
    ExpectEqual(ResolveMSWorkBufCapacity(64, 0), 256, "P64");
    ExpectEqual(ResolveMSWorkBufCapacity(65, 0), 128, "P65");
    ExpectEqual(ResolveMSWorkBufCapacity(1024, 0), 128, "P1024");
}

void TestConfiguredCapacityWins()
{
    ExpectEqual(ResolveMSWorkBufCapacity(150, 1024), 1024,
                "configured capacity");
    ExpectTrue(IsValidMSWorkBufCapacity(128), "128 must be valid");
    ExpectTrue(IsValidMSWorkBufCapacity(1024), "1024 must be valid");
    ExpectTrue(!IsValidMSWorkBufCapacity(64), "64 must be invalid");
}

void TestDerivedWorkUsesSelectedCapacity()
{
    auto config = ResolveMSWorkBufScaleConfig(50, 0);
    ExpectEqual(config.capacity, 256, "P50 capacity");
    ExpectEqual(config.assistTriggerObjects, 256, "P50 assist trigger");
    ExpectEqual(config.assistSliceObjects, 256, "P50 assist slice");
    ExpectEqual(config.helperPromoteObjects, 32, "P50 helper tail");
    ExpectEqual(config.checkpointObjects, 1ULL << 20,
                "checkpoint object quantum");
    ExpectEqual(config.backlogObjectsPerLevel, 1ULL << 20,
                "backlog object quantum");
}

} // namespace

int main()
{
    TestProcessorBoundaries();
    TestConfiguredCapacityWins();
    TestDerivedWorkUsesSelectedCapacity();
    return 0;
}

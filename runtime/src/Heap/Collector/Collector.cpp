// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Collector/Collector.h"

#include "Heap/Heap.h"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
namespace {
const char* const COLLECTOR_NAME[] = { "No Collector", "Proxy Collector", "Regional-Copying Collector",
                                       "Smooth Collector", "Mark-Sweep Collector" };
}

const char* Collector::GetGCPhaseName(GCPhase phase)
{
    // Handle Mark-Sweep phases explicitly (values >= 20)
    switch (phase) {
        case GC_PHASE_MS_MARK_SETUP:       return "MS_MARK_SETUP";
        case GC_PHASE_MS_CONCURRENT_MARK:  return "MS_CONCURRENT_MARK";
        case GC_PHASE_MS_MARK_TERMINATION: return "MS_MARK_TERMINATION";
        default: break;
    }
    static const char* phaseNames[] = {
        "undefined phase", // 0
        "idle phase",      // 1
        "finish phase",    "reclaim satb phase",
        "stub phase",      "stub phase",
        "stub phase",      "stub phase",
        "init phase",      "enum phase",
        "trace phase",     "clear satb phase",
        "forward phase",   "enum fix phase",
        "trace fix phase", "clear trace fix phase",
        "fix stack phase", "preforward phase",
    };
    constexpr size_t maxPhase = sizeof(phaseNames) / sizeof(phaseNames[0]);
    if (phase >= maxPhase) {
        return "unknown phase";
    }
    return phaseNames[phase];
}

Collector::Collector() {}

const char* Collector::GetCollectorName() const { return COLLECTOR_NAME[collectorType]; }

void Collector::RequestGC(GCReason reason, bool async) { RequestGCInternal(reason, async); }
} // namespace MapleRuntime.

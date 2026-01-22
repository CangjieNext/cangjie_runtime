// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "GcStats.h"

#include "Base/LogFile.h"
#include "Collector/TracingCollector.h"
#include "Heap/Heap.h"

namespace MapleRuntime {

void GCStats::Init()
{
    isConcurrentMark = false;
    async = false;
    gcStartTime = TimeUtil::NanoSeconds();
    gcEndTime = TimeUtil::NanoSeconds();
    collectedObjects = 0;
    collectedBytes = 0;

    fromSpaceSize = 0;
    smallGarbageSize = 0;

    pinnedSpaceSize = 0;
    pinnedGarbageSize = 0;

    largeSpaceSize = 0;
    largeGarbageSize = 0;

    liveBytesBeforeGC = 0;
    liveBytesAfterGC = 0;

    garbageRatio = 0.0;
    collectionRate = 0.0;

    // 20 MB:set 20 MB as intial value
    heapThreshold = std::min(CangjieRuntime::GetGCParam().gcThreshold, 20 * MB);
    // 0.2:set 20% heap size as intial value
    heapThreshold = std::min(static_cast<size_t>(Heap::GetHeap().GetMaxCapacity() * 0.2), heapThreshold);
}

void GCStats::Dump() const
{
    // Print a summary of the last GC.
    size_t liveSize = Heap::GetHeap().GetAllocatedSize();
    size_t heapSize = Heap::GetHeap().GetUsedPageSize();
    double utilization = (heapSize == 0) ? 0 : (static_cast<double>(liveSize) / heapSize) * 100; // 100 for percentage.

    // Do not change this GC log format.
    // Output one line statistic info after each gc task,
    // include the gc type, collected objects and current heap utilization, etc.
    // display to std-output. take care to modify.
    LOG(RTLOG_INFO,
        "GC for %s: %s collected objects: %zu->%s, %.2f%% utilization (%zu->%s/%zu->%s), "
        "total GC time: %lu->%s",
        g_gcRequests[reason].name, (async ? "async:" : "sync:"),
        collectedBytes, PrettyOrderInfo(collectedBytes, "B").Str(),
        utilization, liveSize, PrettyOrderInfo(liveSize, "B").Str(),
        heapSize, PrettyOrderInfo(heapSize, "B").Str(),
        gcEndTime - gcStartTime, PrettyOrderMathNano(gcEndTime - gcStartTime, "s").Str());

    VLOG(REPORT, "allocated size: %s, heap size: %s, heap utilization: %.2f%%", Pretty(liveSize).Str(),
         Pretty(heapSize).Str(), utilization);
}

// GCEventHistory implementation
void GCEventHistory::AddEvent(const GCEvent& event)
{
    std::lock_guard<std::mutex> lock(mutex);
    events[nextIndex] = event;
    nextIndex = (nextIndex + 1) % MAX_EVENTS;
    if (count < MAX_EVENTS) {
        count++;
    }
}

void GCEventHistory::DumpEvents() const
{
    std::lock_guard<std::mutex> lock(mutex);
    if (count == 0) {
        LOG(RTLOG_ERROR, "\tNo GC events recorded");
        return;
    }

    LOG(RTLOG_ERROR, "\tLast %zu GC events:", count);
    for (size_t i = 0; i < count; i++) {
        // Calculate the index of the i-th most recent event
        size_t index = (nextIndex - 1 - i + MAX_EVENTS) % MAX_EVENTS;
        const GCEvent& event = events[index];
        LOG(RTLOG_ERROR, "\tEvent %zu: time=%s, before=%zu B, after=%zu B, collected=%zu B, reason=%s, duration=%zu us",
            i + 1,
            PrettyOrderMathNano(event.timestamp, "s").Str(),
            event.liveBytesBefore,
            event.liveBytesAfter,
            event.collectedBytes,
            g_gcRequests[event.reason].name,
            event.durationNs / NS_PER_US);
    }
}

// Static members initialization
uint64_t GCStats::prevGcStartTime = TimeUtil::NanoSeconds() - LONG_MIN_HEU_GC_INTERVAL_NS;
uint64_t GCStats::prevGcFinishTime = TimeUtil::NanoSeconds() - LONG_MIN_HEU_GC_INTERVAL_NS;
size_t g_gcCount = 0;
uint64_t g_gcTotalTimeUs = 0;
size_t g_gcCollectedTotalBytes = 0;

} // namespace MapleRuntime

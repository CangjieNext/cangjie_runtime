// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CopyCollector.h"

#include "Allocator/AllocBuffer.h"
#include "Allocator/RegionSpace.h"
#include "CangjieRuntime.h"
#include "Common/Runtime.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/SatbBuffer.h"
#include "ObjectModel/RefField.inline.h"
#include "schedule.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
void CopyCollector::PostGarbageCollection(uint64_t gcIndex)
{
    TracingCollector::PostGarbageCollection(gcIndex);
    MutatorManager::Instance().DestroyExpiredMutators();
}

void CopyCollector::CopyObject(const BaseObject& fromObj, BaseObject& toObj, size_t size) const
{
    uintptr_t from = reinterpret_cast<uintptr_t>(&fromObj);
    uintptr_t to = reinterpret_cast<uintptr_t>(&toObj);
    CHECK_E(memmove_s(reinterpret_cast<void*>(to), size, reinterpret_cast<void*>(from), size) != EOK, "memmove_s fail");
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanFixShadow(reinterpret_cast<void*>(from), reinterpret_cast<void*>(to), size);
#endif
}

void CopyCollector::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    ScopedEntryTrace trace("CJRT_GC_START");
    // prevent other threads stop-the-world during GC.
    // this may be removed in the future.
    ScopedSTWLock stwLock;
    // ScopedStopTheWorld stw;

    gcReason = reason;
    PreGarbageCollection(true);
    ScheduleTraceEvent(TRACE_EV_GC_START, -1, nullptr, 0);
    VLOG(REPORT, "[GC] Start %s %s gcIndex= %lu", GetCollectorName(), g_gcRequests[gcReason].name, gcIndex);
    GCStats& gcStats = GetGCStats();
    gcStats.collectedBytes = 0;
    gcStats.gcStartTime = TimeUtil::NanoSeconds();

    DoGarbageCollection();

    if (reason == GC_REASON_OOM) {
        Heap::GetHeap().GetAllocator().ReclaimGarbageMemory(true);
    }

    PostGarbageCollection(gcIndex);
    gcStats.gcEndTime = TimeUtil::NanoSeconds();
    UpdateGCStats();
    uint64_t gcTimeNs = gcStats.gcEndTime - gcStats.gcStartTime;
    ScheduleTraceEvent(TRACE_EV_GC_DONE, -1, nullptr, 0);
    double rate = (static_cast<double>(gcStats.collectedBytes) / gcTimeNs) * (static_cast<double>(NS_PER_S) / MB);
    VLOG(REPORT, "total gc time: %s us, collection rate %.3lf MB/s\n", Pretty(gcTimeNs / NS_PER_US).Str(), rate);
    g_gcCount++;
    g_gcTotalTimeUs += (gcTimeNs / NS_PER_US);
    g_gcCollectedTotalBytes += gcStats.collectedBytes;
    gcStats.collectionRate = rate;
}

void CopyCollector::ForwardFromSpace()
{
    ScopedEntryTrace trace("CJRT_GC_FORWARD");
    TransitionToGCPhase(GCPhase::GC_PHASE_FORWARD, true);

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    GCStats& stats = GetGCStats();
    stats.liveBytesBeforeGC = space.AllocatedBytes();
    stats.fromSpaceSize = space.FromSpaceSize();
    space.ForwardFromSpace(GetThreadPool());
}

void CopyCollector::RefineFromSpace()
{
    GCStats& stats = GetGCStats();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    stats.smallGarbageSize = space.RefineFromSpace();
}

void CopyCollector::MergeRootsFromAllocBuffers(WorkStack& workStack)
{
    reinterpret_cast<RegionSpace&>(theAllocator).VisitAllocBuffers([&workStack](AllocBuffer& buffer) {
        buffer.MergeRoots(workStack);
    });
}

void CopyCollector::UpdateGCStats()
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    GCStats& gcStats = GetGCStats();
    gcStats.Dump();

    size_t oldThreshold = gcStats.heapThreshold;
    size_t liveBytes = space.AllocatedBytes();
    size_t heapSize = space.GetMaxCapacity();
    size_t recentBytes = space.GetRecentAllocatedSize();

    // 4 ways to estimate heap next threshold.
#if defined (__OHOS__)
    constexpr double lowUtilGrowth = 1.8;
    constexpr double lowUtilRatio = 0.25;
    double heapGrowth = liveBytes < heapSize * lowUtilRatio ?
        lowUtilGrowth : 1 + (CangjieRuntime::GetHeapParam().heapGrowth);
#else
    double heapGrowth = 1 + (CangjieRuntime::GetHeapParam().heapGrowth);
#endif
    size_t threshold1 = liveBytes * heapGrowth;
    size_t threshold2 = oldThreshold * heapGrowth;
    size_t threshold3 = liveBytes * 1.2 / (1.0 + gcStats.garbageRatio);
    size_t threshold4 = space.GetTargetSize();
    size_t newThreshold = 0;
    uint64_t gcInterval = CangjieRuntime::GetGCParam().gcInterval;
    // 2 : We regard the half of heap size as a limit because of copying algorithm.
    if (liveBytes < oldThreshold && oldThreshold < (heapSize / 2)) {
#if defined (__OHOS__)
        // When the ulitization is low, we can give the old threshold a larger weight to compute average value.
        // 1, 4, 2, 1: These are the weights of the different parameters.
        // 8: It is the total weight.
        newThreshold = (threshold1 * 1 + threshold2 * 4 + threshold3 * 2 + threshold4 * 1) / 8;
        // 2s: We set the max waiting time to 2s to avoid memory increasing too fast.
        auto maxAdaptiveInterval = static_cast<uint64_t>(2) * MapleRuntime::SECOND_TO_NANO_SECOND;
        uint64_t gcAdaptiveInterval = static_cast<uint64_t>((newThreshold - liveBytes) / gcStats.collectionRate / MB);
        gcAdaptiveInterval = std::min(gcAdaptiveInterval, maxAdaptiveInterval);
        gcInterval = std::max(gcInterval, gcAdaptiveInterval);
#else
        // 4: Computing arithmetic mean
        newThreshold = (threshold1 + threshold2 + threshold3 + threshold4) / 4;
#endif
    } else {
        // When the ulitization is high, we try to avoid threshold increasing and give it a small weight.
        // 2, 1, 2, 3: These are the weights of the different parameters.
        // 8: It is the total weight.
        newThreshold = (threshold1 * 2 + threshold2 * 1 + threshold3 * 2 + threshold4 * 3) / 8;
    }
    // 0.98: make sure new threshold does not exceed reasonable limit.
    newThreshold = std::min(newThreshold, static_cast<size_t>(space.GetMaxCapacity() * 0.98));
    gcStats.heapThreshold = std::min(newThreshold, CangjieRuntime::GetGCParam().gcThreshold);
    g_gcRequests[GC_REASON_HEU].SetMinInterval(gcInterval);
    VLOG(REPORT, "live bytes %zu (survived %zu, recent-allocated %zu), update gc threshold %zu -> %zu", liveBytes,
         liveBytes - recentBytes, recentBytes, oldThreshold, gcStats.heapThreshold);
    TRACE_COUNT("CJRT_post_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}
} // namespace MapleRuntime

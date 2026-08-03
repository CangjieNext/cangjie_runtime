// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_STATS_H
#define MRT_STATS_H

#include <algorithm>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>

#include "Base/ImmortalWrapper.h"
#include "Base/Panic.h"
#include "GcRequest.h"

namespace MapleRuntime {
// statistics for previous gc.
class GCStats {
public:
    GCStats() = default;
    ~GCStats() = default;

    void Init();

    size_t GetThreshold() const { return heapThreshold.load(std::memory_order_acquire); }
    void SetThreshold(size_t val) { heapThreshold.store(val, std::memory_order_release); }
    size_t GetScavengeThreshold() const { return scavengeThreshold.load(std::memory_order_acquire); }
    void SetScavengeThreshold(size_t val) { scavengeThreshold.store(val, std::memory_order_release); }
    size_t GetMarkRunway() const { return markRunway.load(std::memory_order_acquire); }
    void SetMarkRunway(size_t val) { markRunway.store(val, std::memory_order_release); }
    // Snapshots persisted at GC end for pacing / HEU trigger calculations.
    size_t baselineAllocatedBytes = 0;  // in-use Region bytes snapshot at GC end
    size_t lastTrueLiveBytes = 0;       // trueLiveBytes from last GC marking
    size_t lastStableLiveBytes = 0;     // last non-inflated trueLiveBytes sample
    size_t lastRegionBytes = 0;         // in-use Region bytes snapshot at GC end
    size_t lastSurvivingRegionBytes = 0;        // predicted surviving region bytes from last GC end

    void Dump() const;

    static uint64_t GetPrevGCStartTime() { return prevGcStartTime; }

    static void SetPrevGCStartTime(uint64_t timestamp) { prevGcStartTime = timestamp; }

    static uint64_t GetPrevGCFinishTime() { return prevGcFinishTime; }

    static void SetPrevGCFinishTime(uint64_t timestamp) { prevGcFinishTime = timestamp; }

    static uint64_t prevGcStartTime;
    static uint64_t prevGcFinishTime;

    GCReason reason;
    bool isConcurrentMark;
    bool async;

    uint64_t gcStartTime;
    uint64_t gcEndTime;

    size_t liveBytesBeforeGC;
    size_t liveBytesAfterGC;

    size_t fromSpaceSize;
    size_t smallGarbageSize;

    size_t pinnedSpaceSize;
    size_t pinnedGarbageSize;

    size_t largeSpaceSize;
    size_t largeGarbageSize;

    size_t collectedBytes;
    size_t collectedObjects;

    double garbageRatio;
    double collectionRate; // bytes per nano-second

private:
    std::atomic<size_t> heapThreshold{0};
    std::atomic<size_t> scavengeThreshold{0};
    std::atomic<size_t> markRunway{0};
};
extern size_t g_gcCount;
extern uint64_t g_gcTotalTimeUs;
extern size_t g_gcCollectedTotalBytes;
} // namespace MapleRuntime
#endif // MRT_STATS_H

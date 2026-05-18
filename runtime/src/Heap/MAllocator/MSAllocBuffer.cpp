// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "MSRegionSpace.h"
#include "Heap.h"
#include "Mutator/Mutator.h"
#include "Base/Log.h"
#include "Collector/CollectorResources.h"
#include "Heap/MCollector/MarkSweepCollector.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

namespace MapleRuntime {

constexpr uint8_t AllocBufferMS::kCachedRegionSlots;

namespace {
enum class MSAllocRefillConsumer : uint8_t {
    SLOW_ALLOC = 0,
    BACKGROUND_FILL = 1,
};

enum class MSAllocRefillSource : uint8_t {
    CACHED_HIT = 0,
    PARTIAL_SWEPT,
    PARTIAL_UNSWEPT,
    FULL_UNSWEPT,
    EMPTY_REBIND,
    FRESH,
    FAILED,
};

struct MSAllocSlowPathCounters {
    std::atomic<uint64_t> slowPathCalls {0};
    std::atomic<uint64_t> fastAllocHits {0};
    std::atomic<uint64_t> cachedMisses {0};
    std::atomic<uint64_t> slowSweepNotifyCalls {0};
    std::atomic<uint64_t> slowFeedNotifyCalls {0};
    std::atomic<uint64_t> centralSweepNotifyCalls {0};
    std::atomic<uint64_t> refillCounters[2][7] {};
};

MSAllocSlowPathCounters g_msAllocSlowPathCounters;

// Lifetime-total counters (never reset, printed at process exit by DumpAllocCounters_MSAllocBuffer).
static std::atomic<uint64_t> g_msAllocFastHitsTotal{0};
static std::atomic<uint64_t> g_msAllocSlowCallsTotal{0};

constexpr size_t ToIndex(MSAllocRefillConsumer consumer)
{
    return static_cast<size_t>(consumer);
}

constexpr size_t ToIndex(MSAllocRefillSource source)
{
    return static_cast<size_t>(source);
}

MSAllocRefillSource ToRefillSource(MSRegionAcquireSource source)
{
    switch (source) {
        case MSRegionAcquireSource::PARTIAL_SWEPT:
            return MSAllocRefillSource::PARTIAL_SWEPT;
        case MSRegionAcquireSource::PARTIAL_UNSWEPT:
            return MSAllocRefillSource::PARTIAL_UNSWEPT;
        case MSRegionAcquireSource::FULL_UNSWEPT:
            return MSAllocRefillSource::FULL_UNSWEPT;
        case MSRegionAcquireSource::EMPTY_REBIND:
            return MSAllocRefillSource::EMPTY_REBIND;
        case MSRegionAcquireSource::NONE:
        default:
            return MSAllocRefillSource::FAILED;
    }
}

void RecordRefillSource(MSAllocRefillConsumer consumer, MSAllocRefillSource source)
{
    g_msAllocSlowPathCounters.refillCounters[ToIndex(consumer)][ToIndex(source)].fetch_add(1,
        std::memory_order_relaxed);
}
}

static constexpr bool kDisableMutatorAssistProbe = false;
constexpr uint8_t kWarmCacheHeatThreshold = 3;
constexpr uint8_t kHotCacheHeatThreshold = 6;
constexpr uint8_t kHeatBoostOnCacheMiss = 2;
constexpr uint8_t kHeatBoostOnCachedHit = 1;
constexpr uint8_t kHeatBoostOnCentralRefill = 1;
constexpr uint8_t kHeatCoolOnSaturatedCache = 1;
constexpr uint8_t kHeatCoolOnIdleScan = 1;
constexpr uint8_t kPrimaryFeedNotifyCachedLimit = 1;

uint8_t SaturatingAddU8(uint8_t value, uint8_t delta)
{
    return value > UINT8_MAX - delta ? UINT8_MAX : static_cast<uint8_t>(value + delta);
}

uint8_t SaturatingSubU8(uint8_t value, uint8_t delta)
{
    return value < delta ? 0 : static_cast<uint8_t>(value - delta);
}

void MSAllocBuffer_RecordCentralSweepNotify()
{
    g_msAllocSlowPathCounters.centralSweepNotifyCalls.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void MSAllocBuffer_DumpAndResetSlowAllocCounters()
{
    uint64_t slowPathCalls = g_msAllocSlowPathCounters.slowPathCalls.exchange(0, std::memory_order_relaxed);
    uint64_t cachedMisses = g_msAllocSlowPathCounters.cachedMisses.exchange(0, std::memory_order_relaxed);
    uint64_t slowSweepNotifyCalls =
        g_msAllocSlowPathCounters.slowSweepNotifyCalls.exchange(0, std::memory_order_relaxed);
    uint64_t slowFeedNotifyCalls =
        g_msAllocSlowPathCounters.slowFeedNotifyCalls.exchange(0, std::memory_order_relaxed);
    uint64_t centralSweepNotifyCalls =
        g_msAllocSlowPathCounters.centralSweepNotifyCalls.exchange(0, std::memory_order_relaxed);

    uint64_t refillCounters[2][7] = {};
    for (size_t consumer = 0; consumer < 2; ++consumer) {
        for (size_t source = 0; source < 7; ++source) {
            refillCounters[consumer][source] =
                g_msAllocSlowPathCounters.refillCounters[consumer][source].exchange(0, std::memory_order_relaxed);
        }
    }

    VLOG(REPORT,
         "[MS-ALLOC-MISS] slow_calls=%llu cached_hit=%llu cached_miss=%llu slow_sweep_notify=%llu "
         "slow_feed_notify=%llu central_sweep_notify=%llu slow_partial_swept=%llu "
         "slow_partial_unswept=%llu slow_full_unswept=%llu slow_empty_rebind=%llu slow_fresh=%llu slow_failed=%llu "
         "bg_partial_swept=%llu bg_partial_unswept=%llu bg_full_unswept=%llu bg_empty_rebind=%llu bg_fresh=%llu bg_failed=%llu",
         (unsigned long long)slowPathCalls,
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::SLOW_ALLOC)][ToIndex(MSAllocRefillSource::CACHED_HIT)],
         (unsigned long long)cachedMisses,
         (unsigned long long)slowSweepNotifyCalls,
         (unsigned long long)slowFeedNotifyCalls,
         (unsigned long long)centralSweepNotifyCalls,
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::SLOW_ALLOC)][ToIndex(MSAllocRefillSource::PARTIAL_SWEPT)],
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::SLOW_ALLOC)][ToIndex(MSAllocRefillSource::PARTIAL_UNSWEPT)],
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::SLOW_ALLOC)][ToIndex(MSAllocRefillSource::FULL_UNSWEPT)],
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::SLOW_ALLOC)][ToIndex(MSAllocRefillSource::EMPTY_REBIND)],
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::SLOW_ALLOC)][ToIndex(MSAllocRefillSource::FRESH)],
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::SLOW_ALLOC)][ToIndex(MSAllocRefillSource::FAILED)],
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::BACKGROUND_FILL)][ToIndex(MSAllocRefillSource::PARTIAL_SWEPT)],
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::BACKGROUND_FILL)][ToIndex(MSAllocRefillSource::PARTIAL_UNSWEPT)],
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::BACKGROUND_FILL)][ToIndex(MSAllocRefillSource::FULL_UNSWEPT)],
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::BACKGROUND_FILL)][ToIndex(MSAllocRefillSource::EMPTY_REBIND)],
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::BACKGROUND_FILL)][ToIndex(MSAllocRefillSource::FRESH)],
         (unsigned long long)refillCounters[ToIndex(MSAllocRefillConsumer::BACKGROUND_FILL)][ToIndex(MSAllocRefillSource::FAILED)]);
}

extern "C" void DumpAllocCounters_MSAllocBuffer()
{
    uint64_t fastHits  = g_msAllocFastHitsTotal.load(std::memory_order_relaxed);
    uint64_t slowCalls = g_msAllocSlowCallsTotal.load(std::memory_order_relaxed);
    fprintf(stderr, "[ALLOC-COUNTER] MSGC  fast_alloc_hits=%llu  slow_alloc_calls=%llu\n",
            (unsigned long long)fastHits, (unsigned long long)slowCalls);
}


AllocBufferMS::~AllocBufferMS()
{
    Fini();
}

AllocBufferMS::AllocBufferMS()
{
    // Ensure tlRegion_ is at offset 0 — same as AllocBuffer::tlRegion.
    // LLVM fast-path reads *(buffer + 0) as RegionInfo* and calls Alloc().
    static_assert(offsetof(AllocBufferMS, tlRegion_) == 0,
                  "tlRegion_ must be at offset 0 for LLVM fast-path compatibility");
    tlRegion_ = RegionInfo::NullRegion();
}

void AllocBufferMS::Init(CentralRegionPool* central)
{
    central_ = central;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        activeRegions_[cls] = nullptr;
    }
    // Register with CentralRegionPool so GC can FlushAll
    if (central_ != nullptr) {
        central_->RegisterAllocBufferMS(this);
    }
}

void AllocBufferMS::Fini()
{
    if (central_ != nullptr) {
        central_->RemoveAllocBufferMS(this);
    }
    central_ = nullptr;
}

uint8_t AllocBufferMS::BumpCacheMiss(uint8_t cls)
{
    uint8_t miss = cacheMiss_[cls].load(std::memory_order_relaxed);
    miss = SaturatingAddU8(miss, 1);
    cacheMiss_[cls].store(miss, std::memory_order_relaxed);
    return miss;
}

void AllocBufferMS::SetCacheMiss(uint8_t cls, uint8_t value)
{
    cacheMiss_[cls].store(value, std::memory_order_relaxed);
}

void AllocBufferMS::BoostCacheHeat(uint8_t cls, uint8_t delta)
{
    uint8_t heat = cacheHeat_[cls].load(std::memory_order_relaxed);
    cacheHeat_[cls].store(SaturatingAddU8(heat, delta), std::memory_order_relaxed);
}

void AllocBufferMS::CoolCacheHeat(uint8_t cls, uint8_t delta)
{
    uint8_t heat = cacheHeat_[cls].load(std::memory_order_relaxed);
    cacheHeat_[cls].store(SaturatingSubU8(heat, delta), std::memory_order_relaxed);
}

uint8_t AllocBufferMS::CountCachedRegions(uint8_t cls) const
{
    uint8_t cachedCount = 0;
    for (uint8_t slot = 0; slot < kCachedRegionSlots; ++slot) {
        if (cachedRegion_[cls][slot].load(std::memory_order_relaxed) != nullptr) {
            ++cachedCount;
        }
    }
    return cachedCount;
}

size_t AllocBufferMS::CountActiveRegions() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        if (activeRegions_[cls] != nullptr) {
            ++total;
        }
    }
    return total;
}

size_t AllocBufferMS::CountCachedRegions() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        total += CountCachedRegions(cls);
    }
    return total;
}

uint16_t AllocBufferMS::GetHotClassScore(uint8_t cls) const
{
    uint8_t heat = cacheHeat_[cls].load(std::memory_order_relaxed);
    uint8_t miss = cacheMiss_[cls].load(std::memory_order_relaxed);
    return static_cast<uint16_t>(heat) * 16 + miss;
}

uint8_t AllocBufferMS::GetHotClassRank(uint8_t cls) const
{
    uint16_t myScore = GetHotClassScore(cls);
    if (myScore == 0) {
        return kNumSizeClasses;
    }

    uint8_t rank = 0;
    for (uint8_t other = 0; other < kNumSizeClasses; ++other) {
        if (other == cls) {
            continue;
        }
        uint16_t otherScore = GetHotClassScore(other);
        if (otherScore > myScore || (otherScore == myScore && other < cls)) {
            ++rank;
        }
    }
    return rank;
}

uint8_t AllocBufferMS::GetHotHintClass() const
{
    return hotHintCls_.load(std::memory_order_relaxed);
}

bool AllocBufferMS::IsPrimaryHotClass(uint8_t cls) const
{
    uint8_t rank = GetHotClassRank(cls);
    return rank == 0;
}

bool AllocBufferMS::IsSecondaryHotClass(uint8_t cls) const
{
    return !IsPrimaryHotClass(cls) && GetHotClassScore(cls) >= kHotCacheHeatThreshold * 16;
}

uint8_t AllocBufferMS::GetTargetCachedSlots(uint8_t cls,
                                            MSFeedPressureLevel pressureLevel) const
{
    uint16_t score = GetHotClassScore(cls);
    if (score == 0) {
        return 0;
    }
    if (pressureLevel == MSFeedPressureLevel::EXTREME) {
        return 0;
    }
    if (pressureLevel == MSFeedPressureLevel::HIGH) {
        return IsPrimaryHotClass(cls) ? 1 : 0;
    }
    if (pressureLevel == MSFeedPressureLevel::MEDIUM) {
        if (IsPrimaryHotClass(cls)) {
            return 2;
        }
        return IsSecondaryHotClass(cls) ? 1 : 0;
    }
    if (IsPrimaryHotClass(cls)) {
        return kCachedRegionSlots;
    }
    if (score >= kHotCacheHeatThreshold * 16) {
        return 3;
    }
    return kBaseCachedRegionSlots;
}

bool AllocBufferMS::ShouldNotifyBackgroundFeed(uint8_t cls) const
{
    if (!IsPrimaryHotClass(cls)) {
        return false;
    }
    return CountCachedRegions(cls) <= kPrimaryFeedNotifyCachedLimit;
}

bool AllocBufferMS::ShouldBackgroundFillClass(uint8_t cls, uint8_t preferredCls,
                                             MSFeedPressureLevel pressureLevel) const
{
    bool isPreferred = preferredCls < kNumSizeClasses && cls == preferredCls;
    if (isPreferred) {
        uint8_t localHint = GetHotHintClass();
        return localHint == cls && ShouldNotifyBackgroundFeed(cls);
    }

    uint8_t targetSlots = GetTargetCachedSlots(cls, pressureLevel);
    if (targetSlots == 0 || CountCachedRegions(cls) >= targetSlots) {
        return false;
    }
    if (pressureLevel == MSFeedPressureLevel::HIGH) {
        return IsPrimaryHotClass(cls);
    }
    return IsPrimaryHotClass(cls) || IsSecondaryHotClass(cls) || GetBackgroundFillPriority(cls) != 0;
}

uint8_t AllocBufferMS::GetCacheFillThreshold(uint8_t cls) const
{
    uint8_t targetSlots = GetTargetCachedSlots(cls, MSFeedPressureLevel::LOW);
    if (targetSlots >= kCachedRegionSlots) {
        return 2;
    }
    if (targetSlots > kBaseCachedRegionSlots) {
        return 3;
    }
    return 4;
}

uint16_t AllocBufferMS::GetBackgroundFillPriority(uint8_t cls) const
{
    uint8_t targetSlots = GetTargetCachedSlots(cls, MSFeedPressureLevel::LOW);
    uint8_t cachedCount = CountCachedRegions(cls);
    uint8_t miss = cacheMiss_[cls].load(std::memory_order_relaxed);
    bool needsBaseRefill = miss >= GetCacheFillThreshold(cls);
    bool needsHotSpare = targetSlots > kBaseCachedRegionSlots && cachedCount < targetSlots;
    if (!needsBaseRefill && !needsHotSpare) {
        return 0;
    }

    uint8_t deficit = targetSlots > cachedCount ? static_cast<uint8_t>(targetSlots - cachedCount) : 0;
    return static_cast<uint16_t>(deficit) * 1024 + static_cast<uint16_t>(GetHotClassScore(cls)) * 4 + miss;
}

void AllocBufferMS::NotifyBackgroundFillIfNeeded(uint8_t cls)
{
    if (!ShouldNotifyBackgroundFeed(cls)) {
        return;
    }
    if (central_ != nullptr) {
        MSFeedPressureLevel pressureLevel = central_->GetFeedPressureLevel();
        if (pressureLevel == MSFeedPressureLevel::EXTREME) {
            return;
        }
        if (pressureLevel == MSFeedPressureLevel::HIGH && !IsPrimaryHotClass(cls)) {
            return;
        }
    }
    hotHintCls_.store(cls, std::memory_order_relaxed);
    Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor().MaybeNotifyFeed(cls);
}

void AllocBufferMS::TrimExtraCachedRegions(uint8_t cls, uint8_t targetSlots)
{
    targetSlots = std::min(targetSlots, kCachedRegionSlots);
    for (uint8_t slot = targetSlots; slot < kCachedRegionSlots; ++slot) {
        MSRegionInfo* extra = cachedRegion_[cls][slot].exchange(nullptr, std::memory_order_acquire);
        if (extra != nullptr) {
            ReturnRegionToCentral(extra, cls);
        }
    }
}

// ============================================================================
// Allocate — main entry point
// Pacing debt is no longer tracked per-object here.  It is consolidated into
// AccumulatePacingDebt(MS_REGION_SIZE) called once per region acquisition in
// SlowAlloc(), covering both the compiler fast-path (Allocate() bypassed) and
// the runtime slow-path uniformly.
// ============================================================================
MAddress AllocBufferMS::Allocate(size_t size, ThreadLocalData* local)
{
    (void)local;
    uint8_t cls = SizeClassInfo::SizeToClass(size);
    if (UNLIKELY(cls >= kNumSizeClasses)) {
        return 0;  // Large object — caller handles it via the large-region path
    }
    return FastAlloc(cls);
}

// ============================================================================
// FastAlloc — try allocation on the current active Region
// ============================================================================
MAddress AllocBufferMS::FastAlloc(uint8_t cls)
{
    MSRegionInfo* region = activeRegions_[cls];
    if (LIKELY(region != nullptr)) {
        uintptr_t addr;
        if (region->regionType == MSRegionType::MS_REGION_BUMP) {
            addr = region->AllocBump();
        } else {
            addr = region->AllocFromBitmapFast();
        }
        if (LIKELY(addr != 0)) {
            g_msAllocSlowPathCounters.fastAllocHits.fetch_add(1, std::memory_order_relaxed);
            g_msAllocFastHitsTotal.fetch_add(1, std::memory_order_relaxed);
            return static_cast<MAddress>(addr);
        }
    }

    // Current Region has no free slots (or no Region at all) — slow path
    return SlowAlloc(cls);
}

// ============================================================================
// SlowAlloc — refill from CentralRegionPool
// ============================================================================
MAddress AllocBufferMS::SlowAlloc(uint8_t cls)
{
    g_msAllocSlowPathCounters.slowPathCalls.fetch_add(1, std::memory_order_relaxed);
    g_msAllocSlowCallsTotal.fetch_add(1, std::memory_order_relaxed);
    bool backgroundFillRequested = false;

    // Step 1: Return the exhausted Region to the central pool (if any).
    // When the compiler fast path (EnableGCFastPath) is enabled, the mutator
    // bump-allocates inline and never calls Allocate(), so activeRegions_[cls]
    // may already be null here.  Pacing debt is recorded after the new region
    // is installed (Step 3) regardless of this condition.
    if (activeRegions_[cls] != nullptr) {
        ReturnRegion(cls);
    }

    // O2: Check thread-local cache before going to CentralRegionPool
    MSRegionInfo* newRegion = TakeCachedRegion(cls);
    if (newRegion != nullptr) {
        cacheConsumed_[cls].store(SaturatingAddU8(cacheConsumed_[cls].load(std::memory_order_relaxed), 1),
            std::memory_order_relaxed);
        BoostCacheHeat(cls, kHeatBoostOnCachedHit);
        RecordRefillSource(MSAllocRefillConsumer::SLOW_ALLOC, MSAllocRefillSource::CACHED_HIT);
    } else {
        g_msAllocSlowPathCounters.cachedMisses.fetch_add(1, std::memory_order_relaxed);
        BoostCacheHeat(cls, kHeatBoostOnCacheMiss);
        uint8_t missCount = BumpCacheMiss(cls);
        if (missCount >= GetCacheFillThreshold(cls) && ShouldNotifyBackgroundFeed(cls)) {
            g_msAllocSlowPathCounters.slowFeedNotifyCalls.fetch_add(1, std::memory_order_relaxed);
            hotHintCls_.store(cls, std::memory_order_relaxed);
            Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor().MaybeNotifyFeed(cls);
            backgroundFillRequested = true;
        }
        // Step 2: Get a new Region from CentralRegionPool
        newRegion = RefillFromCentral(cls);
        if (newRegion != nullptr) {
            BoostCacheHeat(cls, kHeatBoostOnCentralRefill);
        }
    }
    if (UNLIKELY(newRegion == nullptr)) {
        return 0;  // OOM or GC needed — caller handles
    }

    // Step 3: Set as active Region and record pacing debt for the consumed region.
    // AccumulatePacingDebt(MS_REGION_SIZE) accounts for ~one region worth of bytes,
    // covering both paths:
    //   - compiler fast-path: the previous region was exhausted inline (activeRegions_
    //     was null on entry), so no per-object debt was recorded.
    //   - runtime slow-path: Allocate() no longer tracks per-object debt; all debt
    //     is consolidated here at region-acquisition granularity.
    activeRegions_[cls] = newRegion;
    AccumulatePacingDebt(MS_REGION_SIZE);
    if (!backgroundFillRequested) {
        NotifyBackgroundFillIfNeeded(cls);
    }

    // Backup cache refill is handled by BackgroundFillCache after repeated misses.
    // Avoid doing another synchronous GetRegion() here: on case1 this can turn one
    // SlowAlloc miss into two central-pool refills, and the second refill may also
    // fall into lazy sweep on the mutator thread.

    // Step 4: Allocate from the new Region (respect region type)
    uintptr_t addr;
    if (newRegion->regionType == MSRegionType::MS_REGION_BUMP) {
        addr = newRegion->AllocBump();
    } else {
        addr = newRegion->AllocFromBitmapFast();
    }
    return static_cast<MAddress>(addr);
}

// ============================================================================
// ReturnRegion — classify and return active Region to CentralRegionPool
// ============================================================================
void AllocBufferMS::ReturnRegion(uint8_t cls)
{
    MSRegionInfo* region = activeRegions_[cls];
    if (region == nullptr) {
        return;
    }

    ReturnRegionToCentral(region, cls);
    activeRegions_[cls] = nullptr;
}

MSRegionInfo* AllocBufferMS::TakeCachedRegion(uint8_t cls)
{
    for (uint8_t slot = 0; slot < kCachedRegionSlots; ++slot) {
        MSRegionInfo* region = cachedRegion_[cls][slot].exchange(nullptr, std::memory_order_acquire);
        if (region != nullptr) {
            return region;
        }
    }
    return nullptr;
}

bool AllocBufferMS::TryCacheRegion(uint8_t cls, MSRegionInfo* region, uint8_t slotLimit)
{
    slotLimit = std::min(slotLimit, kCachedRegionSlots);
    for (uint8_t slot = 0; slot < slotLimit; ++slot) {
        MSRegionInfo* expected = nullptr;
        if (cachedRegion_[cls][slot].compare_exchange_strong(expected, region,
                std::memory_order_release, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void AllocBufferMS::ReturnRegionToCentral(MSRegionInfo* region, uint8_t cls)
{
    if (region == nullptr) {
        return;
    }

    // Experimental policy: TL regions returned after the cycle boundary are
    // stamped as swept in the current generation so they participate in the
    // next GC cycle instead of the current sweep.
    region->SetSweepGen(CentralRegionPool::GetGlobalSweepGen());

    bool isFree = region->IsCompletelyFree();
    bool hasSpace = region->HasFreeSpace();

    if (isFree) {
        central_->ReturnEmptyRegion(region);
    } else if (hasSpace) {
        // Has free slots → partial list
        central_->ReturnPartialRegion(region, cls);
    } else {
        // Full → full list
        central_->ReturnFullRegion(region, cls);
    }
}

// ============================================================================
// RefillFromCentral — get a Region from CentralRegionPool
// ============================================================================
MSRegionInfo* AllocBufferMS::RefillFromCentral(uint8_t cls, bool backgroundFill)
{
    MSAllocRefillConsumer consumer =
        backgroundFill ? MSAllocRefillConsumer::BACKGROUND_FILL : MSAllocRefillConsumer::SLOW_ALLOC;

    if (UNLIKELY(central_ == nullptr)) {
        RecordRefillSource(consumer, MSAllocRefillSource::FAILED);
        return nullptr;
    }

    MSRegionInfo* region = nullptr;
    if (!backgroundFill) {
        // Mutator: central pool first, then direct bump/grow. Do not go through releasedTree.
        MSRegionAcquireSource source = MSRegionAcquireSource::NONE;
        region = central_->GetRegion(cls, &source);
        if (region != nullptr) {
            RecordRefillSource(consumer, ToRefillSource(source));
            return region;
        }

        region = central_->GetFreshRegion(cls);
    } else {
        // gc-helper feed: bypass central lists and directly source from releasedTree or bump/grow.
        region = central_->GetFreshRegionForFeed(cls);
    }

    RecordRefillSource(consumer,
        region != nullptr ? MSAllocRefillSource::FRESH : MSAllocRefillSource::FAILED);
    return region;
}

// ============================================================================
// FlushAll — GC STW: return all active Regions to CentralRegionPool
// ============================================================================
void AllocBufferMS::FlushAll()
{
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        // O2: Return cached backup regions first
        for (uint8_t slot = 0; slot < kCachedRegionSlots; ++slot) {
            MSRegionInfo* cached = cachedRegion_[cls][slot].exchange(nullptr, std::memory_order_acquire);
            if (cached != nullptr) {
                // markBitmap clearing is now handled by ClearAllMarkBitmaps
                // before STW, so no per-region ClearMarkBitmap here.
                ReturnRegionToCentral(cached, cls);
            }
        }

        // Active region: return to pool (bitmap already cleared pre-STW)
        ReturnRegion(cls);
    }
}

// ============================================================================
// BackgroundFillCache — O9: adaptive prefetch by gc-helper thread
// ============================================================================
MSAllocCacheFillStats AllocBufferMS::BackgroundFillCache(uint8_t preferredCls,
                                                         MSFeedPressureLevel pressureLevel)
{
    MSAllocCacheFillStats stats;
    if (UNLIKELY(central_ == nullptr)) {
        return stats;
    }
    if (pressureLevel == MSFeedPressureLevel::EXTREME) {
        return stats;
    }

    uint8_t localHintCls = GetHotHintClass();
    if (preferredCls >= kNumSizeClasses) {
        preferredCls = localHintCls;
    }
    std::array<uint8_t, kNumSizeClasses> classOrder {};
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        classOrder[cls] = cls;
    }
    std::sort(classOrder.begin(), classOrder.end(), [this, preferredCls](uint8_t lhs, uint8_t rhs) {
        bool lhsPrimary = preferredCls < kNumSizeClasses && lhs == preferredCls;
        bool rhsPrimary = preferredCls < kNumSizeClasses && rhs == preferredCls;
        if (lhsPrimary != rhsPrimary) {
            return lhsPrimary;
        }
        bool lhsHot = IsPrimaryHotClass(lhs);
        bool rhsHot = IsPrimaryHotClass(rhs);
        if (lhsHot != rhsHot) {
            return lhsHot;
        }
        uint16_t lhsPriority = GetBackgroundFillPriority(lhs);
        uint16_t rhsPriority = GetBackgroundFillPriority(rhs);
        if (lhsPriority != rhsPriority) {
            return lhsPriority > rhsPriority;
        }
        return lhs < rhs;
    });

    for (uint8_t cls : classOrder) {
        bool clsFilled = false;

        uint8_t targetSlots = GetTargetCachedSlots(cls, pressureLevel);
        TrimExtraCachedRegions(cls, targetSlots);
        uint8_t cachedBefore = CountCachedRegions(cls);
        if (targetSlots == 0) {
            SetCacheMiss(cls, 0);
            CoolCacheHeat(cls, kHeatCoolOnIdleScan);
            continue;
        }
        if (cachedBefore >= targetSlots) {
            SetCacheMiss(cls, 0);
            CoolCacheHeat(cls, kHeatCoolOnSaturatedCache);
            continue;
        }

        if (!ShouldBackgroundFillClass(cls, preferredCls, pressureLevel)) {
            continue;
        }

        if (GetBackgroundFillPriority(cls) == 0) {
            CoolCacheHeat(cls, kHeatCoolOnIdleScan);
            continue;
        }

        bool cacheFull = true;
        size_t filledForCls = 0;
        for (uint8_t slot = 0; slot < targetSlots; ++slot) {
            if (cachedRegion_[cls][slot].load(std::memory_order_relaxed) != nullptr) {
                continue;
            }
            cacheFull = false;
            MSRegionInfo* region = RefillFromCentral(cls, true);
            if (region == nullptr) {
                break;
            }
            if (!TryCacheRegion(cls, region, targetSlots)) {
                central_->ReturnPartialRegion(region, cls);
                break;
            }
            stats.filledRegions++;
            filledForCls++;
            clsFilled = true;
        }
        if (clsFilled) {
            stats.filledClasses++;
        }
        if (cacheFull) {
            SetCacheMiss(cls, 0);
            CoolCacheHeat(cls, kHeatCoolOnSaturatedCache);
            continue;
        }

        uint8_t cachedAfter = CountCachedRegions(cls);

        if (cachedAfter >= targetSlots) {
            SetCacheMiss(cls, 0);
            cacheConsumed_[cls].store(0, std::memory_order_relaxed);
            CoolCacheHeat(cls, kHeatCoolOnSaturatedCache);
        }
    }

    if (preferredCls < kNumSizeClasses &&
        CountCachedRegions(preferredCls) >= GetTargetCachedSlots(preferredCls, pressureLevel)) {
        hotHintCls_.store(kNoPreferredHotClass, std::memory_order_relaxed);
    }
    return stats;
}

AllocBufferMS* AllocBufferMS::GetAllocBufferMS()
{
    return ThreadLocal::GetAllocBufferMS();
}

AllocBufferMS* AllocBufferMS::GetOrCreateAllocBufferMS()
{
    return GetOrCreateAllocBufferMS(ThreadLocal::GetThreadLocalData());
}

AllocBufferMS* AllocBufferMS::GetOrCreateAllocBufferMS(ThreadLocalData* local)
{
    AllocBufferMS* buffer = local != nullptr ? local->msAllocBuffer : GetAllocBufferMS();
    if (buffer == nullptr) {
        buffer = new (std::nothrow) AllocBufferMS();
        CHECK_DETAIL(buffer != nullptr, "new MS alloc buffer fail");
        RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
        buffer->Init(space.GetCentralPool());
        if (local != nullptr) {
            local->msAllocBuffer = buffer;
        } else {
            ThreadLocal::SetAllocBufferMS(buffer);
        }
    }
    return buffer;
}

// ============================================================================
// AccumulatePacingDebt — unified pacing debt accumulation
// Called at region-acquisition time from SlowAlloc() (once per ~MS_REGION_SIZE
// bytes, covering both compiler fast-path and runtime slow-path), and also
// directly from large-object paths (TryAllocateOnceMS).
// Resets gcAssistDebtScaled_ when marking is not active so stale debt from a
// previous GC cycle does not carry over.
// ============================================================================
void AllocBufferMS::AccumulatePacingDebt(size_t size)
{
    // CJ_MS_DISABLE_ASSIST=1 : suppress mutator assist entirely (for benchmarking).
    static const bool assistDisabled = (std::getenv("CJ_MS_DISABLE_ASSIST") != nullptr &&
                                        std::getenv("CJ_MS_DISABLE_ASSIST")[0] == '1');
    if (assistDisabled) {
        return;
    }
    // Check whether marking is active before accumulating debt.
    // The previous guard `if (gcAssistDebtScaled_ == 0) return` was wrong:
    // since the field is zero-initialized and only grows here, the early-exit
    // prevented debt from ever accumulating, causing assist to never trigger.
    MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
    if (collector == nullptr) {
        gcAssistDebtScaled_ = 0;
        return;
    }
    uint64_t ratioScaled = collector->GetAssistWorkPerByteScaled();
    if (ratioScaled == 0) {
        gcAssistDebtScaled_ = 0;  // Marking not active — clear stale debt
        return;
    }
    uint64_t deltaDebt = static_cast<uint64_t>(size) * ratioScaled;
    if (UNLIKELY(gcAssistDebtScaled_ > UINT64_MAX - deltaDebt)) {
        gcAssistDebtScaled_ = UINT64_MAX;
    } else {
        gcAssistDebtScaled_ += deltaDebt;
    }
    const uint64_t ratioScale = MarkSweepCollector::GetAssistRatioScale();
    size_t debtObjects = static_cast<size_t>(gcAssistDebtScaled_ / ratioScale);
    if (UNLIKELY(debtObjects >= collector->GetAssistDebtTriggerObjects())) {
        size_t paidObjects = collector->MSMarkAssist(debtObjects);
        size_t accounted = std::min(paidObjects, debtObjects);
        gcAssistDebtScaled_ -= static_cast<uint64_t>(accounted) * ratioScale;
    }
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

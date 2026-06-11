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

namespace MapleRuntime {

constexpr uint8_t AllocBufferMS::kCachedRegionSlots;

static constexpr bool kDisableMutatorAssistProbe = false;
constexpr uint8_t kWarmCacheHeatThreshold = 3;
constexpr uint8_t kHotCacheHeatThreshold = 6;
constexpr uint8_t kHeatBoostOnCacheMiss = 2;
constexpr uint8_t kHeatBoostOnCachedHit = 1;
constexpr uint8_t kHeatBoostOnCentralRefill = 1;
constexpr uint8_t kHeatCoolOnSaturatedCache = 1;
constexpr uint8_t kHeatCoolOnIdleScan = 1;
constexpr uint8_t kPrimaryFeedNotifyCachedLimit = 0;
constexpr uint8_t kFeedHotnessDecayShift = 3;
constexpr uint8_t kFeedHotnessMissDemand = 48;
constexpr uint8_t kFeedHotnessCachedHitDemand = 8;
constexpr uint8_t kFeedHotnessCentralRefillDemand = 16;

uint8_t SaturatingAddU8(uint8_t value, uint8_t delta)
{
    return value > UINT8_MAX - delta ? UINT8_MAX : static_cast<uint8_t>(value + delta);
}

uint8_t SaturatingSubU8(uint8_t value, uint8_t delta)
{
    return value < delta ? 0 : static_cast<uint8_t>(value - delta);
}

uint16_t SaturatingAddU16(uint16_t value, uint16_t delta)
{
    return value > UINT16_MAX - delta ? UINT16_MAX : static_cast<uint16_t>(value + delta);
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
        std::vector<MSRegionInfo*> drained;
        (void)DrainAllLocalEmptyRegions(drained);
        for (MSRegionInfo* region : drained) {
            central_->ReturnEmptyRegion(region);
        }
    }
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
    return cachedRegion_[cls].load(std::memory_order_relaxed) != nullptr ? 1 : 0;
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
    uint16_t stable = feedHotness_[cls].load(std::memory_order_relaxed);
    uint8_t heat = cacheHeat_[cls].load(std::memory_order_relaxed);
    uint8_t miss = cacheMiss_[cls].load(std::memory_order_relaxed);
    uint32_t score = static_cast<uint32_t>(stable) + static_cast<uint32_t>(heat) * 16 +
                     static_cast<uint32_t>(miss) * 8;
    return score > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(score);
}

void AllocBufferMS::ObserveFeedHotness(uint8_t cls, uint8_t demand)
{
    uint16_t old = feedHotness_[cls].load(std::memory_order_relaxed);
    uint16_t decay = static_cast<uint16_t>(old >> kFeedHotnessDecayShift);
    if (old != 0 && decay == 0) {
        decay = 1;
    }
    uint16_t base = old > decay ? static_cast<uint16_t>(old - decay) : 0;
    feedHotness_[cls].store(SaturatingAddU16(base, demand), std::memory_order_relaxed);
}

void AllocBufferMS::DecayFeedHotness(uint8_t cls, uint8_t floor)
{
    uint16_t old = feedHotness_[cls].load(std::memory_order_relaxed);
    uint16_t decay = static_cast<uint16_t>(old >> kFeedHotnessDecayShift);
    if (old != 0 && decay == 0) {
        decay = 1;
    }
    uint16_t next = old > decay ? static_cast<uint16_t>(old - decay) : 0;
    if (next < floor) {
        next = floor;
    }
    feedHotness_[cls].store(next, std::memory_order_relaxed);
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
    uint8_t rank = GetHotClassRank(cls);
    return rank > 0 && rank < kBackgroundFeedTopK && GetHotClassScore(cls) != 0;
}

uint8_t AllocBufferMS::GetTargetCachedSlots(uint8_t cls) const
{
    return IsTopFeedHotClass(cls, GetHotHintClass()) ? kCachedRegionSlots : 0;
}

bool AllocBufferMS::ShouldNotifyBackgroundFeed(uint8_t cls) const
{
    return IsTopFeedHotClass(cls, cls) && CountCachedRegions(cls) <= kPrimaryFeedNotifyCachedLimit;
}

uint8_t AllocBufferMS::GetCacheFillThreshold(uint8_t cls) const
{
    return IsTopFeedHotClass(cls, cls) ? 1 : UINT8_MAX;
}

uint16_t AllocBufferMS::GetBackgroundFillPriority(uint8_t cls) const
{
    uint8_t preferredCls = GetHotHintClass();
    if (!IsTopFeedHotClass(cls, preferredCls)) {
        return 0;
    }
    if (CountCachedRegions(cls) >= kCachedRegionSlots) {
        return 0;
    }
    uint16_t score = GetHotClassScore(cls);
    if (score == 0) {
        return 0;
    }
    uint16_t miss = cacheMiss_[cls].load(std::memory_order_relaxed);
    uint16_t preferredBias = preferredCls < kNumSizeClasses && cls == preferredCls ? 4096 : 0;
    return static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, preferredBias + score + miss * 64));
}

bool AllocBufferMS::IsTopFeedHotClass(uint8_t cls, uint8_t preferredCls) const
{
    uint16_t score = GetHotClassScore(cls);
    bool selfPreferred = preferredCls < kNumSizeClasses && cls == preferredCls;
    if (score == 0 && !selfPreferred) {
        return false;
    }

    uint8_t outrank = 0;
    for (uint8_t other = 0; other < kNumSizeClasses; ++other) {
        if (other == cls) {
            continue;
        }
        bool otherPreferred = preferredCls < kNumSizeClasses && other == preferredCls;
        uint16_t otherScore = GetHotClassScore(other);
        if (otherScore == 0 && !otherPreferred) {
            continue;
        }
        if (otherPreferred != selfPreferred) {
            if (otherPreferred) {
                ++outrank;
            }
            continue;
        }
        if (otherScore > score || (otherScore == score && other < cls)) {
            ++outrank;
        }
        if (outrank >= kBackgroundFeedTopK) {
            return false;
        }
    }
    return true;
}

void AllocBufferMS::TrimExtraCachedRegions(uint8_t cls, uint8_t targetSlots)
{
    if (targetSlots == 0) {
        MSRegionInfo* extra = cachedRegion_[cls].exchange(nullptr, std::memory_order_acquire);
        if (extra != nullptr) {
            ReturnRegionToCentral(extra, cls);
        }
    }
}

bool AllocBufferMS::NeedsBackgroundFill(uint8_t preferredCls) const
{
    if (preferredCls >= kNumSizeClasses) {
        preferredCls = GetHotHintClass();
    }
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        if (!IsTopFeedHotClass(cls, preferredCls)) {
            continue;
        }
        if (CountCachedRegions(cls) != 0) {
            continue;
        }
        if (GetHotClassScore(cls) == 0) {
            continue;
        }
        return true;
    }
    return false;
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
        ObserveFeedHotness(cls, kFeedHotnessCachedHitDemand);
    } else {
        BoostCacheHeat(cls, kHeatBoostOnCacheMiss);
        uint8_t missCount = BumpCacheMiss(cls);
        ObserveFeedHotness(cls, kFeedHotnessMissDemand);
        if (missCount >= GetCacheFillThreshold(cls) && ShouldNotifyBackgroundFeed(cls)) {
            hotHintCls_.store(cls, std::memory_order_relaxed);
        }
        // Step 2: Get a new Region from CentralRegionPool
        newRegion = RefillFromCentral(cls);
        if (newRegion != nullptr) {
            BoostCacheHeat(cls, kHeatBoostOnCentralRefill);
            ObserveFeedHotness(cls, kFeedHotnessCentralRefillDemand);
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

MSRegionInfo* AllocBufferMS::TakeLocalEmptyRegion(uint8_t cls)
{
    MSRegionInfo* region = nullptr;
    {
        std::lock_guard<std::mutex> lg(localEmptyRegionLock_);
        region = localEmptyRegionCache_.TakeHead();
    }
    if (region == nullptr) {
        return nullptr;
    }
    central_->DecrementEmptySweptCount();

    CHECK_DETAIL(region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE,
                 "TakeLocalEmptyRegion got bad regionType=%u",
                 static_cast<unsigned>(region->regionType));

    size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
    uintptr_t regionStart = MSRegionInfoArray::GetRegionAddress(regionIdx);
    region->InitForSizeClass(cls);
    CHECK_DETAIL(region->allocBitmap != nullptr,
                 "TakeLocalEmptyRegion: InitForSizeClass failed for cls=%u idx=%zu",
                 static_cast<unsigned>(cls), regionIdx);
    region->allocPtr = regionStart;
    region->regionEnd = regionStart + MS_REGION_SIZE;
    region->SetSweepGen(CentralRegionPool::GetGlobalSweepGen());
    return region;
}

void AllocBufferMS::RefillLocalEmptyRegionsFromGlobal(std::vector<MSRegionInfo*>& refill)
{
    refill.clear();
    if (central_ == nullptr) {
        return;
    }

    size_t globalCount = central_->GetEmptySweptGlobalCount();
    size_t refillThreshold = std::max<size_t>(1, central_->GetAllocBufferCount()) * kLocalEmptyRegionCap;
    if (globalCount <= refillThreshold) {
        return;
    }

    size_t refillCount = std::min(kLocalEmptyRegionCap, globalCount - refillThreshold);
    if (refillCount == 0) {
        return;
    }

    central_->DrainEmptySweptGlobal(refillCount, refill);
    if (refill.size() <= 1) {
        return;
    }

    size_t cachedCount = 0;
    {
        std::lock_guard<std::mutex> lg(localEmptyRegionLock_);
        while (refill.size() > 1) {
            localEmptyRegionCache_.Prepend(refill.back());
            refill.pop_back();
            ++cachedCount;
        }
    }
    if (cachedCount > 0) {
        central_->IncrementEmptySweptCount(cachedCount);
    }
}

size_t AllocBufferMS::DrainAllLocalEmptyRegions(std::vector<MSRegionInfo*>& out)
{
    size_t drained = 0;
    {
        std::lock_guard<std::mutex> lg(localEmptyRegionLock_);
        for (;;) {
            MSRegionInfo* region = localEmptyRegionCache_.TakeHead();
            if (region == nullptr) {
                break;
            }
            out.push_back(region);
            ++drained;
        }
    }
    if (drained > 0) {
        central_->DecrementEmptySweptCount(drained);
    }
    return drained;
}

MSRegionInfo* AllocBufferMS::RefillEmptyRegionFromCaches(uint8_t cls)
{
    MSRegionInfo* region = TakeLocalEmptyRegion(cls);
    if (region != nullptr) {
        return region;
    }

    std::vector<MSRegionInfo*> refill;
    RefillLocalEmptyRegionsFromGlobal(refill);
    if (!refill.empty()) {
        region = refill.back();
        refill.pop_back();
    } else if (central_ != nullptr) {
        region = central_->TakeEmptySweptRegionFromGlobalOnly();
    }
    if (region == nullptr) {
        return nullptr;
    }

    CHECK_DETAIL(region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE,
                 "RefillEmptyRegionFromCaches got bad regionType=%u",
                 static_cast<unsigned>(region->regionType));

    size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
    uintptr_t regionStart = MSRegionInfoArray::GetRegionAddress(regionIdx);
    region->InitForSizeClass(cls);
    CHECK_DETAIL(region->allocBitmap != nullptr,
                 "RefillEmptyRegionFromCaches: InitForSizeClass failed for cls=%u idx=%zu",
                 static_cast<unsigned>(cls), regionIdx);
    region->allocPtr = regionStart;
    region->regionEnd = regionStart + MS_REGION_SIZE;
    region->SetSweepGen(CentralRegionPool::GetGlobalSweepGen());
    return region;
}

MSRegionInfo* AllocBufferMS::TakeCachedRegion(uint8_t cls)
{
    return cachedRegion_[cls].exchange(nullptr, std::memory_order_acquire);
}

bool AllocBufferMS::TryCacheRegion(uint8_t cls, MSRegionInfo* region, uint8_t slotLimit)
{
    if (slotLimit == 0) {
        return false;
    }
    MSRegionInfo* expected = nullptr;
    return cachedRegion_[cls].compare_exchange_strong(expected, region,
        std::memory_order_release, std::memory_order_relaxed);
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
    if (UNLIKELY(central_ == nullptr)) {
        return nullptr;
    }

    MSRegionAcquireSource source = MSRegionAcquireSource::NONE;
    MSRegionInfo* region = central_->GetRegion(cls, &source);
    if (region != nullptr) {
        return region;
    }

    if (!backgroundFill) {
        region = RefillEmptyRegionFromCaches(cls);
        if (region != nullptr) {
            return region;
        }
    } else {
        region = central_->TakeEmptySweptRegionFromGlobalOnly();
        if (region != nullptr) {
            CHECK_DETAIL(region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE,
                         "RefillFromCentral(bg): bad empty regionType=%u",
                         static_cast<unsigned>(region->regionType));
            size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
            uintptr_t regionStart = MSRegionInfoArray::GetRegionAddress(regionIdx);
            region->InitForSizeClass(cls);
            CHECK_DETAIL(region->allocBitmap != nullptr,
                         "RefillFromCentral(bg): InitForSizeClass failed for cls=%u idx=%zu",
                         static_cast<unsigned>(cls), regionIdx);
            region->allocPtr = regionStart;
            region->regionEnd = regionStart + MS_REGION_SIZE;
            region->SetSweepGen(CentralRegionPool::GetGlobalSweepGen());
            return region;
        }
    }

    region = central_->GetFreshRegion(cls);
    return region;
}

// ============================================================================
// BackgroundFillCache — O9: adaptive prefetch by gc-helper thread
// ============================================================================
MSAllocCacheFillStats AllocBufferMS::BackgroundFillCache(uint8_t preferredCls)
{
    MSAllocCacheFillStats stats;
    if (UNLIKELY(central_ == nullptr)) {
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

        bool topHot = IsTopFeedHotClass(cls, preferredCls);
        uint8_t targetSlots = topHot ? kCachedRegionSlots : 0;
        TrimExtraCachedRegions(cls, targetSlots);
        uint8_t cachedBefore = CountCachedRegions(cls);
        if (targetSlots == 0) {
            SetCacheMiss(cls, 0);
            CoolCacheHeat(cls, kHeatCoolOnIdleScan);
            DecayFeedHotness(cls);
            continue;
        }
        if (cachedBefore >= targetSlots) {
            SetCacheMiss(cls, 0);
            CoolCacheHeat(cls, kHeatCoolOnSaturatedCache);
            DecayFeedHotness(cls, 1);
            continue;
        }

        if (GetBackgroundFillPriority(cls) == 0) {
            CoolCacheHeat(cls, kHeatCoolOnIdleScan);
            DecayFeedHotness(cls);
            continue;
        }

        bool cacheFull = cachedBefore >= targetSlots;
        if (cachedBefore == 0) {
            MSRegionInfo* region = RefillFromCentral(cls, true);
            if (region != nullptr) {
                if (TryCacheRegion(cls, region, targetSlots)) {
                    stats.filledRegions++;
                    clsFilled = true;
                    cacheFull = true;
                } else {
                    central_->ReturnPartialRegion(region, cls);
                }
            }
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

    if (preferredCls < kNumSizeClasses && !NeedsBackgroundFill(preferredCls)) {
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

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
#include "Common/BaseObject.h"
#include "ObjectModel/MClass.inline.h"
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>

namespace MapleRuntime {

constexpr uint8_t AllocBufferMS::kCachedRegionSlots;

static constexpr bool kDisableMutatorAssistProbe = false;

uintptr_t AllocateFromRefilledRegion(MSRegionInfo* region)
{
    if (region == nullptr) {
        return 0;
    }
    return region->regionType == MSRegionType::MS_REGION_BUMP
        ? region->AllocBump()
        : region->AllocFromBitmapFast();
}

void LogRefillRegionAllocationFailure(const char* stage, uint8_t cls, const MSRegionInfo* region)
{
    if (region == nullptr) {
        LOG(RTLOG_ERROR, "[MS-GC] %s: refill region is null, cls=%u",
            stage, static_cast<unsigned>(cls));
        return;
    }

    LOG(RTLOG_ERROR,
        "[MS-GC] %s: refill region unusable, cls=%u, region=%p, regionType=%u, "
        "sweepState=%u, sizeClass=%u, freeSlots=%u/%u, sweepGen=%u, "
        "allocPtr=0x%zx, regionEnd=0x%zx, allocBitmap=%p, reserved=0x%llx",
        stage,
        static_cast<unsigned>(cls),
        static_cast<const void*>(region),
        static_cast<unsigned>(region->regionType),
        static_cast<unsigned>(region->sweepState),
        static_cast<unsigned>(region->sizeClass),
        static_cast<unsigned>(region->freeSlotCount),
        static_cast<unsigned>(region->totalSlotCount),
        static_cast<unsigned>(region->GetSweepGen()),
        region->allocPtr,
        region->regionEnd,
        static_cast<void*>(region->allocBitmap),
        static_cast<unsigned long long>(region->reserved));
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
    // AArch64 MSGC write-barrier fast path directly accesses wbBuf_ via a
    // fixed offset from AllocBufferMS. Keep this layout contract explicit.
    static_assert(offsetof(AllocBufferMS, wbBuf_) == 896,
                  "wbBuf_ offset changed; update AArch64 MSGC write-barrier fast path");
    static_assert(WriteBarrierBufferLayoutAccess::BufOffset() == 0,
                  "WriteBarrierBuffer entries must start at offset 0");
    static_assert(WriteBarrierBufferLayoutAccess::CountOffset() ==
                      WriteBarrierBuffer::kCapacity * sizeof(BaseObject*),
                  "WriteBarrierBuffer count_ layout changed; update AArch64 MSGC write-barrier fast path");
    static_assert(WriteBarrierBufferLayoutAccess::LayoutSize() ==
                      WriteBarrierBufferLayoutAccess::CountOffset() + sizeof(size_t),
                  "WriteBarrierBuffer trailing layout changed; update AArch64 MSGC write-barrier fast path");
    tlRegion_ = RegionInfo::NullRegion();
}

void AllocBufferMS::Init(CentralRegionPool* central)
{
    central_ = central;
    cachedHomeShardId_ = (central_ != nullptr)
        ? central_->ResolveHomeShardId()
        : kMSInvalidShardId;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        activeRegions_[cls] = nullptr;
        cachedRegionOrigin_[cls].store(static_cast<uint8_t>(RegionAcquireOrigin::NONE), std::memory_order_relaxed);
    }
    // Register with CentralRegionPool so GC can FlushAll
    if (central_ != nullptr) {
        central_->RegisterAllocBufferMS(this);
    }
}

void AllocBufferMS::Fini()
{
    MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
    if (collector != nullptr) {
        if (!wbBuf_.IsEmpty()) {
            collector->FlushExitedWriteBarrierBuffer(wbBuf_);
        }
        if (gcWork_.IsInitialized()) {
            gcWork_.Dispose();
            gcWork_.Reset();
        }
    } else {
        wbBuf_.Clear();
        if (gcWork_.IsInitialized()) {
            gcWork_.Reset();
        }
    }

    if (central_ != nullptr) {
        (void)ReturnAllRegionsToCentral();
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

void AllocBufferMS::TrimExtraCachedRegions(uint8_t cls, uint8_t targetSlots)
{
    if (targetSlots == 0) {
        MSRegionInfo* extra = cachedRegion_[cls].exchange(nullptr, std::memory_order_acquire);
        if (extra != nullptr) {
            (void)cachedRegionOrigin_[cls].exchange(static_cast<uint8_t>(RegionAcquireOrigin::NONE),
                                                    std::memory_order_acq_rel);
            ReturnRegionToCentral(extra, cls);
        }
    }
}

bool AllocBufferMS::NeedsBackgroundFill() const
{
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        if (activeRegions_[cls] != nullptr && CountCachedRegions(cls) == 0) {
            return true;
        }
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

    // O2: Refill directly from central / local empty-region caches.
    MSRegionInfo* newRegion = RefillFromCentral(cls, false, nullptr);
    if (UNLIKELY(newRegion == nullptr)) {
        return 0;  // OOM or GC needed — caller handles
    }

    uintptr_t addr = AllocateFromRefilledRegion(newRegion);
    if (UNLIKELY(addr == 0)) {
        // Protect against a region that was handed out by cache/central lists
        // but cannot satisfy even one real allocation attempt.
        LogRefillRegionAllocationFailure("SlowAlloc primary", cls, newRegion);
        central_->ReturnFullRegion(newRegion, cls);

        newRegion = central_->GetFreshRegion(cls, cachedHomeShardId_);
        if (UNLIKELY(newRegion == nullptr)) {
            return 0;
        }
        addr = AllocateFromRefilledRegion(newRegion);
        if (UNLIKELY(addr == 0)) {
            LogRefillRegionAllocationFailure("SlowAlloc fresh-retry", cls, newRegion);
            central_->ReturnFullRegion(newRegion, cls);
            return 0;
        }
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

    // Avoid doing another synchronous central-pool acquisition here: on case1 this
    // can turn one SlowAlloc miss into two central refills, and the second refill
    // may also fall into lazy sweep on the mutator thread.

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

MSRegionInfo* AllocBufferMS::TakeLocalEmptyRegion()
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
    return region;
}

bool AllocBufferMS::TryPushLocalEmptyRegion(MSRegionInfo* region, size_t slotLimit)
{
    if (region == nullptr || central_ == nullptr || slotLimit == 0) {
        return false;
    }

    region->ResetToEmptyCache();
    bool inserted = false;
    {
        std::lock_guard<std::mutex> lg(localEmptyRegionLock_);
        if (localEmptyRegionCache_.Count() < slotLimit) {
            localEmptyRegionCache_.Prepend(region);
            inserted = true;
        }
    }
    if (inserted) {
        central_->IncrementEmptySweptCount();
    }
    return inserted;
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
            MSRegionInfo* region = refill.back();
            localEmptyRegionCache_.Prepend(region);
            refill.pop_back();
            ++cachedCount;
        }
    }
    if (cachedCount > 0) {
        central_->IncrementEmptySweptCount(cachedCount);
    }
}

size_t AllocBufferMS::DrainLocalEmptyRegions(std::vector<MSRegionInfo*>& out, size_t maxRegions)
{
    if (maxRegions == 0) {
        return 0;
    }

    size_t drained = 0;
    {
        std::lock_guard<std::mutex> lg(localEmptyRegionLock_);
        for (;;) {
            if (drained >= maxRegions) {
                break;
            }
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

size_t AllocBufferMS::DrainAllLocalEmptyRegions(std::vector<MSRegionInfo*>& out)
{
    return DrainLocalEmptyRegions(out, std::numeric_limits<size_t>::max());
}

bool AllocBufferMS::HasLocalEmptyRegionInRange(size_t startRegionIdx, size_t endRegionIdx) const
{
    if (startRegionIdx >= endRegionIdx) {
        return false;
    }

    std::lock_guard<std::mutex> lg(localEmptyRegionLock_);
    bool found = false;
    localEmptyRegionCache_.ForEach([&](MSRegionInfo* region) {
        if (found || region == nullptr) {
            return;
        }
        size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
        if (regionIdx >= startRegionIdx && regionIdx < endRegionIdx) {
            found = true;
        }
    });
    return found;
}

size_t AllocBufferMS::ExtractLocalEmptyRegionsInRange(std::vector<MSRegionInfo*>& out,
                                                      size_t startRegionIdx,
                                                      size_t endRegionIdx,
                                                      size_t maxRegions)
{
    if (maxRegions == 0 || startRegionIdx >= endRegionIdx) {
        return 0;
    }

    size_t extracted = 0;
    std::vector<MSRegionInfo*> kept;
    {
        std::lock_guard<std::mutex> lg(localEmptyRegionLock_);
        for (;;) {
            MSRegionInfo* region = localEmptyRegionCache_.TakeHead();
            if (region == nullptr) {
                break;
            }
            size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
            if (extracted < maxRegions && regionIdx >= startRegionIdx && regionIdx < endRegionIdx) {
                out.push_back(region);
                ++extracted;
                continue;
            }
            kept.push_back(region);
        }
        for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
            localEmptyRegionCache_.Prepend(*it);
        }
    }

    if (extracted > 0 && central_ != nullptr) {
        central_->DecrementEmptySweptCount(extracted);
    }
    return extracted;
}

MSRegionInfo* AllocBufferMS::GetCacheRegion(uint8_t cls, RegionAcquireOrigin* originOut)
{
    MSRegionInfo* region = TakeLocalEmptyRegion();
    if (region == nullptr) {
        std::vector<MSRegionInfo*> refill;
        RefillLocalEmptyRegionsFromGlobal(refill);
        if (!refill.empty()) {
            region = refill.back();
            refill.pop_back();
        } else if (central_ != nullptr) {
            region = central_->TakeGlobalEmptyRegion();
        }
        if (region == nullptr) {
            return nullptr;
        }
    }

    if (originOut != nullptr) {
        *originOut = RegionAcquireOrigin::EMPTY_CACHE;
    }

    CHECK_DETAIL(region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE,
                 "GetCacheRegion got bad regionType=%u",
                 static_cast<unsigned>(region->regionType));

    region->InitForSizeClass(cls);
    region->SetSweepGen(CentralRegionPool::GetGlobalSweepGen());
    return region;
}

bool AllocBufferMS::TryCacheRegion(uint8_t cls, MSRegionInfo* region, RegionAcquireOrigin origin,
                                   uint8_t slotLimit)
{
    if (slotLimit == 0) {
        return false;
    }
    uint8_t expectedOrigin = static_cast<uint8_t>(RegionAcquireOrigin::NONE);
    if (!cachedRegionOrigin_[cls].compare_exchange_strong(expectedOrigin, static_cast<uint8_t>(origin),
            std::memory_order_relaxed, std::memory_order_relaxed)) {
        return false;
    }

    MSRegionInfo* expected = nullptr;
    if (!cachedRegion_[cls].compare_exchange_strong(expected, region,
            std::memory_order_release, std::memory_order_relaxed)) {
        cachedRegionOrigin_[cls].store(static_cast<uint8_t>(RegionAcquireOrigin::NONE), std::memory_order_relaxed);
        return false;
    }
    return true;
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

size_t AllocBufferMS::DrainCachedRegionsToCentral()
{
    if (central_ == nullptr) {
        return 0;
    }

    size_t drained = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        MSRegionInfo* region = cachedRegion_[cls].exchange(nullptr, std::memory_order_acquire);
        if (region == nullptr) {
            continue;
        }
        (void)cachedRegionOrigin_[cls].exchange(static_cast<uint8_t>(RegionAcquireOrigin::NONE),
                                                std::memory_order_acq_rel);
        ReturnRegionToCentral(region, cls);
        ++drained;
    }
    return drained;
}

size_t AllocBufferMS::ReturnAllRegionsToCentral()
{
    if (central_ == nullptr) {
        return 0;
    }

    size_t returned = DrainCachedRegionsToCentral();
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        MSRegionInfo* region = activeRegions_[cls];
        if (region == nullptr) {
            continue;
        }
        activeRegions_[cls] = nullptr;
        ReturnRegionToCentral(region, cls);
        ++returned;
    }
    return returned;
}

// ============================================================================
// RefillFromCentral — get a Region from CentralRegionPool
// ============================================================================
MSRegionInfo* AllocBufferMS::RefillFromCentral(uint8_t cls, bool backgroundFill,
                                               RegionAcquireOrigin* originOut)
{
    if (UNLIKELY(central_ == nullptr)) {
        return nullptr;
    }
    if (originOut != nullptr) {
        *originOut = RegionAcquireOrigin::NONE;
    }

    (void)central_->AssistSweepCredit(1, cls, this);

    MSRegionInfo* region = nullptr;
    if (!backgroundFill) {
        region = central_->TakePartialSweptRegion(cls);
        if (region != nullptr) {
            if (originOut != nullptr) {
                *originOut = RegionAcquireOrigin::SWEPT_READY;
            }
            return region;
        }

        region = GetCacheRegion(cls, originOut);
        if (region != nullptr) {
            return region;
        }

        region = central_->TakeUnsweptRegion(cls);
        if (region != nullptr) {
            if (originOut != nullptr) {
                *originOut = RegionAcquireOrigin::UNSWEPT;
            }
            return region;
        }
    } else {
        region = central_->TakeUnsweptRegion(cls);
        if (region != nullptr) {
            if (originOut != nullptr) {
                *originOut = RegionAcquireOrigin::UNSWEPT;
            }
            return region;
        }

        region = GetCacheRegion(cls, originOut);
        if (region != nullptr) {
            return region;
        }
    }

    region = central_->GetFreshRegion(cls, cachedHomeShardId_);
    if (region != nullptr && originOut != nullptr) {
        *originOut = RegionAcquireOrigin::FRESH;
    }
    return region;
}

// ============================================================================
// BackgroundFillCache — prefill one spare cached region for active size classes
// ============================================================================
MSAllocCacheFillStats AllocBufferMS::BackgroundFillCache()
{
    MSAllocCacheFillStats stats;
    if (UNLIKELY(central_ == nullptr)) {
        return stats;
    }

    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        bool clsFilled = false;
        uint8_t targetSlots = activeRegions_[cls] != nullptr ? kCachedRegionSlots : 0;
        TrimExtraCachedRegions(cls, targetSlots);
        uint8_t cachedBefore = CountCachedRegions(cls);
        if (targetSlots == 0) {
            continue;
        }
        if (cachedBefore >= targetSlots) {
            continue;
        }

        bool cacheFull = cachedBefore >= targetSlots;
        if (cachedBefore == 0) {
            RegionAcquireOrigin origin = RegionAcquireOrigin::NONE;
            MSRegionInfo* region = RefillFromCentral(cls, true, &origin);
            if (region != nullptr) {
                if (TryCacheRegion(cls, region, origin, targetSlots)) {
                    stats.filledRegions++;
                    clsFilled = true;
                    cacheFull = true;
                } else {
                    ReturnRegionToCentral(region, cls);
                }
            }
        }
        if (clsFilled) {
            stats.filledClasses++;
        }
        if (cacheFull) {
            continue;
        }
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

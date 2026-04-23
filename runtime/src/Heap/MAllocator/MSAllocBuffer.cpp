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
#include <cstdint>
#include <cstdio>

namespace MapleRuntime {

static inline bool IsMSMarkingPhase(GCPhase phase)
{
    return phase == GCPhase::GC_PHASE_MS_MARK_SETUP ||
           phase == GCPhase::GC_PHASE_MS_CONCURRENT_MARK ||
           phase == GCPhase::GC_PHASE_MS_MARK_TERMINATION;
}

static inline Mutator* ResolveMutatorForAllocFastPath(Mutator* mutator)
{
    return mutator != nullptr ? mutator : Mutator::GetMutator();
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
    FlushAll();
    // Unregister from CentralRegionPool before clearing
    if (central_ != nullptr) {
        central_->RemoveAllocBufferMS(this);
    }
    central_ = nullptr;
}

// ============================================================================
// Allocate — main entry point
// ============================================================================
MAddress AllocBufferMS::Allocate(size_t size, Mutator* mutator)
{
    uint8_t cls = SizeClassInfo::SizeToClass(size);
    if (UNLIKELY(cls >= kNumSizeClasses)) {
        return 0;  // Large object — caller handles it via the large-region path
    }
    MAddress addr = FastAlloc(cls);

    if (UNLIKELY(addr != 0)) {
        mutator = ResolveMutatorForAllocFastPath(mutator);
        GCPhase phase = mutator->GetMutatorPhase();
        if (UNLIKELY(IsMSMarkingPhase(phase))) {
            Heap::GetHeap().GetCollector().MarkNewObject(reinterpret_cast<BaseObject*>(addr));

            // Go-style dynamic pacing: debt += allocatedBytes * assistRatio.
            MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
            if (collector != nullptr) {
                uint64_t ratioScaled = collector->GetAssistWorkPerByteScaled();
                if (ratioScaled > 0) {
                    uint64_t deltaDebt = static_cast<uint64_t>(size) * ratioScaled;
                    if (UNLIKELY(gcAssistDebtScaled_ > UINT64_MAX - deltaDebt)) {
                        gcAssistDebtScaled_ = UINT64_MAX;
                    } else {
                        gcAssistDebtScaled_ += deltaDebt;
                    }

                    const uint64_t ratioScale = MarkSweepCollector::GetAssistRatioScale();
                    size_t debtObjects = static_cast<size_t>(gcAssistDebtScaled_ / ratioScale);
                    if (UNLIKELY(debtObjects >= collector->GetAssistDebtTriggerObjects())) {
                        // Call MSMarkAssist unconditionally: it first tries bgScanCredit
                        // (even when the mark work queue is empty), then does real marking
                        // only if credit is insufficient and grey objects are available.
                        // The old hasWork guard was wrong: it prevented credit consumption
                        // when the work queue happened to be empty, causing debt to pile up
                        // and trigger burst assist later.
                        size_t paidObjects = collector->MSMarkAssist(debtObjects);
                        size_t accounted = std::min(paidObjects, debtObjects);
                        gcAssistDebtScaled_ -= static_cast<uint64_t>(accounted) * ratioScale;
                    }
                }
            }
        } else {
            gcAssistDebtScaled_ = 0;
        }
    }

    return addr;
}

// ============================================================================
// FastAlloc — try allocation on the current active Region
// ============================================================================
MAddress AllocBufferMS::FastAlloc(uint8_t cls)
{
    MSRegionInfo* region = activeRegions_[cls];
    if (LIKELY(region != nullptr)) {
#ifdef MRT_DEBUG
        if (UNLIKELY(region->regionType == MSRegionType::MS_REGION_LARGE ||
                     region->regionType == MSRegionType::MS_REGION_FREE ||
                     region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE)) {
            LOG(RTLOG_FATAL, "[MS-GC] FastAlloc: activeRegion has bad type=%d, cls=%u, "
                "region=%p, allocPtr=0x%zx, regionEnd=0x%zx",
                (int)region->regionType, (unsigned)cls, (void*)region,
                region->allocPtr, region->regionEnd);
        }
#endif

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
    // Step 1: Return the exhausted Region to the central pool
    if (activeRegions_[cls] != nullptr) {
        ReturnRegion(cls);
    }

    // O2: Check thread-local cache before going to CentralRegionPool
    MSRegionInfo* newRegion = cachedRegion_[cls];
    if (newRegion != nullptr) {
        cachedRegion_[cls] = nullptr;
    } else {
        if (central_ != nullptr) {
            central_->MaybeNotifySweep(cls);
        }
        if (cacheMiss_[cls] < UINT8_MAX) {
            ++cacheMiss_[cls];
        }
        if (cacheMiss_[cls] >= kCacheFillThreshold) {
            Heap::GetHeap().GetCollectorResources().GetFinalizerProcessor().NotifyToFeedAllocBuffers();
        }
        // Step 2: Get a new Region from CentralRegionPool
        newRegion = RefillFromCentral(cls);
    }
    if (UNLIKELY(newRegion == nullptr)) {
        return 0;  // OOM or GC needed — caller handles
    }

    // Debug: validate newly-acquired region
#ifdef MRT_DEBUG
    {
        size_t span = newRegion->regionEnd - newRegion->allocPtr;
        size_t ridx = MSRegionInfoArray::GetRegionIdx(newRegion);
        uintptr_t expectedEnd = MSRegionInfoArray::GetRegionAddress(ridx) + MS_REGION_SIZE;
        if (UNLIKELY(newRegion->regionEnd != expectedEnd)) {
            LOG(RTLOG_FATAL, "[MS-GC] SlowAlloc: region has wrong regionEnd! "
                "regionEnd=0x%zx, expected=0x%zx, allocPtr=0x%zx, "
                "cls=%u, regionType=%d, sweepState=%d, sizeClass=%u, "
                "allocBitmap=%p, markBitmap=%p, "
                "totalSlots=%u, freeSlots=%u, "
                "nextRegionIdx=%d, prevRegionIdx=%d, "
                "sweepGen=%u, reserved=0x%llx, "
                "this=%p (idx=%zu)",
                newRegion->regionEnd, expectedEnd, newRegion->allocPtr,
                (unsigned)cls, (int)newRegion->regionType, (int)newRegion->sweepState,
                (unsigned)newRegion->sizeClass,
                (void*)newRegion->allocBitmap, (void*)newRegion->markBitmap,
                (unsigned)newRegion->totalSlotCount, (unsigned)newRegion->freeSlotCount,
                newRegion->nextRegionIdx, newRegion->prevRegionIdx,
                newRegion->sweepGen, (unsigned long long)newRegion->reserved,
                (void*)newRegion, ridx);
        }
        if (UNLIKELY(newRegion->allocBitmap == nullptr)) {
            LOG(RTLOG_FATAL, "[MS-GC] SlowAlloc: region has allocBitmap=nullptr, "
                "cls=%u, regionType=%d, allocPtr=0x%zx, regionEnd=0x%zx, "
                "this=%p (idx=%zu)",
                (unsigned)cls, (int)newRegion->regionType,
                newRegion->allocPtr, newRegion->regionEnd,
                (void*)newRegion, ridx);
        }
    }
#endif

    // Step 3: Set as active Region
    activeRegions_[cls] = newRegion;

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

    activeRegions_[cls] = nullptr;
}

// ============================================================================
// RefillFromCentral — get a Region from CentralRegionPool
// ============================================================================
MSRegionInfo* AllocBufferMS::RefillFromCentral(uint8_t cls)
{
    if (UNLIKELY(central_ == nullptr)) {
        return nullptr;
    }

    // Priority 1: partial Region with free slots (from CentralRegionPool's 4-list)
    MSRegionInfo* region = central_->GetRegion(cls);
    if (region != nullptr) {
        return region;
    }

    // Priority 2: brand-new Region from RegionManager
    region = central_->GetFreshRegion(cls);
    return region;
}

// ============================================================================
// FlushAll — GC STW: return all active Regions to CentralRegionPool
// ============================================================================
void AllocBufferMS::FlushAll()
{
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        // O2: Return cached backup regions first
        if (cachedRegion_[cls] != nullptr) {
            MSRegionInfo* cached = cachedRegion_[cls];
            cachedRegion_[cls] = nullptr;
            // markBitmap clearing is now handled by ClearAllMarkBitmaps
            // before STW, so no per-region ClearMarkBitmap here.
            if (cached->IsCompletelyFree()) {
                central_->ReturnEmptyRegion(cached);
            } else if (cached->HasFreeSpace()) {
                central_->ReturnPartialRegion(cached, cls);
            } else {
                central_->ReturnFullRegion(cached, cls);
            }
        }

        // Active region: return to pool (bitmap already cleared pre-STW)
        ReturnRegion(cls);
    }
}

// ============================================================================
// BackgroundFillCache — O9: adaptive prefetch by gc-helper thread
// ============================================================================
void AllocBufferMS::BackgroundFillCache()
{
    if (UNLIKELY(central_ == nullptr)) {
        return;
    }
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        if (cacheMiss_[cls] < kCacheFillThreshold) {
            continue;
        }
        if (cachedRegion_[cls].load(std::memory_order_relaxed) != nullptr) {
            cacheMiss_[cls] = 0;
            continue;
        }
        MSRegionInfo* region = central_->GetRegion(cls);
        if (region == nullptr) {
            central_->MaybeNotifySweep(cls);
            continue;
        }
        MSRegionInfo* expected = nullptr;
        if (cachedRegion_[cls].compare_exchange_strong(expected, region,
                std::memory_order_release, std::memory_order_relaxed)) {
            cacheMiss_[cls] = 0;
        } else {
            central_->ReturnPartialRegion(region, cls);
        }
    }
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
// AccumulatePacingDebt — pacing debt accumulation for paths bypassing Allocate()
// (e.g. large-object allocation in TryAllocateOnceMS)
// ============================================================================
void AllocBufferMS::AccumulatePacingDebt(size_t size)
{
    MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
    if (collector == nullptr) {
        return;
    }
    uint64_t ratioScaled = collector->GetAssistWorkPerByteScaled();
    if (ratioScaled == 0) {
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

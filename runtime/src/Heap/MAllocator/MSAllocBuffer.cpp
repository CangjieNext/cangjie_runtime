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
#include "CangjieRuntime.h"
#include "Mutator/Mutator.h"
#include "Base/Log.h"
#include "Collector/CollectorResources.h"
#include "Heap/MCollector/MarkSweepCollector.h"
#include "Common/BaseObject.h"
#include "ObjectModel/MClass.inline.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <thread>
#include <type_traits>

namespace MapleRuntime {

uintptr_t AllocateFromRefilledRegion(MSRegionInfo* region)
{
    if (region == nullptr) {
        return 0;
    }
    if (region->regionType == MSRegionType::MS_REGION_BUMP) {
        return region->AllocBump();
    }
    if (region->regionType == MSRegionType::MS_REGION_BITMAP) {
        return region->AllocFromBitmapFast();
    }
    return 0;
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
    static_assert(std::is_standard_layout<AllocBufferMS>::value,
                  "AllocBufferMS must remain standard-layout for compiler ABI checks");
    // Ensure tlRegion_ is at offset 0 — same as AllocBuffer::tlRegion.
    // LLVM fast-path reads *(buffer + 0) as RegionInfo* and calls Alloc().
    static_assert(offsetof(AllocBufferMS, tlRegion_) == 0,
                  "tlRegion_ must be at offset 0 for LLVM fast-path compatibility");
#if UINTPTR_MAX == UINT64_MAX
    static_assert(offsetof(AllocBufferMS, activeRegions_) == 8,
                  "activeRegions_ offset changed; update AArch64/X86 MSGC allocation fast path");
    static_assert(sizeof(activeRegions_[0]) == 8,
                  "activeRegions_ stride changed; update AArch64/X86 MSGC allocation fast path");
#endif
    static_assert(std::extent<decltype(activeRegions_)>::value == 36,
                  "activeRegions_ length changed; update AArch64/X86 MSGC allocation fast path");
    // The AArch64/X86 MSGC write-barrier fast paths load the WorkBuf pointer
    // activeLog_ (kept at offset 896), then appends an already shaded old-ref
    // into activeLog_->objs[len++]. These offsets are the ABI contract with
    // target emitters (activeLogOffsetInAllocBuffer / WorkBufLenOffset /
    // WorkBufObjsOffset / WorkBufCapacity). Keep them in sync.
    static_assert(offsetof(AllocBufferMS, activeLog_) == AllocBufferMS::kActiveLogOffset,
                  "activeLog_ offset changed; update AArch64/X86 MSGC write-barrier fast path");
    static_assert(offsetof(WorkBuf, len) == 8,
                  "WorkBuf::len offset changed; update AArch64/X86 MSGC write-barrier fast path");
    static_assert(offsetof(WorkBuf, capacity) == 12,
                  "WorkBuf::capacity offset changed; update AArch64/X86 MSGC write-barrier fast path");
    static_assert(sizeof(WorkBuf) == 16,
                  "WorkBuf object array offset changed; update AArch64/X86 MSGC write-barrier fast path");
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
        // Owner exiting mid-mark: publish its partial log so a consumer discovers
        // it, then return its active GC work buffers to the global pool.
        collector->PublishExitedLog(*this);
        if (gcWork_.IsInitialized()) {
            gcWork_.Dispose();
            gcWork_.Reset();
        }
    } else {
        // Not marking: the active log was already returned to the collector-global
        // pool by the cycle-end reset.
        activeLog_ = nullptr;
        if (gcWork_.IsInitialized()) {
            gcWork_.Reset();
        }
    }

    if (central_ != nullptr) {
        (void)ReturnAllRegionsToCentral();
    }
    if (central_ != nullptr) {
        central_->RemoveAllocBufferMS(this);
    }
    central_ = nullptr;
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

// ============================================================================
// Allocate — main entry point
// Pacing debt is no longer tracked per-object here.  It is consolidated into
// AccumulatePacingDebt(MS_REGION_SIZE) called once per region acquisition in
// SlowAlloc(), covering both the compiler fast-path (Allocate() bypassed) and
// the runtime slow-path uniformly.
// ============================================================================
MAddress AllocBufferMS::Allocate(size_t size)
{
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
        uintptr_t addr = 0;
        if (region->regionType == MSRegionType::MS_REGION_BUMP) {
            addr = region->AllocBump();
        } else if (region->regionType == MSRegionType::MS_REGION_BITMAP) {
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

    // O2: Refill directly from the central empty-region cache.
    MSRegionInfo* newRegion = RefillFromCentral(cls);
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
    AccumulateSweepDebt(1, cls);

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

    activeRegions_[cls] = nullptr;
    ReturnRegionToCentral(region, cls);
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

    MSCentralReturnDecision decision = region->PrepareForCentralReturn(cls);
    if (UNLIKELY(decision.callerClassMismatch)) {
        LOG(RTLOG_ERROR,
            "[MS-GC] ReturnRegionToCentral: caller cls=%u mismatches region sizeClass=%u, "
            "region=%p, normalizedType=%u, freeSlots=%u/%u, allocPtr=0x%zx, regionEnd=0x%zx",
            static_cast<unsigned>(cls),
            static_cast<unsigned>(decision.bucketSizeClass),
            static_cast<void*>(region),
            static_cast<unsigned>(region->regionType),
            static_cast<unsigned>(region->freeSlotCount),
            static_cast<unsigned>(region->totalSlotCount),
            region->allocPtr,
            region->regionEnd);
    }

    switch (decision.kind) {
        case MSCentralReturnKind::EMPTY_CACHE:
            central_->ReturnEmptyRegion(region);
            return;
        case MSCentralReturnKind::PARTIAL:
            central_->ReturnPartialRegion(region, decision.bucketSizeClass);
            return;
        case MSCentralReturnKind::FULL:
            central_->ReturnFullRegion(region, decision.bucketSizeClass);
            return;
    }
    MRT_ASSERT(false, "unexpected central return kind");
}

size_t AllocBufferMS::ReturnAllRegionsToCentral()
{
    if (central_ == nullptr) {
        return 0;
    }

    size_t returned = 0;
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
MSRegionInfo* AllocBufferMS::RefillFromCentral(uint8_t cls)
{
    if (UNLIKELY(central_ == nullptr)) {
        return nullptr;
    }
    MSRegionInfo* region = central_->TakePartialSweptRegion(cls);
    if (region != nullptr) {
        return region;
    }
    region = central_->TakeGlobalEmptyRegion();
    if (region != nullptr) {
        CHECK_DETAIL(region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE,
                     "RefillFromCentral got bad global empty regionType=%u",
                     static_cast<unsigned>(region->regionType));
        region->InitForSizeClass(cls);
        region->SetSweepGen(CentralRegionPool::GetGlobalSweepGen());
        return region;
    }
    region = central_->TakeUnsweptRegion(cls);
    if (region != nullptr) {
        return region;
    }
    return central_->GetFreshRegion(cls, cachedHomeShardId_);
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
    size_t workBufCapacity = CangjieRuntime::GetMSWorkBufScaleConfig().capacity;
    if (UNLIKELY(debtObjects >= workBufCapacity)) {
        size_t paidObjects = collector->MSMarkAssist(*this, debtObjects);
        size_t accounted = std::min(paidObjects, debtObjects);
        gcAssistDebtScaled_ -= static_cast<uint64_t>(accounted) * ratioScale;
    }
}

// ============================================================================
// AccumulateSweepDebt — P-local Sweep pacing
// Called only after a successful Region/span consumption. Background cache fill
// never reaches this function. A generation mismatch lazily discards the old
// P's fractional debt and statistics after PrepareForSweep publishes the new
// central ratio.
// ============================================================================
void AllocBufferMS::AccumulateSweepDebt(size_t allocatedRegions, uint8_t preferredCls)
{
    if (central_ == nullptr || allocatedRegions == 0) {
        return;
    }

    uint32_t currentGeneration = CentralRegionPool::GetGlobalSweepGen();
    uint32_t localGeneration = sweepDebtGen_;
    if (localGeneration != currentGeneration) {
        sweepAssistDebtScaled_ = 0;
        sweepDebtGen_ = currentGeneration;
    }

    // An emergency OOM force-finish intentionally does not publish
    // sweepDone_. If the admission is open but no unswept work remains, there
    // is nevertheless no debt left for this allocation buffer to repay.
    if (central_->IsSweepDone() || !central_->HasPendingSweepWork()) {
        sweepAssistDebtScaled_ = 0;
        return;
    }

    uint64_t ratioScaled = central_->GetSweepAssistRatioScaled();
    if (ratioScaled == 0) {
        sweepAssistDebtScaled_ = 0;
        return;
    }

    sweepAssistDebtScaled_ = AccumulateMSSweepDebtScaled(
        sweepAssistDebtScaled_, allocatedRegions, ratioScaled);

    size_t debtRegions = static_cast<size_t>(sweepAssistDebtScaled_ / kMSSweepAssistRatioScale);
    if (debtRegions == 0) {
        return;
    }

    MSSweepAssistResult result = central_->MSSweepAssist(debtRegions, preferredCls);
    if (result.sweepComplete) {
        sweepAssistDebtScaled_ = 0;
        return;
    }
    sweepAssistDebtScaled_ = PayMSSweepDebtRegions(
        sweepAssistDebtScaled_, result.TotalPaidRegions());
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

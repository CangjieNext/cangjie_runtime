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
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <thread>
#include <type_traits>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace MapleRuntime {
namespace {
void* AllocateMSAllocBufferStorage(size_t size) noexcept
{
#if defined(_WIN32)
    return _aligned_malloc(size, alignof(AllocBufferMS));
#else
    void* storage = nullptr;
    if (posix_memalign(&storage, alignof(AllocBufferMS), size) != 0) {
        return nullptr;
    }
    return storage;
#endif
}

void FreeMSAllocBufferStorage(void* storage) noexcept
{
#if defined(_WIN32)
    _aligned_free(storage);
#else
    free(storage);
#endif
}
} // namespace

uintptr_t AllocateFromRefilledRegion(MSRegionInfo* region)
{
    if (region == nullptr) {
        return 0;
    }
    if (region->GetAllocMode() == MSAllocMode::BUMP) {
        return region->AllocBump();
    }
    if (region->GetAllocMode() == MSAllocMode::BITMAP) {
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
        "allocMode=%u, sizeClass=%u, freeSlots=%u/%u, collectEpoch=%u, "
        "allocPtr=0x%zx, regionEnd=0x%zx, allocBitmap=%p, reserved=0x%llx",
        stage,
        static_cast<unsigned>(cls),
        static_cast<const void*>(region),
        static_cast<unsigned>(region->GetRegionType()),
        static_cast<unsigned>(region->GetAllocMode()),
        static_cast<unsigned>(region->sizeClass),
        static_cast<unsigned>(region->CountFreeSlots()),
        static_cast<unsigned>(region->GetTotalSlotCount()),
        static_cast<unsigned>(region->collectEpoch),
        region->allocPtr,
        region->regionEnd,
        static_cast<void*>(region->GetAllocBitmap()),
        static_cast<unsigned long long>(region->allocSummary));
}

AllocBufferMS::~AllocBufferMS()
{
    Fini();
}

void* AllocBufferMS::operator new(size_t size)
{
    void* storage = AllocateMSAllocBufferStorage(size);
    CHECK_DETAIL(storage != nullptr, "aligned AllocBufferMS allocation failed");
    return storage;
}

void* AllocBufferMS::operator new(size_t size, const std::nothrow_t&) noexcept
{
    return AllocateMSAllocBufferStorage(size);
}

void AllocBufferMS::operator delete(void* ptr) noexcept
{
    FreeMSAllocBufferStorage(ptr);
}

void AllocBufferMS::operator delete(void* ptr, size_t) noexcept
{
    FreeMSAllocBufferStorage(ptr);
}

void AllocBufferMS::operator delete(
    void* ptr, const std::nothrow_t&) noexcept
{
    FreeMSAllocBufferStorage(ptr);
}

AllocBufferMS::AllocBufferMS()
{
    static_assert(std::is_standard_layout<AllocBufferMS>::value,
                  "AllocBufferMS must remain standard-layout for compiler ABI checks");
    static_assert(alignof(AllocBufferMS) == 64,
                  "AllocBufferMS must remain cache-line aligned");
#if UINTPTR_MAX == UINT64_MAX
    static_assert(offsetof(AllocBufferMS, activeRegions_) == 0,
                  "activeRegions_ offset changed; update AArch64/X86 MSGC allocation fast path");
    static_assert(offsetof(AllocBufferMS, barrierMarkBuffer_) == 576,
                  "barrierMarkBuffer_ offset changed; update AArch64/X86 MSGC barrier fast path");
    static_assert(sizeof(activeRegions_[0][0]) == 8,
                  "activeRegions_ stride changed; update AArch64/X86 MSGC allocation fast path");
#endif
    static_assert(std::extent<decltype(activeRegions_)>::value == 36,
                  "activeRegions_ length changed; update AArch64/X86 MSGC allocation fast path");
    static_assert(std::extent<decltype(activeRegions_), 1>::value == kMSScanClassCount,
                  "activeRegions_ ScanClass dimension changed");
    static_assert(
        sizeof(activeRegions_) ==
            kNumSizeClasses * kMSScanClassCount * sizeof(MSRegionInfo*),
        "AllocBufferMS compiler-visible prefix must contain only active Region pointers");
    CHECK_DETAIL(
        (reinterpret_cast<uintptr_t>(this) & (alignof(AllocBufferMS) - 1)) == 0,
        "[MS-GC] AllocBufferMS instance is not cache-line aligned");
}

void AllocBufferMS::Init(CentralRegionPool* central)
{
    ResetMSBarrierMarkBuffer(barrierMarkBuffer_);
    ResetGCMarkAssistCycleState();
    central_ = central;
    cachedHomeShardId_ = (central_ != nullptr)
        ? central_->ResolveHomeShardId()
        : kMSInvalidShardId;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            activeRegions_[cls][scan] = nullptr;
        }
    }
    if (stackRoots_.capacity() < kRootStagingReserve) {
        stackRoots_.reserve(kRootStagingReserve);
    }
    if (rootOverflowFree_ == nullptr) {
        rootOverflowFree_ = new (std::nothrow) RootOverflowChunk();
        CHECK_DETAIL(rootOverflowFree_ != nullptr,
                     "[MS-GC] failed to preallocate root staging overflow chunk");
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
        collector->NotifyAllocBufferExit(*this);
    } else {
        // Without an active mark epoch, pending entries can only belong to the
        // completed cycle. They have no epoch tag and cannot be published into
        // a future mark plane.
        ResetMSBarrierMarkBuffer(barrierMarkBuffer_);
        ResetGCMarkAssistCycleState();
    }

    if (central_ != nullptr) {
        (void)ReturnAllRegionsToCentral();
    }
    if (central_ != nullptr) {
        central_->RemoveAllocBufferMS(this);
    }
    central_ = nullptr;
    stackRoots_.clear();
    DestroyRootOverflowChunks();
}

AllocBufferMS::RootOverflowChunk* AllocBufferMS::AcquireRootOverflowChunk()
{
    RootOverflowChunk* chunk = rootOverflowFree_;
    if (chunk != nullptr) {
        rootOverflowFree_ = chunk->next;
        chunk->next = nullptr;
        chunk->count = 0;
        return chunk;
    }
    chunk = new (std::nothrow) RootOverflowChunk();
    CHECK_DETAIL(chunk != nullptr,
                 "[MS-GC] root staging overflow allocation failed");
    return chunk;
}

void AllocBufferMS::PushRoot(BaseObject* root)
{
    if (stackRoots_.size() < stackRoots_.capacity()) {
        stackRoots_.emplace_back(root);
        return;
    }

    if (rootOverflowTail_ == nullptr ||
        rootOverflowTail_->count == kRootOverflowChunkCapacity) {
        RootOverflowChunk* chunk = AcquireRootOverflowChunk();
        if (rootOverflowTail_ == nullptr) {
            rootOverflowHead_ = chunk;
        } else {
            rootOverflowTail_->next = chunk;
        }
        rootOverflowTail_ = chunk;
    }
    rootOverflowTail_->roots[rootOverflowTail_->count++] = root;
}

void AllocBufferMS::DestroyRootOverflowChunks()
{
    RootOverflowChunk* lists[] = {rootOverflowHead_, rootOverflowFree_};
    rootOverflowHead_ = nullptr;
    rootOverflowTail_ = nullptr;
    rootOverflowFree_ = nullptr;
    for (RootOverflowChunk* chunk : lists) {
        while (chunk != nullptr) {
            RootOverflowChunk* next = chunk->next;
            delete chunk;
            chunk = next;
        }
    }
}

size_t AllocBufferMS::CountActiveRegions() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            if (activeRegions_[cls][scan] != nullptr) {
                ++total;
            }
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
MAddress AllocBufferMS::Allocate(size_t size, MSScanClass scanClass)
{
    uint8_t cls = SizeClassInfo::SizeToClass(size);
    if (UNLIKELY(cls >= kNumSizeClasses)) {
        return 0;  // Large object — caller handles it via the large-region path
    }
    return FastAlloc(cls, scanClass);
}

// ============================================================================
// FastAlloc — try allocation on the current active Region
// ============================================================================
MAddress AllocBufferMS::FastAlloc(uint8_t cls, MSScanClass scanClass)
{
    MSRegionInfo* region = activeRegions_[cls][ToMSScanClassIndex(scanClass)];
    if (LIKELY(region != nullptr)) {
        uintptr_t addr = 0;
        if (region->GetAllocMode() == MSAllocMode::BUMP) {
            addr = region->AllocBump();
        } else if (region->GetAllocMode() == MSAllocMode::BITMAP) {
            addr = region->AllocFromBitmapFast();
        }
        if (LIKELY(addr != 0)) {
            return static_cast<MAddress>(addr);
        }
    }

    // Current Region has no free slots (or no Region at all) — slow path
    return SlowAlloc(cls, scanClass);
}

// ============================================================================
// SlowAlloc — refill from CentralRegionPool
// ============================================================================
MAddress AllocBufferMS::SlowAlloc(uint8_t cls, MSScanClass scanClass)
{
    uint8_t scanIdx = ToMSScanClassIndex(scanClass);
    // Step 1: Return the exhausted Region to the central pool (if any).
    // Compiler allocation hits bypass Allocate(). On a miss, Runtime reloads
    // the same active Region here; if it is exhausted, SlowAlloc returns it
    // before installing a replacement.
    if (activeRegions_[cls][scanIdx] != nullptr) {
        ReturnRegion(cls, scanClass);
    }

    // O2: Refill directly from the central empty-region cache.
    MSRegionInfo* newRegion = RefillFromCentral(cls, scanClass);
    if (UNLIKELY(newRegion == nullptr)) {
        return 0;  // OOM or GC needed — caller handles
    }

    uintptr_t addr = AllocateFromRefilledRegion(newRegion);
    if (UNLIKELY(addr == 0)) {
        // Protect against a region that was handed out by cache/central lists
        // but cannot satisfy even one real allocation attempt.
        LogRefillRegionAllocationFailure("SlowAlloc primary", cls, newRegion);
        central_->ReturnFullRegion(newRegion, cls);

        newRegion = central_->GetFreshRegion(cls, scanClass, cachedHomeShardId_);
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
    //   - compiler fast-path: allocations in the previous Region happened
    //     inline, so no per-object debt was recorded.
    //   - runtime slow-path: Allocate() no longer tracks per-object debt; all debt
    //     is consolidated here at region-acquisition granularity.
    activeRegions_[cls][scanIdx] = newRegion;
    AccumulatePacingDebt(MS_REGION_SIZE);
    AccumulateSweepDebt(1, cls);

    // Avoid doing another synchronous central-pool acquisition here: on case1 this
    // can turn one SlowAlloc miss into two central refills.

    return static_cast<MAddress>(addr);
}

// ============================================================================
// ReturnRegion — classify and return active Region to CentralRegionPool
// ============================================================================
void AllocBufferMS::ReturnRegion(uint8_t cls, MSScanClass scanClass)
{
    uint8_t scanIdx = ToMSScanClassIndex(scanClass);
    MSRegionInfo* region = activeRegions_[cls][scanIdx];
    if (region == nullptr) {
        return;
    }

    activeRegions_[cls][scanIdx] = nullptr;
    ReturnRegionToCentral(region, cls);
}

void AllocBufferMS::ReturnRegionToCentral(MSRegionInfo* region, uint8_t cls)
{
    if (region == nullptr) {
        return;
    }

    MSCentralReturnDecision decision = region->PrepareForCentralReturn(cls);
    if (UNLIKELY(decision.callerClassMismatch)) {
        LOG(RTLOG_ERROR,
            "[MS-GC] ReturnRegionToCentral: caller cls=%u mismatches region sizeClass=%u, "
            "region=%p, normalizedType=%u, freeSlots=%u/%u, allocPtr=0x%zx, regionEnd=0x%zx",
            static_cast<unsigned>(cls),
            static_cast<unsigned>(decision.bucketSizeClass),
            static_cast<void*>(region),
            static_cast<unsigned>(region->GetRegionType()),
            static_cast<unsigned>(decision.freeSlots),
            static_cast<unsigned>(region->GetTotalSlotCount()),
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
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            MSRegionInfo* region = activeRegions_[cls][scan];
            if (region == nullptr) {
                continue;
            }
            activeRegions_[cls][scan] = nullptr;
            ReturnRegionToCentral(region, cls);
            ++returned;
        }
    }
    return returned;
}

// ============================================================================
// RefillFromCentral — get a Region from CentralRegionPool
// ============================================================================
MSRegionInfo* AllocBufferMS::RefillFromCentral(uint8_t cls, MSScanClass scanClass)
{
    if (UNLIKELY(central_ == nullptr)) {
        return nullptr;
    }
    MSRegionInfo* region = central_->TakePartialSweptRegion(cls, scanClass);
    if (region != nullptr) {
        return region;
    }
    region = central_->TakeGlobalEmptyRegion();
    if (region != nullptr) {
        CHECK_DETAIL(region->IsFreeRegion(),
                     "RefillFromCentral got bad global empty regionType=%u",
                     static_cast<unsigned>(region->GetRegionType()));
        region->InitForSizeClass(cls, scanClass);
        return region;
    }
    return central_->GetFreshRegion(cls, scanClass, cachedHomeShardId_);
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
        gcAssistCreditWU_ = 0;
        return;
    }
    uint64_t ratioScaled = collector->GetAssistWorkPerByteScaled();
    if (ratioScaled == 0) {
        gcAssistDebtScaled_ = 0;  // Marking not active — clear stale debt
        gcAssistCreditWU_ = 0;
        return;
    }
    uint64_t deltaDebt = SaturatingMSWUMul(
        static_cast<uint64_t>(size), ratioScaled);
    gcAssistDebtScaled_ = SaturatingMSWUAdd(
        gcAssistDebtScaled_, deltaDebt);
    const uint64_t ratioScale = MarkSweepCollector::GetAssistRatioScale();
    ConsumeMSMarkAssistCredit(
        gcAssistDebtScaled_, gcAssistCreditWU_, ratioScale);
    size_t debtWU = static_cast<size_t>(gcAssistDebtScaled_ / ratioScale);
    if (UNLIKELY(debtWU >= MS_WU_ASSIST_THRESHOLD)) {
        size_t completedWU = collector->MSMarkAssist(*this, debtWU);
        AccountMSMarkAssistWork(
            gcAssistDebtScaled_, gcAssistCreditWU_,
            static_cast<uint64_t>(completedWU), ratioScale);
    }
}

// ============================================================================
// AccumulateSweepDebt — P-local Sweep pacing
// Called only after a successful Region/span consumption. Background cache fill
// never reaches this function. A cycle-id mismatch lazily discards the old P's
// fractional debt and statistics after PostMark publishes the new central ratio.
// ============================================================================
void AllocBufferMS::AccumulateSweepDebt(size_t allocatedRegions, uint8_t preferredCls)
{
    if (central_ == nullptr || allocatedRegions == 0) {
        return;
    }

    uint32_t currentGeneration = CentralRegionPool::GetSweepCycleId();
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

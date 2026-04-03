// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSFreeManager.h"

#include <utility>
#ifndef _WIN64
#include <sys/mman.h>
#else
#include <windows.h>
#endif

#include "Base/MemUtils.h"
#include "Base/Log.h"
#include "Common/ScopedObjectAccess.h"

namespace MapleRuntime {

namespace {

class CartesianTreeBackend final : public FreeManager::Backend {
public:
    using UnitIndex = FreeManager::UnitIndex;
    using UnitCount = FreeManager::UnitCount;

    void Initialize(UnitCount maxUnitCount) override
    {
        tree_.Init(maxUnitCount);
    }

    void Finalize() noexcept override
    {
        tree_.Fini();
    }

    bool TakeUnits(UnitCount num, UnitIndex& idx) override
    {
        return tree_.TakeUnits(num, idx, false);
    }

    bool TakeTrailingUnits(UnitIndex targetEnd, UnitIndex& idx, UnitCount& num) override
    {
        return tree_.TakeRightmostIfEndEquals(targetEnd, idx, num);
    }

    bool MergeInsert(UnitIndex idx, UnitCount num) override
    {
        return tree_.MergeInsert(idx, num, false);
    }

    UnitCount GetTotalCount() const override
    {
        return tree_.GetTotalCount();
    }

    uint32_t GetNodeCount() const override
    {
        return tree_.GetNodeCount();
    }

    uint32_t GetMaxBlockCount() const override
    {
        return tree_.GetMaxBlockCount();
    }

    size_t GetMetadataBytesInUse() const override
    {
        return tree_.GetMetadataBytesInUse();
    }

    size_t GetNodeBytesInUse() const override
    {
        return tree_.GetNodeBytesInUse();
    }

    void CollectUnitSpans(std::vector<FreeManager::UnitSpan>& spans) const override
    {
        spans.clear();
        CartesianTree& tree = const_cast<CartesianTree&>(tree_);
        spans.reserve(tree.GetNodeCount());
        CartesianTree::Iterator it(tree);
        for (CartesianTree::Node* node = it.Next(); node != nullptr; node = it.Next()) {
            spans.push_back({ node->GetIndex(), node->GetCount() });
        }
    }

private:
    mutable CartesianTree tree_;
};

} // namespace

FreeManager::FreeManager() : FreeManager(nullptr)
{
}

FreeManager::FreeManager(std::unique_ptr<Backend> backend)
    : backend_(backend == nullptr ? std::make_unique<CartesianTreeBackend>() : std::move(backend))
{
}

FreeManager::~FreeManager()
{
    if (backend_ != nullptr) {
        backend_->Finalize();
    }
}

void FreeManager::Initialize(UnitCount maxUnitCount, size_t unitSize, size_t unitsPerRegion,
                             RegionCount committedRegionCount, RegionCount maxVirtualRegionCount,
                             uintptr_t heapStart, uintptr_t committedEnd, uintptr_t maxHeapEnd,
                             size_t segmentSize)
{
    CHECK_DETAIL(unitSize != 0, "[MS-GC] FreeManager unitSize must not be zero");
    CHECK_DETAIL(unitsPerRegion != 0, "[MS-GC] FreeManager unitsPerRegion must not be zero");
    CHECK_DETAIL(heapStart <= committedEnd, "[MS-GC] FreeManager committed end is below heap start");
    CHECK_DETAIL(committedEnd <= maxHeapEnd, "[MS-GC] FreeManager max heap end is below committed end");

    unitSize_ = unitSize;
    unitsPerRegion_ = unitsPerRegion;
    heapStart_ = heapStart;
    committedEnd_ = committedEnd;
    maxHeapEnd_ = maxHeapEnd;
    committedRegionCount_ = static_cast<size_t>(committedRegionCount);
    committedUnitCount_ = static_cast<size_t>(RegionCountToUnitCount(committedRegionCount));
    maxVirtualRegionCount_ = static_cast<size_t>(maxVirtualRegionCount);
    maxVirtualUnitCount_ = static_cast<size_t>(maxUnitCount);
    segmentSize_ = segmentSize;
    inactiveZone_.store(heapStart, std::memory_order_relaxed);
    GetBackend().Initialize(maxUnitCount);
}

FreeManager::TryTakeResult FreeManager::TryTakeRegions(RegionCount num, RegionIndex& idx)
{
    if (!mutex_.try_lock()) {
        return TryTakeResult::BUSY;
    }
    std::lock_guard<std::mutex> lg(mutex_, std::adopt_lock);

    UnitIndex unitIdx = 0;
    UnitCount unitCount = RegionCountToUnitCount(num);
    if (!GetBackend().TakeUnits(unitCount, unitIdx)) {
        return TryTakeResult::EMPTY;
    }

    idx = UnitToRegionIdx(unitIdx);
    return TryTakeResult::SUCCESS;
}

bool FreeManager::TakeTrailingRegions(RegionIndex targetEnd, RegionIndex& idx, RegionCount& num)
{
    std::lock_guard<std::mutex> lg(mutex_);

    UnitIndex unitIdx = 0;
    UnitCount unitCount = 0;
    if (!GetBackend().TakeTrailingUnits(RegionToUnitIdx(targetEnd), unitIdx, unitCount)) {
        return false;
    }

    idx = UnitToRegionIdx(unitIdx);
    num = UnitCountToRegionCount(unitCount);
    return true;
}

bool FreeManager::TryIncreaseInactiveZone(RegionCount num, RegionIndex& idx)
{
    size_t byteCount = static_cast<size_t>(num) * GetRegionSizeBytes();
    uintptr_t oldAddr = inactiveZone_.load(std::memory_order_relaxed);
    do {
        if (oldAddr + byteCount > committedEnd_) {
            return false;
        }
    } while (!inactiveZone_.compare_exchange_weak(oldAddr, oldAddr + byteCount,
                                                  std::memory_order_relaxed));

    idx = AddrToRegionIdx(oldAddr);
    return true;
}

void FreeManager::InsertRegions(RegionIndex idx, RegionCount num)
{
    std::lock_guard<std::mutex> lg(mutex_);

    UnitIndex unitIdx = RegionToUnitIdx(idx);
    UnitCount unitCount = RegionCountToUnitCount(num);
    if (!GetBackend().MergeInsert(unitIdx, unitCount)) {
        LOG(RTLOG_FATAL, "[MS-GC] FreeManager: failed to insert released regions [%u+%u, %u)",
            idx, num, idx + num);
    }
}

void FreeManager::ReleaseUnits(UnitIndex idx, UnitCount num) const
{
    uintptr_t addr = heapStart_ + static_cast<uintptr_t>(idx) * unitSize_;
    size_t size = static_cast<size_t>(num) * unitSize_;
#if defined(_WIN64)
    CHECK_E(UNLIKELY(!VirtualFree(reinterpret_cast<void*>(addr), size, MEM_DECOMMIT)),
            "VirtualFree failed in FreeManager::ReleaseUnits");
#elif defined(__APPLE__)
    MemorySet(addr, size, 0, size);
    void* ret = mmap(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (ret == MAP_FAILED) {
        LOG(RTLOG_ERROR, "[MS-GC] FreeManager::ReleaseUnits mmap fixed failed");
    }
#else
    (void)madvise(reinterpret_cast<void*>(addr), size, MADV_DONTNEED);
#endif
}

void FreeManager::ReclaimRegionsReleased(RegionIndex idx, RegionCount num)
{
    ReleaseUnits(RegionToUnitIdx(idx), RegionCountToUnitCount(num));
    ScopedEnterSaferegion enterSaferegion(true);
    InsertRegions(idx, num);
}

bool FreeManager::GrowOneSegment(HeapRangeHandler& handler)
{
    std::lock_guard<std::mutex> lg(growMutex_);

    if (committedEnd_ >= maxHeapEnd_) {
        return false;
    }

    uintptr_t oldCommittedEnd = committedEnd_;
    size_t growBytes = segmentSize_;
    if (oldCommittedEnd + growBytes > maxHeapEnd_) {
        growBytes = maxHeapEnd_ - oldCommittedEnd;
    }

    RegionCount growRegions = static_cast<RegionCount>(growBytes / GetRegionSizeBytes());
    UnitCount growUnits = static_cast<UnitCount>(growBytes / unitSize_);
    RegionIndex oldRegionCount = static_cast<RegionIndex>(committedRegionCount_);

    handler.InitializeFreshRegions(oldRegionCount, growRegions);

    committedRegionCount_ += static_cast<size_t>(growRegions);
    committedUnitCount_ += static_cast<size_t>(growUnits);
    committedEnd_ = oldCommittedEnd + growBytes;

    handler.OnCommittedHeapExtended(committedEnd_);
    heapGrowCount_++;

    return true;
}

size_t FreeManager::RewindInactiveZoneUsingReleasedTail()
{
    size_t reclaimedRegions = 0;
    while (true) {
        uintptr_t tail = inactiveZone_.load(std::memory_order_relaxed);
        if (tail <= heapStart_) {
            break;
        }

        RegionIndex targetEnd = AddrToRegionIdx(tail);
        RegionIndex idx = 0;
        RegionCount num = 0;
        if (!TakeTrailingRegions(targetEnd, idx, num)) {
            break;
        }

        uintptr_t newTail = RegionIdxToAddr(idx);
        if (!inactiveZone_.compare_exchange_strong(tail, newTail, std::memory_order_relaxed)) {
            ScopedEnterSaferegion enterSaferegion(true);
            InsertRegions(idx, num);
            break;
        }
        reclaimedRegions += num;
    }
    return reclaimedRegions;
}

void FreeManager::ShrinkHeap()
{
    std::lock_guard<std::mutex> lg(growMutex_);

    RewindInactiveZoneUsingReleasedTail();

    uintptr_t iz = inactiveZone_.load(std::memory_order_relaxed);
    if (committedEnd_ <= iz) {
        return;
    }

    size_t shrinkGap = committedEnd_ - iz;
    size_t shrinkSegments = shrinkGap / segmentSize_;
    if (shrinkSegments == 0) {
        return;
    }

    size_t shrinkBytes = shrinkSegments * segmentSize_;
    uintptr_t newCommitted = committedEnd_ - shrinkBytes;

    if (newCommitted < heapStart_ + segmentSize_) {
        newCommitted = heapStart_ + segmentSize_;
        if (newCommitted >= committedEnd_) {
            return;
        }
    }

    committedEnd_ = newCommitted;
    committedRegionCount_ = (newCommitted - heapStart_) / GetRegionSizeBytes();
    committedUnitCount_ = (newCommitted - heapStart_) / unitSize_;
}

FreeManager::UnitCount FreeManager::GetReleasedUnitCount() const
{
    std::lock_guard<std::mutex> lg(mutex_);
    return GetBackend().GetTotalCount();
}

FreeManager::UnitCount FreeManager::GetFreeUnitCount() const
{
    CheckAlignmentConfig();
    UnitCount releasedUnits = GetReleasedUnitCount();
    UnitCount inactiveUnits = 0;
    uintptr_t inactiveZone = inactiveZone_.load(std::memory_order_relaxed);
    if (committedEnd_ > inactiveZone) {
        inactiveUnits = static_cast<UnitCount>((committedEnd_ - inactiveZone) / unitSize_);
    }
    return releasedUnits + inactiveUnits;
}

size_t FreeManager::GetReleasedBytes() const
{
    std::lock_guard<std::mutex> lg(mutex_);
    return static_cast<size_t>(GetBackend().GetTotalCount()) * unitSize_;
}

uint32_t FreeManager::GetNodeCount() const
{
    std::lock_guard<std::mutex> lg(mutex_);
    return GetBackend().GetNodeCount();
}

uint32_t FreeManager::GetMaxBlockRegions() const
{
    std::lock_guard<std::mutex> lg(mutex_);
    return UnitCountToRegionCount(GetBackend().GetMaxBlockCount());
}

size_t FreeManager::GetMetadataBytesInUse() const
{
    std::lock_guard<std::mutex> lg(mutex_);
    return GetBackend().GetMetadataBytesInUse();
}

size_t FreeManager::GetNodeBytesInUse() const
{
    std::lock_guard<std::mutex> lg(mutex_);
    return GetBackend().GetNodeBytesInUse();
}

void FreeManager::CollectRegionSpans(std::vector<RegionSpan>& spans) const
{
    std::vector<UnitSpan> unitSpans;
    {
        std::lock_guard<std::mutex> lg(mutex_);
        GetBackend().CollectUnitSpans(unitSpans);
    }

    spans.clear();
    spans.reserve(unitSpans.size());
    for (const UnitSpan& span : unitSpans) {
        spans.push_back({ UnitToRegionIdx(span.idx), UnitCountToRegionCount(span.num) });
    }
}

FreeManager::UnitIndex FreeManager::RegionToUnitIdx(RegionIndex regionIdx) const
{
    CheckAlignmentConfig();
    return static_cast<UnitIndex>(static_cast<size_t>(regionIdx) * unitsPerRegion_);
}

FreeManager::UnitCount FreeManager::RegionCountToUnitCount(RegionCount regionCount) const
{
    CheckAlignmentConfig();
    return static_cast<UnitCount>(static_cast<size_t>(regionCount) * unitsPerRegion_);
}

FreeManager::RegionIndex FreeManager::UnitToRegionIdx(UnitIndex unitIdx) const
{
    CheckAlignmentConfig();
    CHECK_DETAIL(unitIdx % static_cast<UnitIndex>(unitsPerRegion_) == 0,
                 "[MS-GC] unit index is not region aligned: unitIdx=%u, unitsPerRegion=%zu",
                 unitIdx, unitsPerRegion_);
    return static_cast<RegionIndex>(static_cast<size_t>(unitIdx) / unitsPerRegion_);
}

FreeManager::RegionCount FreeManager::UnitCountToRegionCount(UnitCount unitCount) const
{
    CheckAlignmentConfig();
    CHECK_DETAIL(unitCount % static_cast<UnitCount>(unitsPerRegion_) == 0,
                 "[MS-GC] unit count is not region aligned: unitCount=%u, unitsPerRegion=%zu",
                 unitCount, unitsPerRegion_);
    return static_cast<RegionCount>(static_cast<size_t>(unitCount) / unitsPerRegion_);
}

FreeManager::Backend& FreeManager::GetBackend()
{
    CHECK_DETAIL(backend_ != nullptr, "[MS-GC] FreeManager backend must not be null");
    return *backend_;
}

const FreeManager::Backend& FreeManager::GetBackend() const
{
    CHECK_DETAIL(backend_ != nullptr, "[MS-GC] FreeManager backend must not be null");
    return *backend_;
}

void FreeManager::CheckAlignmentConfig() const
{
    CHECK_DETAIL(unitSize_ != 0, "[MS-GC] FreeManager unitSize must not be zero");
    CHECK_DETAIL(unitsPerRegion_ != 0, "[MS-GC] FreeManager unitsPerRegion must not be zero");
}

size_t FreeManager::GetRegionSizeBytes() const
{
    CheckAlignmentConfig();
    return unitSize_ * unitsPerRegion_;
}

FreeManager::RegionIndex FreeManager::AddrToRegionIdx(uintptr_t addr) const
{
    CHECK_DETAIL(addr >= heapStart_, "[MS-GC] address is below heap start: addr=0x%zx, heapStart=0x%zx",
                 addr, heapStart_);
    uintptr_t delta = addr - heapStart_;
    size_t regionSize = GetRegionSizeBytes();
    CHECK_DETAIL(delta % regionSize == 0,
                 "[MS-GC] address is not region aligned: addr=0x%zx, heapStart=0x%zx, regionSize=%zu",
                 addr, heapStart_, regionSize);
    return static_cast<RegionIndex>(delta / regionSize);
}

uintptr_t FreeManager::RegionIdxToAddr(RegionIndex regionIdx) const
{
    return heapStart_ + static_cast<uintptr_t>(regionIdx) * GetRegionSizeBytes();
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
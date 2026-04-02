// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSFreeManager.h"

#include <utility>

#include "Base/Log.h"

namespace MapleRuntime {

namespace {

class CartesianTreeBackend final : public FreeManager::Backend {
public:
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
    using UnitIndex = FreeManager::UnitIndex;
    using UnitCount = FreeManager::UnitCount;

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

void FreeManager::Initialize(UnitCount maxUnitCount, size_t unitSize, size_t unitsPerRegion)
{
    CHECK_DETAIL(unitSize != 0, "[MS-GC] FreeManager unitSize must not be zero");
    CHECK_DETAIL(unitsPerRegion != 0, "[MS-GC] FreeManager unitsPerRegion must not be zero");

    unitSize_ = unitSize;
    unitsPerRegion_ = unitsPerRegion;
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

FreeManager::UnitCount FreeManager::GetReleasedUnitCount() const
{
    std::lock_guard<std::mutex> lg(mutex_);
    return GetBackend().GetTotalCount();
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

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
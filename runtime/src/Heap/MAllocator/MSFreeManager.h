// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_FREE_MANAGER_H
#define MRT_MS_FREE_MANAGER_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "Heap/Allocator/CartesianTree.h"
#include "Base/Panic.h"

namespace MapleRuntime {

class FreeManager {
public:
    using UnitIndex = CartesianTree::Index;
    using UnitCount = CartesianTree::Count;
    using RegionIndex = CartesianTree::Index;
    using RegionCount = CartesianTree::Count;

    struct UnitSpan {
        UnitIndex idx = 0;
        UnitCount num = 0;
    };

    struct RegionSpan {
        RegionIndex idx = 0;
        RegionCount num = 0;
    };

    enum class TryTakeResult : uint8_t {
        SUCCESS,
        EMPTY,
        BUSY,
    };

    class Backend {
    public:
        virtual ~Backend() = default;

        virtual void Initialize(UnitCount maxUnitCount) = 0;
        virtual void Finalize() noexcept = 0;

        virtual bool TakeUnits(UnitCount num, UnitIndex& idx) = 0;
        virtual bool TakeTrailingUnits(UnitIndex targetEnd, UnitIndex& idx, UnitCount& num) = 0;
        virtual bool MergeInsert(UnitIndex idx, UnitCount num) = 0;

        virtual UnitCount GetTotalCount() const = 0;
        virtual uint32_t GetNodeCount() const = 0;
        virtual uint32_t GetMaxBlockCount() const = 0;
        virtual size_t GetMetadataBytesInUse() const = 0;
        virtual size_t GetNodeBytesInUse() const = 0;
        virtual void CollectUnitSpans(std::vector<UnitSpan>& spans) const = 0;
    };

    FreeManager();
    explicit FreeManager(std::unique_ptr<Backend> backend);
    ~FreeManager();

    void Initialize(UnitCount maxUnitCount, size_t unitSize, size_t unitsPerRegion);

    TryTakeResult TryTakeRegions(RegionCount num, RegionIndex& idx);
    bool TakeTrailingRegions(RegionIndex targetEnd, RegionIndex& idx, RegionCount& num);
    void InsertRegions(RegionIndex idx, RegionCount num);

    UnitCount GetReleasedUnitCount() const;
    size_t GetReleasedBytes() const;
    uint32_t GetNodeCount() const;
    uint32_t GetMaxBlockRegions() const;
    size_t GetMetadataBytesInUse() const;
    size_t GetNodeBytesInUse() const;
    void CollectRegionSpans(std::vector<RegionSpan>& spans) const;

    UnitIndex RegionToUnitIdx(RegionIndex regionIdx) const;
    UnitCount RegionCountToUnitCount(RegionCount regionCount) const;
    RegionIndex UnitToRegionIdx(UnitIndex unitIdx) const;
    RegionCount UnitCountToRegionCount(UnitCount unitCount) const;

private:
    Backend& GetBackend();
    const Backend& GetBackend() const;
    void CheckAlignmentConfig() const;

    mutable std::mutex mutex_;
    std::unique_ptr<Backend> backend_;
    size_t unitSize_ = 0;
    size_t unitsPerRegion_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_FREE_MANAGER_H
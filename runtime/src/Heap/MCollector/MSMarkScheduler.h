// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_SCHEDULER_H
#define MRT_MS_MARK_SCHEDULER_H

#ifdef MRT_USE_MARK_SWEEP

#include <atomic>
#include <cstdint>
#include <limits>
#include <utility>

#include "Base/Log.h"
#include "Base/TimeUtils.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MCollector/MSWorkUnit.h"

namespace MapleRuntime {

enum class MSMarkTaskKind : uint8_t {
    REGION,
    OBJECT,
};

struct MSMarkWorkerCaps {
    bool region = true;
    bool object = true;

    static MSMarkWorkerCaps GCWorker() { return {true, true}; }
    static MSMarkWorkerCaps Assist() { return {true, false}; }
};

class MSTaskLedger {
public:
    void AddProvisional()
    {
        uint64_t old = outstandingTotal_.fetch_add(1, std::memory_order_relaxed);
        CHECK_DETAIL(old != std::numeric_limits<uint64_t>::max(),
                     "[MS-GC] TaskLedger outstanding overflow");
    }

    void CancelProvisional()
    {
        uint64_t old = outstandingTotal_.fetch_sub(1, std::memory_order_relaxed);
        CHECK_DETAIL(old != 0, "[MS-GC] TaskLedger provisional underflow");
    }

    void Complete()
    {
        uint64_t old = outstandingTotal_.fetch_sub(1, std::memory_order_release);
        CHECK_DETAIL(old != 0, "[MS-GC] TaskLedger completion underflow");
    }

    uint64_t LoadHint() const
    {
        return outstandingTotal_.load(std::memory_order_relaxed);
    }

    uint64_t LoadForTermination() const
    {
        return outstandingTotal_.load(std::memory_order_acquire);
    }

    void ResetQuiescent()
    {
        CHECK_DETAIL(LoadForTermination() == 0,
                     "[MS-GC] reset non-empty TaskLedger: outstanding=%llu",
                     static_cast<unsigned long long>(LoadForTermination()));
    }

private:
    std::atomic<uint64_t> outstandingTotal_{0};
};

class MSMarkReadyStack {
public:
    // True means this push changed the lane from empty to non-empty.
    bool Push(MSRegionInfo* region);
    MSRegionInfo* Pop();

    bool IsEmpty() const;
    void ResetQuiescent();

private:
    static int32_t IdxOf(uint64_t value)
    {
        return static_cast<int32_t>(static_cast<uint32_t>(value));
    }
    static uint32_t TagOf(uint64_t value)
    {
        return static_cast<uint32_t>(value >> 32);
    }
    static uint64_t Pack(int32_t idx, uint32_t tag)
    {
        return (static_cast<uint64_t>(tag) << 32) |
            static_cast<uint64_t>(static_cast<uint32_t>(idx));
    }

    std::atomic<uint64_t> taggedHead_{
        Pack(MSRegionInfo::INVALID_IDX, 0)};
};

class MSTaskLease {
public:
    MSTaskLease() = default;
    MSTaskLease(const MSTaskLease&) = delete;
    MSTaskLease& operator=(const MSTaskLease&) = delete;

    MSTaskLease(MSTaskLease&& other) noexcept
    {
        MoveFrom(other);
    }

    MSTaskLease& operator=(MSTaskLease&& other) noexcept
    {
        CHECK_DETAIL(!active_,
                     "[MS-GC] overwriting active TaskLease idx=%u",
                     regionIdx_);
        MoveFrom(other);
        return *this;
    }

    ~MSTaskLease()
    {
        CHECK_DETAIL(!active_,
                     "[MS-GC] destroying active TaskLease kind=%u idx=%u",
                     static_cast<unsigned>(kind_), regionIdx_);
    }

    bool IsActive() const { return active_; }
    MSMarkTaskKind GetKind() const { return kind_; }
    uint32_t GetRegionIdx() const { return regionIdx_; }
    MSRegionInfo* GetRegionInfo() const
    {
        return active_
            ? MSRegionInfoArray::GetRegionInfoByIdx(regionIdx_)
            : nullptr;
    }

    void Complete(bool localBatchEmpty)
    {
        CHECK_DETAIL(active_, "[MS-GC] completing inactive TaskLease");
        CHECK_DETAIL(localBatchEmpty,
                     "[MS-GC] TaskLease completed with non-empty MarkBatch");
        ledger_->Complete();
        active_ = false;
        ledger_ = nullptr;
    }

private:
    friend class MSMarkScheduler;

    MSTaskLease(
        MSTaskLedger* ledger, MSMarkTaskKind kind, uint32_t regionIdx)
        : ledger_(ledger), kind_(kind), regionIdx_(regionIdx), active_(true)
    {
    }

    void MoveFrom(MSTaskLease& other)
    {
        ledger_ = other.ledger_;
        kind_ = other.kind_;
        regionIdx_ = other.regionIdx_;
        active_ = other.active_;
        other.ledger_ = nullptr;
        other.regionIdx_ = MSRegionInfo::INVALID_REGION_IDX;
        other.active_ = false;
    }

    MSTaskLedger* ledger_ = nullptr;
    MSMarkTaskKind kind_ = MSMarkTaskKind::REGION;
    uint32_t regionIdx_ = MSRegionInfo::INVALID_REGION_IDX;
    bool active_ = false;
};

struct MSMarkSliceResult {
    uint64_t completedWU = 0;
    size_t completedTasks = 0;
};

class MSMarkScheduler {
public:
    void ResetCycle();

    // Caller publishes pending bits before this call. Only an IDLE winner with
    // pending work creates a Region task. QUEUED/OWNED producers leave their
    // pending bit for the current owner. The owner rechecks after releasing to
    // IDLE, and MarkTermination performs the final STW reconciliation for the
    // residual release race. A delayed producer whose bit was already consumed
    // retracts its empty admission.
    // True means the ready lane changed from empty to non-empty.
    bool TryPublishRegion(MSRegionInfo* region);

    // Owner-budget replacement. The replacement must be published before the
    // caller completes its current lease.
    bool RequeueOwnedRegion(MSRegionInfo* region);

    // Caller must already own the LARGE marked winner.
    bool PublishObject(MSRegionInfo* largePrimary);

    bool TryClaim(
        const MSMarkWorkerCaps& caps, bool preferObject, MSTaskLease& lease);

    uint64_t OutstandingHint() const { return ledger_.LoadHint(); }
    uint64_t OutstandingForTermination() const
    {
        return ledger_.LoadForTermination();
    }
    bool ReadyEmptyForTermination() const
    {
        return regionReady_.IsEmpty() && objectReady_.IsEmpty();
    }

    template<typename Handler>
    MSMarkSliceResult RunMarkSlice(
        const MSMarkWorkerCaps& caps, uint64_t wuBudget,
        uint64_t wallBudgetUs, Handler&& handler)
    {
        MSMarkSliceResult result;
        uint64_t startUs = TimeUtil::MicroSeconds();
        bool preferObject = caps.object;
        while (result.completedWU < wuBudget) {
            if (result.completedTasks != 0 &&
                TimeUtil::MicroSeconds() - startUs >= wallBudgetUs) {
                break;
            }
            MSTaskLease lease;
            if (!TryClaim(caps, preferObject, lease)) {
                break;
            }
            preferObject = !preferObject;
            MSWorkUnitCounter work;
            handler(lease, work);
            CHECK_DETAIL(!lease.IsActive(),
                         "[MS-GC] RunMarkSlice handler retained TaskLease");
            result.completedWU =
                SaturatingMSWUAdd(result.completedWU, work.Total());
            ++result.completedTasks;
        }
        return result;
    }

private:
    bool TryClaimFrom(
        MSMarkReadyStack& ready, MSMarkTaskKind kind, MSTaskLease& lease);

    MSMarkReadyStack regionReady_;
    MSMarkReadyStack objectReady_;
    MSTaskLedger ledger_;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_MARK_SCHEDULER_H

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_SWEEP_EPOCH_COORDINATOR_H
#define MRT_MS_SWEEP_EPOCH_COORDINATOR_H

#ifdef MRT_USE_MARK_SWEEP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace MapleRuntime {

// Owns the Sweep epoch admission protocol. Region ownership and actual sweeping
// remain in CentralRegionPool; this class only linearizes who may sweep and
// when a closed epoch has no admitted workers left.
class MSSweepEpochCoordinator {
public:
    bool TryEnterSweep();
    void LeaveSweep();
    void CloseSweepAdmissionAndWait();
    void OpenSweepAdmission();
    void BeginEpoch(bool hasUnsweptWork);
    void PublishSweepDoneAfterQuiescence();

    bool IsSweepDone() const;
    bool IsAdmissionOpen() const;
    uint32_t GetEpochId() const;
    void SetForceFinishRunning(bool running);
    bool IsForceFinishRunning() const;

private:
    static constexpr uint64_t kAdmissionClosed = 1ULL << 63;
    static constexpr uint64_t kAdmissionActiveMask = ~kAdmissionClosed;

    std::atomic<uint64_t> admission_{0};
    std::atomic<bool> sweepDone_{true};
    std::atomic<bool> forceFinishRunning_{false};
    std::atomic<uint32_t> epochId_{1};
    std::mutex quiescenceMtx_;
    std::condition_variable quiescenceCV_;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_SWEEP_EPOCH_COORDINATOR_H

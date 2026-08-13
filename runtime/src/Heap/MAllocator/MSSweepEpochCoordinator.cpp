// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/MAllocator/MSSweepEpochCoordinator.h"

#include <cassert>
#include <chrono>

namespace MapleRuntime {
namespace {
constexpr uint64_t SWEEP_QUIESCENCE_SPIN_US = 20;

inline void SweepQuiescenceSpinPause()
{
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__)
    __asm__ __volatile__("pause");
#endif
}
} // namespace

bool MSSweepEpochCoordinator::TryEnterSweep()
{
    uint64_t oldState = admission_.load(std::memory_order_acquire);
    for (;;) {
        if ((oldState & kAdmissionClosed) != 0) {
            return false;
        }
        assert((oldState & kAdmissionActiveMask) != kAdmissionActiveMask);
        if (admission_.compare_exchange_weak(oldState, oldState + 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return true;
        }
    }
}

void MSSweepEpochCoordinator::LeaveSweep()
{
    uint64_t oldState = admission_.load(std::memory_order_acquire);
    for (;;) {
        assert((oldState & kAdmissionActiveMask) != 0);
        uint64_t newState = oldState - 1;
        if (admission_.compare_exchange_weak(oldState, newState,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            if ((newState & kAdmissionClosed) != 0 &&
                (newState & kAdmissionActiveMask) == 0) {
                std::lock_guard<std::mutex> lock(quiescenceMtx_);
                quiescenceCV_.notify_one();
            }
            return;
        }
    }
}

void MSSweepEpochCoordinator::CloseSweepAdmissionAndWait()
{
    admission_.fetch_or(kAdmissionClosed, std::memory_order_acq_rel);
    auto quiescent = [this]() {
        return (admission_.load(std::memory_order_acquire) & kAdmissionActiveMask) == 0;
    };
    if (quiescent()) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(SWEEP_QUIESCENCE_SPIN_US);
    do {
        SweepQuiescenceSpinPause();
        if (quiescent()) {
            return;
        }
    } while (std::chrono::steady_clock::now() < deadline);

    std::unique_lock<std::mutex> lock(quiescenceMtx_);
    quiescenceCV_.wait(lock, quiescent);
}

void MSSweepEpochCoordinator::OpenSweepAdmission()
{
    uint64_t state = admission_.load(std::memory_order_acquire);
    assert((state & kAdmissionActiveMask) == 0);
    assert((state & kAdmissionClosed) != 0);
    admission_.store(0, std::memory_order_release);
}

void MSSweepEpochCoordinator::BeginEpoch(bool hasUnsweptWork)
{
    assert((admission_.load(std::memory_order_acquire) & kAdmissionActiveMask) == 0);
    sweepDone_.store(!hasUnsweptWork, std::memory_order_release);
    forceFinishRunning_.store(false, std::memory_order_release);
    epochId_.fetch_add(1, std::memory_order_acq_rel);
}

void MSSweepEpochCoordinator::PublishSweepDoneAfterQuiescence()
{
    uint64_t state = admission_.load(std::memory_order_acquire);
    assert((state & kAdmissionClosed) != 0);
    assert((state & kAdmissionActiveMask) == 0);
    assert(!forceFinishRunning_.load(std::memory_order_acquire));
    sweepDone_.store(true, std::memory_order_release);
}

bool MSSweepEpochCoordinator::IsSweepDone() const
{
    return !forceFinishRunning_.load(std::memory_order_acquire) &&
        sweepDone_.load(std::memory_order_acquire) &&
        (admission_.load(std::memory_order_acquire) & kAdmissionActiveMask) == 0;
}

bool MSSweepEpochCoordinator::IsAdmissionOpen() const
{
    return (admission_.load(std::memory_order_acquire) & kAdmissionClosed) == 0;
}

uint32_t MSSweepEpochCoordinator::GetEpochId() const
{
    return epochId_.load(std::memory_order_acquire);
}

void MSSweepEpochCoordinator::SetForceFinishRunning(bool running)
{
    forceFinishRunning_.store(running, std::memory_order_release);
}

bool MSSweepEpochCoordinator::IsForceFinishRunning() const
{
    return forceFinishRunning_.load(std::memory_order_acquire);
}

} // namespace MapleRuntime

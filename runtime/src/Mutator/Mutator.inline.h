// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_MUTATOR_INLINE_H
#define MRT_MUTATOR_INLINE_H

#include "MutatorManager.h"

namespace MapleRuntime {
inline void Mutator::DoEnterSaferegion()
{
    // set current mutator in saferegion.
    SetInSaferegion(SAFE_REGION_TRUE);
}

inline bool Mutator::EnterSaferegion(bool updateUnwindContext) noexcept
{
    ThreadLocalData* tlData = ThreadLocal::GetThreadLocalData();
    bool shouldLog = UNLIKELY(MutatorManager::Instance().SyncTriggered() || HasAnySuspensionRequest() ||
                              (tlData != nullptr && tlData->safepointState != 0));
    if (shouldLog) {
        MRT_LogMutatorSaferegionBoundary("HOUDINI_SAFEPOINT EnterSaferegion.before", this, tlData, GetTid(),
                                         GetSuspensionFlag(), tlData == nullptr ? 0 : tlData->safepointState,
                                         static_cast<int>(InSaferegion()),
                                         static_cast<int>(MutatorManager::Instance().SyncTriggered()),
                                         static_cast<int>(updateUnwindContext));
    }
    if (LIKELY(!InSaferegion())) {
        // When the status is risky, no update is required. Because
        // valid information is already stored in the context.
        if (updateUnwindContext && uwContext.GetUnwindContextStatus() != UnwindContextStatus::RISKY) {
            UpdateUnwindContext();
        }
        DoEnterSaferegion();
        if (shouldLog) {
            MRT_LogMutatorSaferegionBoundary("HOUDINI_SAFEPOINT EnterSaferegion.after_true", this, tlData, GetTid(),
                                             GetSuspensionFlag(), tlData == nullptr ? 0 : tlData->safepointState,
                                             static_cast<int>(InSaferegion()),
                                             static_cast<int>(MutatorManager::Instance().SyncTriggered()),
                                             static_cast<int>(updateUnwindContext));
        }
        return true;
    }
    if (shouldLog) {
        MRT_LogMutatorSaferegionBoundary("HOUDINI_SAFEPOINT EnterSaferegion.after_false", this, tlData, GetTid(),
                                         GetSuspensionFlag(), tlData == nullptr ? 0 : tlData->safepointState,
                                         static_cast<int>(InSaferegion()),
                                         static_cast<int>(MutatorManager::Instance().SyncTriggered()),
                                         static_cast<int>(updateUnwindContext));
    }
    return false;
}

inline bool Mutator::LeaveSaferegion() noexcept
{
    ThreadLocalData* tlData = ThreadLocal::GetThreadLocalData();
    bool shouldLog = UNLIKELY(MutatorManager::Instance().SyncTriggered() || HasAnySuspensionRequest() ||
                              (tlData != nullptr && tlData->safepointState != 0));
    if (shouldLog) {
        MRT_LogMutatorSaferegionBoundary("HOUDINI_SAFEPOINT LeaveSaferegion.before", this, tlData, GetTid(),
                                         GetSuspensionFlag(), tlData == nullptr ? 0 : tlData->safepointState,
                                         static_cast<int>(InSaferegion()),
                                         static_cast<int>(MutatorManager::Instance().SyncTriggered()), 0);
    }
    if (LIKELY(InSaferegion())) {
        DoLeaveSaferegion();
        if (shouldLog) {
            MRT_LogMutatorSaferegionBoundary("HOUDINI_SAFEPOINT LeaveSaferegion.after_true", this, tlData, GetTid(),
                                             GetSuspensionFlag(), tlData == nullptr ? 0 : tlData->safepointState,
                                             static_cast<int>(InSaferegion()),
                                             static_cast<int>(MutatorManager::Instance().SyncTriggered()), 0);
        }
        return true;
    }
    if (shouldLog) {
        MRT_LogMutatorSaferegionBoundary("HOUDINI_SAFEPOINT LeaveSaferegion.after_false", this, tlData, GetTid(),
                                         GetSuspensionFlag(), tlData == nullptr ? 0 : tlData->safepointState,
                                         static_cast<int>(InSaferegion()),
                                         static_cast<int>(MutatorManager::Instance().SyncTriggered()), 0);
    }
    return false;
}

// Ensure that mutator phase is changed only once by mutator itself or GC
__attribute__((always_inline)) inline bool Mutator::TransitionGCPhase(bool bySelf)
{
    LOG(RTLOG_ERROR, "HOUDINI_TRANSGC TransitionGCPhase.enter mutator=%p tid=%u bySelf=%d transitionState=%d",
        this, GetTid(), static_cast<int>(bySelf), static_cast<int>(transitionState.load()));
    do {
        GCPhaseTransitionState state = transitionState.load();
        LOG(RTLOG_ERROR, "HOUDINI_TRANSGC TransitionGCPhase.loop_state mutator=%p tid=%u state=%d bySelf=%d",
            this, GetTid(), static_cast<int>(state), static_cast<int>(bySelf));
        if (state == FINISH_TRANSITION) {
            bool result = mutatorPhase.load() == Heap::GetHeap().GetGCPhase();
            LOG(RTLOG_ERROR, "HOUDINI_TRANSGC TransitionGCPhase.finish mutator=%p tid=%u result=%d",
                this, GetTid(), static_cast<int>(result));
            if (!bySelf && !result) {
                LOG(RTLOG_FATAL, "gc transition mutator %p (phase %u) to gc phase %u failed",
                    this, mutatorPhase.load(), Heap::GetHeap().GetGCPhase());
            }
            return result;
        }

        if (state == IN_TRANSITION) {
            LOG(RTLOG_ERROR, "HOUDINI_TRANSGC TransitionGCPhase.in_transition mutator=%p tid=%u bySelf=%d",
                this, GetTid(), static_cast<int>(bySelf));
            if (bySelf) {
                WaitForPhaseTransition();
                return true;
            } else {
                return false;
            }
        }

        if (!bySelf && state == NO_TRANSITION) {
            LOG(RTLOG_ERROR, "HOUDINI_TRANSGC TransitionGCPhase.no_transition mutator=%p tid=%u",
                this, GetTid());
            return true;
        }

        CHECK(state == NEED_TRANSITION);
        LOG(RTLOG_ERROR, "HOUDINI_TRANSGC TransitionGCPhase.try_exchange mutator=%p tid=%u",
            this, GetTid());
        if (transitionState.compare_exchange_weak(state, IN_TRANSITION)) {
            LOG(RTLOG_ERROR, "HOUDINI_TRANSGC TransitionGCPhase.exchange_success mutator=%p tid=%u",
                this, GetTid());
            TransitionToGCPhaseExclusive(Heap::GetHeap().GetGCPhase());
            LOG(RTLOG_ERROR, "HOUDINI_TRANSGC TransitionGCPhase.after_exclusive mutator=%p tid=%u",
                this, GetTid());
            transitionState.store(FINISH_TRANSITION, std::memory_order_release);
            LOG(RTLOG_ERROR, "HOUDINI_TRANSGC TransitionGCPhase.store_finish mutator=%p tid=%u",
                this, GetTid());
            return true;
        }
        LOG(RTLOG_ERROR, "HOUDINI_TRANSGC TransitionGCPhase.exchange_failed mutator=%p tid=%u",
            this, GetTid());
    } while (true);
}

// Ensure that mutator is changed only once by mutator itself or Profile
__attribute__((always_inline)) inline bool Mutator::TransitionToCpuProfile(bool bySelf)
{
    do {
        CpuProfileState state = cpuProfileState.load();
        // If this mutator profile has finished, just return
        if (state == FINISH_CPUPROFILE) {
            return true;
        }
        // If this mutator is executing profile by other thread, mutator should wait but profile just return
        if (state == IN_CPUPROFILING) {
            if (bySelf) {
                WaitForCpuProfiling();
                return true;
            } else {
                return false;
            }
        }
        if (!bySelf && state == NO_CPUPROFILE) {
            return true;
        }
        // Current thread set atomic variable to ensure atomicity of phase transition
        CHECK(state == NEED_CPUPROFILE);
        if (cpuProfileState.compare_exchange_weak(state, IN_CPUPROFILING)) {
            TransitionToCpuProfileExclusive();
            cpuProfileState.store(FINISH_CPUPROFILE, std::memory_order_release);
            return true;
        }
    } while (true);
}
} // namespace MapleRuntime

#endif // MRT_MUTATOR_INLINE_H

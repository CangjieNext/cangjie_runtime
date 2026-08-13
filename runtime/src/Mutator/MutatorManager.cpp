// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "MutatorManager.h"

#include <chrono>
#include <thread>
#include "Base/TimeUtils.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Collector/TracingCollector.h"
#include "Heap/Heap.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Mutator.inline.h"
#include "schedule.h"
#include "CpuProfiler/CpuProfiler.h"
namespace MapleRuntime {
namespace {
constexpr uint64_t STW_MUTATOR_ARRIVAL_SPIN_US = 20;
constexpr uint64_t GC_PHASE_TRANSITION_ARRIVAL_SPIN_US = 20;

inline void MutatorArrivalSpinPause()
{
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__)
    __asm__ __volatile__("pause");
#endif
}
} // namespace

extern "C" uintptr_t MRT_GetSafepointProtectedPage()
{
    return static_cast<uintptr_t>(true);
}

bool IsRuntimeThread()
{
    if (static_cast<int>(ThreadLocal::GetThreadType()) >= static_cast<int>(ThreadType::GC_THREAD)) {
        return true;
    }
    return false;
}

bool IsGcThread()
{
    if (static_cast<int>(ThreadLocal::GetThreadType()) == static_cast<int>(ThreadType::GC_THREAD)) {
        return true;
    }
    return false;
}

extern "C" void HandleSafepoint(ThreadLocalData* tlData)
{
    Mutator* mutator = tlData->mutator;
    // Current mutator enter saferegion
    mutator->DoEnterSaferegion();
    // Current mutator block before leaving saferegion
    mutator->DoLeaveSaferegion();
    DLOG(SIGNAL, "HandleSafepoint, thread restarted.");
}

#if defined (__arm__)
extern "C" void HandleSafepointForArm(ThreadLocalData* tlData)
{
    if (tlData->safepointState == 0) {
        return;
    }
    Mutator* mutator = tlData->mutator;
    // Current mutator enter saferegion
    mutator->DoEnterSaferegion();
    // Current mutator block before leaving saferegion
    mutator->DoLeaveSaferegion();
    DLOG(SIGNAL, "HandleSafepoint, thread restarted.");
}
#endif

void MutatorManager::BindMutator(Mutator& mutator) const
{
    ThreadLocalData* tlData = ThreadLocal::GetThreadLocalData();
    Allocator::EnsureThreadLocalBuffer();
    mutator.SetSafepointStatePtr(&tlData->safepointState);
    mutator.SetSafepointActive(false);
    ThreadLocal::SetMutator(&mutator);
}

void MutatorManager::UnbindMutator(Mutator& mutator) const
{
    ThreadLocalData* tlData = ThreadLocal::GetThreadLocalData();
    MRT_ASSERT(tlData->mutator == &mutator, "mutator in ThreadLocalData doesn't match in cjthread");
    (void)tlData;
    ThreadLocal::SetMutator(nullptr);
    mutator.SetSafepointStatePtr(nullptr);
}

Mutator* MutatorManager::CreateMutator()
{
    Mutator* mutator = ConcurrencyModel::GetMutator();
    if (mutator == nullptr) {
        mutator = new (std::nothrow) Mutator();
        CHECK_DETAIL(mutator != nullptr, "new Mutator failed");
        MutatorManagementRLock();
        mutator->Init();
        mutator->InitTid();
        BindMutator(*mutator);
        mutator->SetMutatorPhase(Heap::GetHeap().GetGCPhase());
        ConcurrencyModel::SetMutator(mutator);
    } else {
        MutatorManagementRLock();
        mutator->Init();
        mutator->InitTid();
        BindMutator(*mutator);
        mutator->SetMutatorPhase(Heap::GetHeap().GetGCPhase());
    }
    MutatorManagementRUnlock();
    return mutator;
}

void MutatorManager::TransitMutatorToExit()
{
    Mutator* mutator = Mutator::GetMutator();
    CHECK_DETAIL(mutator != nullptr, "Mutator has not initialized or has been fini: %p", mutator);
    // Enter saferegion to avoid blocking gc stw
    mutator->MutatorLock();
    mutator->ResetMutator();
    mutator->MutatorUnlock();
    (void)mutator->EnterSaferegion(false);
    UnbindMutator(*mutator);
}

void MutatorManager::DestroyExpiredMutators()
{
    expiringMutatorListLock.lock();
    ExpiredMutatorList workList;
    workList.swap(expiringMutators);
    expiringMutatorListLock.unlock();
    for (auto it = workList.begin(); it != workList.end(); ++it) {
        Mutator* expiringMutator = *it;
        delete expiringMutator;
    }
}

void MutatorManager::DestroyMutator(Mutator* mutator)
{
    if (TryAcquireMutatorManagementRLock()) {
        delete mutator; // call ~Mutator() under mutatorListLock
        MutatorManagementRUnlock();
    } else {
        expiringMutatorListLock.lock();
        expiringMutators.push_back(mutator);
        expiringMutatorListLock.unlock();
    }
}

Mutator* MutatorManager::CreateRuntimeMutator(ThreadType threadType)
{
    // Because TSAN tool can't identify the RwLock implemented by ourselves,
    // we use a global instance fpMutatorInstance instead of an instance created on
    // heap in order to prevent false positives.
    static Mutator fpMutatorInstance;
    Mutator* mutator = nullptr;
    if (threadType == ThreadType::FP_THREAD) {
        mutator = &fpMutatorInstance;
    } else {
        mutator = new (std::nothrow) Mutator();
    }
    CHECK_DETAIL(mutator != nullptr, "create mutator out of native memory");
    MutatorManagementRLock();
    mutator->Init();
    mutator->InitTid();
    mutator->InitProtectStackAddr();
    mutator->SetManagedContext(false);
    MutatorManager::Instance().BindMutator(*mutator);
    mutator->SetMutatorPhase(Heap::GetHeap().GetGCPhase());
    ThreadLocal::SetMutator(mutator);
    ThreadLocal::SetThreadType(threadType);
    ThreadLocal::SetCJProcessorFlag(true);
    ThreadLocalData* threadData = reinterpret_cast<ThreadLocalData*>(MRT_GetThreadLocalData());
    MRT_PreRunManagedCode(mutator, 2, threadData); // 2 layers
    // only running mutator can enter saferegion.
    MutatorManagementRUnlock();
    return mutator;
}

void MutatorManager::DestroyRuntimeMutator(ThreadType threadType)
{
    Mutator* mutator = ThreadLocal::GetMutator();
    CHECK_DETAIL(mutator != nullptr, "Fini UpdateThreads with null mutator");

    MutatorManagementRLock();
    (void)mutator->LeaveSaferegion();
    // fp mutator is a static instance, we can't delete it, we reset the mutator to avoid invalid memory
    // access when static instance destruction.
    if (threadType != ThreadType::FP_THREAD) {
        delete mutator;
    } else {
        mutator->ResetMutator();
    }
    ThreadLocal::SetAllocBuffer(nullptr);
#ifdef MRT_USE_MARK_SWEEP
    // Clean up per-thread MS alloc buffer
    AllocBufferMS* msBuffer = ThreadLocal::GetAllocBufferMS();
    if (msBuffer != nullptr) {
        delete msBuffer;  // ~AllocBufferMS calls Fini() which unregisters from CentralRegionPool
        ThreadLocal::SetAllocBufferMS(nullptr);
    }
#endif
    ThreadLocal::SetMutator(nullptr);
    ThreadLocal::SetCJProcessorFlag(false);
    MutatorManagementRUnlock();
}

void MutatorManager::Init()
{
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
    safepointPageManager = new (std::nothrow) SafepointPageManager();
    CHECK_DETAIL(safepointPageManager != nullptr, "new safepointPageManager failed");
    safepointPageManager->Init();
#endif
}

MutatorManager& MutatorManager::Instance() noexcept { return Runtime::Current().GetMutatorManager(); }

void MutatorManager::AcquireMutatorManagementWLock()
{
    uint64_t start = TimeUtil::NanoSeconds();
    bool acquired = TryAcquireMutatorManagementWLock();
    while (!acquired) {
        TimeUtil::SleepForNano(WAIT_LOCK_INTERVAL);
        acquired = TryAcquireMutatorManagementWLock();
        uint64_t now = TimeUtil::NanoSeconds();
        if (!acquired && ((now - start) / SECOND_TO_NANO_SECOND > WAIT_LOCK_TIMEOUT)) {
            LOG(RTLOG_FATAL, "Wait mutator list lock timeout");
        }
    }
}

bool MutatorManager::AcquireMutatorManagementWLockForCpuProfile()
{
    uint64_t start = TimeUtil::NanoSeconds();
    bool acquired = TryAcquireMutatorManagementWLock();
    while (!acquired) {
        TimeUtil::SleepForNano(WAIT_LOCK_INTERVAL);
        acquired = TryAcquireMutatorManagementWLock();
        uint64_t now = TimeUtil::NanoSeconds();
        if (!acquired && ((now - start) / SECOND_TO_NANO_SECOND > WAIT_LOCK_TIMEOUT)) {
            LOG(RTLOG_FATAL, "Wait mutator list lock timeout");
        }
        if (!CpuProfiler::GetInstance().GetGenerator().GetIsStart()) {
            break;
        }
    }
    return acquired;
}

// Visit all mutators, hold mutatorListLock firstly
void MutatorManager::VisitAllMutators(MutatorVisitor func)
{
    ScheduleAllCJThreadVisitMutator(VisitMuatorHelper, &func);
    Mutator* mutator = Heap::GetHeap().GetFinalizerProcessor().GetMutator();
    if (mutator != nullptr) {
        func(*mutator);
    }
#ifdef MRT_USE_MARK_SWEEP
    Mutator* backgroundMutator = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor().GetMutator();
    if (backgroundMutator != nullptr) {
        func(*backgroundMutator);
    }
#endif
}

void MutatorManager::NotifyMutatorEnteredSaferegion()
{
    std::lock_guard<std::mutex> lock(mutatorArrivalMtx);
    mutatorArrivalCV.notify_one();
}

void MutatorManager::NotifyPhaseTransitionProgress()
{
    // Pair with EnsurePhaseTransition's mutex-protected predicate recheck.
    // The state/saferegion publication happens before taking this mutex, so a
    // waiter either observes the new predicate or receives this notification.
    std::lock_guard<std::mutex> lock(phaseTransitionArrivalMtx);
    phaseTransitionArrivalCV.notify_one();
}

void MutatorManager::StopTheWorld(bool syncGCPhase, GCPhase phase, STWAcquireStats* stats)
{
    if (stats != nullptr) {
        *stats = {};
    }
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool saferegionEntered = false;
    // Ensure an active mutator entered saferegion before STW (aka. stop all other mutators).
    if (!IsGcThread()) {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr) {
            saferegionEntered = mutator->EnterSaferegion(true);
        }
    }
#endif
    // Block if another thread is holding the syncMutex.
    // Prevent multi-thread doing STW concurrently.
    uint64_t stageStartUs = stats != nullptr ? TimeUtil::MicroSeconds() : 0;
    syncMutex.lock();
    if (stats != nullptr) {
        stats->syncMutexWaitUs = TimeUtil::MicroSeconds() - stageStartUs;
    }
    syncTriggered.store(true);

    stageStartUs = stats != nullptr ? TimeUtil::MicroSeconds() : 0;
    AcquireMutatorManagementWLock();
    if (stats != nullptr) {
        stats->mutatorLockWaitUs = TimeUtil::MicroSeconds() - stageStartUs;
    }

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // If current mutator saferegion state changed,
    // we should restore it after the mutator called StartTheWorld().
    saferegionStateChanged = saferegionEntered;
#endif

    stageStartUs = stats != nullptr ? TimeUtil::MicroSeconds() : 0;
    size_t mutatorCount = GetMutatorCount();
    if (stats != nullptr) {
        stats->mutatorCountUs = TimeUtil::MicroSeconds() - stageStartUs;
        stats->mutatorCount = mutatorCount;
    }
    if (UNLIKELY(mutatorCount == 0)) {
        worldStopped.store(true, std::memory_order_release);
        if (syncGCPhase) {
            stageStartUs = stats != nullptr ? TimeUtil::MicroSeconds() : 0;
            TransitionAllMutatorsToGCPhase(phase);
            if (stats != nullptr) {
                stats->phaseTransitionUs = TimeUtil::MicroSeconds() - stageStartUs;
            }
        }
        return;
    }
    // set mutatorCount as countOfMutatorsToStop.
    SetSuspensionMutatorCount(static_cast<uint32_t>(mutatorCount));
    stageStartUs = stats != nullptr ? TimeUtil::MicroSeconds() : 0;
    DemandSuspensionForSync();
    if (stats != nullptr) {
        stats->suspensionBroadcastUs = TimeUtil::MicroSeconds() - stageStartUs;
    }
    WaitUntilAllMutatorStopped(stats);

    // the world is stopped.
    worldStopped.store(true, std::memory_order_release);
    if (syncGCPhase) {
        stageStartUs = stats != nullptr ? TimeUtil::MicroSeconds() : 0;
        TransitionAllMutatorsToGCPhase(phase);
        if (stats != nullptr) {
            stats->phaseTransitionUs = TimeUtil::MicroSeconds() - stageStartUs;
        }
    }
}

void MutatorManager::StartTheWorld() noexcept
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool shouldLeaveSaferegion = saferegionStateChanged;
#endif
    syncTriggered.store(false);
    worldStopped.store(false, std::memory_order_release);

    CancelSuspensionAfterSync();
    SetSuspensionMutatorCount(0);

    // wakeup all mutators which blocking on countOfMutatorsToStop futex.
#if defined(_WIN64) || defined(__APPLE__)
    WakeAllMutators();
#else
    (void)MapleRuntime::Futex(GetSyncFutexWord(), FUTEX_WAKE, INT_MAX);
#endif

    MutatorManagementWUnlock();

    // Release syncMutex to allow other thread call STW.
    syncMutex.unlock();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // Restore saferegion state if the state is changed when mutator calls StopTheWorld().
    if (!IsGcThread()) {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr && shouldLeaveSaferegion) {
            (void)mutator->LeaveSaferegion();
        }
    }
#endif
}

void MutatorManager::StartLightSync(bool syncGCPhase, GCPhase phase)
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool saferegionEntered = false;
    // Ensure an active mutator entered saferegion before stw.
    if (!IsGcThread()) {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr) {
            saferegionEntered = mutator->EnterSaferegion(true);
        }
    }
#endif
    // Block if another thread is holding the syncMutex.
    // Prevent multi-thread doing lsync concurrently.
    syncMutex.lock();
    syncTriggered.store(true);

    AcquireMutatorManagementWLock();

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // If current mutator saferegion state changed,
    // we should restore it after the mutator called StopLightSync().
    saferegionStateChanged = saferegionEntered;
#endif

    size_t mutatorCount = GetMutatorCount();
    if (UNLIKELY(mutatorCount == 0)) {
        worldStopped.store(true, std::memory_order_release);
    } else {
        // set mutatorCount as countOfMutatorsToStop.
        SetSuspensionMutatorCount(static_cast<uint32_t>(mutatorCount));
        DemandSuspensionForSync();
        WaitUntilAllMutatorStopped();
        worldStopped.store(true, std::memory_order_release);
    }

    DLOG(GCPHASE, "transition gc: %s(%u) -> %s(%u)",
         Collector::GetGCPhaseName(Heap::GetHeap().GetGCPhase()), Heap::GetHeap().GetGCPhase(),
         Collector::GetGCPhaseName(phase), phase);

    // Set global gc phase in the scope of mutatorlist lock
    Heap::GetHeap().InstallBarrier(phase);
    Heap::GetHeap().SetGCPhase(phase);
    lightSyncGCPhase = phase;
    undoneLightSyncMutators.clear();
    // Broadcast mutator phase transition signal to all mutators
    VisitAllMutators([this](Mutator& mutator) {
        mutator.SetSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_GC_PHASE);
        mutator.SetSafepointActive(true);
        this->undoneLightSyncMutators.push_back(&mutator);
    });
}

void MutatorManager::StopLightSync() noexcept
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool shouldLeaveSaferegion = saferegionStateChanged;
#endif

    syncTriggered.store(false);
    worldStopped.store(false, std::memory_order_release);

    CancelSuspensionAfterSync();
    SetSuspensionMutatorCount(0);

    // wakeup all mutators which blocking on countOfMutatorsToStop futex.
#if defined(_WIN64) || defined(__APPLE__)
    WakeAllMutators();
#else
    (void)MapleRuntime::Futex(GetSyncFutexWord(), FUTEX_WAKE, INT_MAX);
#endif
    EnsurePhaseTransition(lightSyncGCPhase, undoneLightSyncMutators);
    MutatorManagementWUnlock();
    // Release syncMutex to allow other thread call lsync.
    syncMutex.unlock();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // Restore saferegion state if the state is changed when mutator calls StartLightSync().
    if (!IsGcThread()) {
        Mutator* mutator = Mutator::GetMutator();
        if (mutator != nullptr && shouldLeaveSaferegion) {
            (void)mutator->LeaveSaferegion();
        }
    }
#endif
}

void MutatorManager::WaitUntilAllMutatorStopped(STWAcquireStats* stats)
{
    uint64_t snapshotStartUs = stats != nullptr ? TimeUtil::MicroSeconds() : 0;
    std::list<Mutator*> unstoppedMutators;
    auto func = [&unstoppedMutators](Mutator& mutator) {
        if ((!mutator.InSaferegion())) {
            unstoppedMutators.emplace_back(&mutator);
        }
    };
    VisitAllMutators(func);

    size_t remainMutatorsSize = unstoppedMutators.size();
    if (stats != nullptr) {
        stats->unstoppedSnapshotUs = TimeUtil::MicroSeconds() - snapshotStartUs;
        stats->initialUnstoppedMutators = remainMutatorsSize;
    }
    if (remainMutatorsSize == 0) {
        return;
    }

    // Synchronize operation to ensure that all mutators complete phase transition
    // Use unstoppedMutators to avoid traversing the entire mutatorList
    uint64_t waitStartUs = stats != nullptr ? TimeUtil::MicroSeconds() : 0;
    auto removeStoppedMutators = [&unstoppedMutators]() {
        for (auto it = unstoppedMutators.begin(); it != unstoppedMutators.end();) {
            Mutator* mutator = *it;
            if (mutator->InSaferegion()) {
                it = unstoppedMutators.erase(it);
            } else {
                ++it;
            }
        }
    };
    auto finishWait = [stats, waitStartUs]() {
        if (stats != nullptr) {
            stats->safepointWaitUs = TimeUtil::MicroSeconds() - waitStartUs;
        }
    };

    // Most safepoint handshakes finish within a few microseconds.  Keep the GC
    // runnable for a bounded interval, but use only processor hints here: an OS
    // sched_yield can delay this latency-critical thread for a full timeslice.
    uint64_t spinDeadlineNs = TimeUtil::NanoSeconds() + STW_MUTATOR_ARRIVAL_SPIN_US * 1000;
    do {
        removeStoppedMutators();
        if (unstoppedMutators.empty()) {
            finishWait();
            return;
        }
        MutatorArrivalSpinPause();
    } while (TimeUtil::NanoSeconds() < spinDeadlineNs);

    const uint64_t timeoutMs =
        ((remainMutatorsSize / STW_TIMEOUTS_THREADS_BASE_COUNT) * STW_TIMEOUTS_BASE_MS) + STW_TIMEOUTS_BASE_MS;
    auto timeoutDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    int timeoutTimes = 0;
    std::unique_lock<std::mutex> arrivalLock(mutatorArrivalMtx);
    while (true) {
        // Recheck while holding mutatorArrivalMtx. A mutator publishes its safe
        // state before acquiring this mutex to notify, so it cannot race through
        // the predicate-to-wait transition and lose the wakeup.
        removeStoppedMutators();
        if (unstoppedMutators.empty()) {
            finishWait();
            return;
        }

        if (mutatorArrivalCV.wait_until(arrivalLock, timeoutDeadline) == std::cv_status::timeout) {
            timeoutTimes++;
            arrivalLock.unlock();
            DumpMutators(timeoutTimes);
            arrivalLock.lock();
            timeoutDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        }
    }
}

void MutatorManager::EnsurePhaseTransition(GCPhase phase, std::list<Mutator*> &undoneMutators)
{
    // Traverse through undoneMutators to select mutators that have not yet completed transition
    // 1. ignore mutators which have completed transition
    // 2. gc compete phase transition with mutators which are in saferegion
    // 3. fill mutators which are running state in undoneMutators
    auto processTransitions = [&]() {
        for (auto it = undoneMutators.begin(); it != undoneMutators.end();) {
            Mutator* mutator = *it;
            if (mutator->GetMutatorPhase() == phase && mutator->FinishedTransition()) {
                it = undoneMutators.erase(it);
                continue;
            }
            if (mutator->InSaferegion() && mutator->TransitionGCPhase(false)) {
                it = undoneMutators.erase(it);
                continue;
            }
            ++it;
        }
    };

    // Preserve the low-latency path for Mutators that are already at a
    // safepoint, but do not let the GC thread busy-scan a delayed Mutator for
    // an unbounded time.
    uint64_t spinDeadlineNs = TimeUtil::NanoSeconds() +
        GC_PHASE_TRANSITION_ARRIVAL_SPIN_US * 1000;
    do {
        processTransitions();
        if (undoneMutators.empty()) {
            return;
        }
        MutatorArrivalSpinPause();
    } while (TimeUtil::NanoSeconds() < spinDeadlineNs);

    for (;;) {
        processTransitions();
        if (undoneMutators.empty()) {
            return;
        }

        std::unique_lock<std::mutex> arrivalLock(phaseTransitionArrivalMtx);
        bool transitionReady = false;
        for (auto it = undoneMutators.begin(); it != undoneMutators.end();) {
            Mutator* mutator = *it;
            if (mutator->GetMutatorPhase() == phase && mutator->FinishedTransition()) {
                it = undoneMutators.erase(it);
                continue;
            }
            transitionReady = transitionReady || mutator->InSaferegion();
            ++it;
        }
        if (undoneMutators.empty()) {
            return;
        }
        if (transitionReady) {
            continue;
        }
        phaseTransitionArrivalCV.wait(arrivalLock);
    }
}

void MutatorManager::TransitionAllMutatorsToGCPhase(GCPhase phase)
{
#ifdef MRT_USE_MARK_SWEEP
    TransitionAllMutatorsToMarkSweepGCPhase(phase);
#else
    TransitionAllMutatorsToCopyGCPhase(phase);
#endif
}

#ifdef MRT_USE_MARK_SWEEP
void MutatorManager::TransitionAllMutatorsToMarkSweepGCPhase(GCPhase phase)
{
    const bool enableEnumAssist =
        phase == GCPhase::GC_PHASE_MS_MARK_SETUP &&
        CangjieRuntime::IsMSEnumAssistEnabled();
    // Try to occupy mutatorListLock prevent some mutators from exiting
    bool worldStopped = WorldStopped();
    if (!worldStopped) {
        AcquireMutatorManagementWLock();
    }

    DLOG(GCPHASE, "transition gc: %s(%u) -> %s(%u)",
         Collector::GetGCPhaseName(Heap::GetHeap().GetGCPhase()), Heap::GetHeap().GetGCPhase(),
         Collector::GetGCPhaseName(phase), phase);

    // Set global gc phase in the scope of mutatorlist lock.
    Heap::GetHeap().InstallBarrier(phase);
    Heap::GetHeap().SetGCPhase(phase);

    std::list<Mutator*> undoneMutators;
    std::vector<Mutator*> enumAssistCandidates;
    if (enableEnumAssist) {
        enumAssistCandidates.reserve(GetMutatorCount());
    }
    // Broadcast mutator phase transition signal to all mutators
    VisitAllMutators([&](Mutator& mutator) {
        mutator.SetSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_GC_PHASE);
        mutator.SetSafepointActive(true);
        if (enableEnumAssist) {
            enumAssistCandidates.push_back(&mutator);
        }
        undoneMutators.push_back(&mutator);
    });
    if (enableEnumAssist) {
        std::lock_guard<std::mutex> guard(enumAssistContext_.lock);
        CHECK_DETAIL(enumAssistContext_.phase == GCPhase::GC_PHASE_UNDEF &&
                     enumAssistContext_.inFlight == 0,
                     "preparing an active EnumRoots assist context");
        enumAssistContext_.phase = phase;
        enumAssistContext_.claimCursor = 0;
        enumAssistContext_.inFlight = 0;
        enumAssistContext_.candidates.swap(enumAssistCandidates);
    }
    EnsurePhaseTransition(phase, undoneMutators);
    if (enableEnumAssist) {
        FinalizeEnumAssistContext(phase);
        VisitAllMutators([](Mutator& mutator) {
            mutator.ClearSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_GC_ENUM_ASSIST);
            if (!mutator.HasAnySuspensionRequest() && !mutator.HasObserver()) {
                mutator.SetSafepointActive(false);
            }
        });
    }
    if (!worldStopped) {
        MutatorManagementWUnlock();
    }
}

#else
void MutatorManager::TransitionAllMutatorsToCopyGCPhase(GCPhase phase)
{
    // Try to occupy mutatorListLock prevent some mutators from exiting.
    bool worldStopped = WorldStopped();
    if (!worldStopped) {
        AcquireMutatorManagementWLock();
    }

    DLOG(GCPHASE, "transition gc: %s(%u) -> %s(%u)",
         Collector::GetGCPhaseName(Heap::GetHeap().GetGCPhase()), Heap::GetHeap().GetGCPhase(),
         Collector::GetGCPhaseName(phase), phase);

    // CopyGC has no EnumRoots assist context: install the barrier and let each
    // mutator perform the ordinary phase transition handshake.
    Heap::GetHeap().InstallBarrier(phase);
    Heap::GetHeap().SetGCPhase(phase);

    std::list<Mutator*> undoneMutators;
    VisitAllMutators([&undoneMutators](Mutator& mutator) {
        mutator.SetSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_GC_PHASE);
        mutator.SetSafepointActive(true);
        undoneMutators.push_back(&mutator);
    });
    EnsurePhaseTransition(phase, undoneMutators);

    if (!worldStopped) {
        MutatorManagementWUnlock();
    }
}
#endif

#ifdef MRT_USE_MARK_SWEEP
bool MutatorManager::HasPendingEnumAssist(GCPhase phase)
{
    if (phase != GCPhase::GC_PHASE_MS_MARK_SETUP) {
        return false;
    }
    std::lock_guard<std::mutex> guard(enumAssistContext_.lock);
    return enumAssistContext_.phase == phase &&
        enumAssistContext_.claimCursor < enumAssistContext_.candidates.size();
}

size_t MutatorManager::ClaimEnumAssistBatch(
    GCPhase phase, size_t batchSize, std::vector<Mutator*>& claimed)
{
    claimed.clear();
    if (phase != GCPhase::GC_PHASE_MS_MARK_SETUP || batchSize == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> guard(enumAssistContext_.lock);
    if (enumAssistContext_.phase != phase ||
        enumAssistContext_.claimCursor == enumAssistContext_.candidates.size()) {
        return 0;
    }

    claimed.reserve(batchSize);
    while (claimed.size() < batchSize &&
           enumAssistContext_.claimCursor < enumAssistContext_.candidates.size()) {
        claimed.push_back(
            enumAssistContext_.candidates[enumAssistContext_.claimCursor++]);
    }
    if (!claimed.empty()) {
        ++enumAssistContext_.inFlight;
    }
    return claimed.size();
}

void MutatorManager::FinishEnumAssistBatch(GCPhase phase)
{
    if (phase != GCPhase::GC_PHASE_MS_MARK_SETUP) {
        return;
    }
    std::lock_guard<std::mutex> guard(enumAssistContext_.lock);
    if (enumAssistContext_.inFlight > 0 && --enumAssistContext_.inFlight == 0) {
        enumAssistContext_.drained.notify_all();
    }
}

size_t MutatorManager::AssistEnumRoots(
    GCPhase phase, size_t batchSize, Mutator* assistant)
{
    if (!HasPendingEnumAssist(phase) || batchSize == 0) {
        return 0;
    }
    std::vector<Mutator*> claimed;
    claimed.reserve(batchSize);
    if (ClaimEnumAssistBatch(phase, batchSize, claimed) == 0) {
        return 0;
    }

    for (Mutator* candidate : claimed) {
        if (candidate == nullptr || candidate == assistant ||
            !candidate->InSaferegion()) {
            continue;
        }
        (void)candidate->TransitionGCPhase(false);
    }
    FinishEnumAssistBatch(phase);
    return claimed.size();
}

void MutatorManager::FinalizeEnumAssistContext(GCPhase phase)
{
    if (phase != GCPhase::GC_PHASE_MS_MARK_SETUP) {
        return;
    }
    std::unique_lock<std::mutex> lock(enumAssistContext_.lock);
    if (enumAssistContext_.phase != phase) {
        return;
    }
    enumAssistContext_.drained.wait(lock, [this] {
        return enumAssistContext_.inFlight == 0;
    });
    enumAssistContext_.phase = GCPhase::GC_PHASE_UNDEF;
    enumAssistContext_.claimCursor = 0;
    enumAssistContext_.candidates.clear();
}
#endif

void MutatorManager::EnsureCpuProfileFinish(std::list<Mutator*> &undoneMutators)
{
    while (undoneMutators.size() > 0) {
        for (auto it = undoneMutators.begin(); it != undoneMutators.end();) {
            Mutator* mutator = *it;
            if (mutator->FinishedCpuProfile()) {
                it = undoneMutators.erase(it);
                continue;
            }
            if (mutator->InSaferegion() && mutator->TransitionToCpuProfile(false)) {
                it = undoneMutators.erase(it);
                continue;
            }
            if (!CpuProfiler::GetInstance().GetGenerator().GetIsStart()) {
                mutator->ClearSuspensionFlag(Mutator::SUSPENSION_FOR_CPU_PROFILE);
                mutator->SetCpuProfileState(Mutator::FINISH_CPUPROFILE);
                it = undoneMutators.erase(it);
                continue;
            }
            ++it;
        }
    }
}

void MutatorManager::TransitionAllMutatorsToCpuProfile()
{
    bool worldStopped = WorldStopped();
    if (!worldStopped) {
        if (!AcquireMutatorManagementWLockForCpuProfile()) {
            return;
        }
    }
    std::list<Mutator*> undoneMutators;
    VisitAllMutators([&undoneMutators](Mutator& mutator) {
        if (mutator.GetTid() != Heap::GetHeap().GetFinalizerProcessor().GetTid() &&
            mutator.GetCjthreadPtr() == MutatorManager::Instance().GetMainThreadHandle()) {
            mutator.SetSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_CPU_PROFILE);
            mutator.SetSafepointActive(true);
            undoneMutators.push_back(&mutator);
        }
    });
    EnsureCpuProfileFinish(undoneMutators);
    if (!worldStopped) {
        MutatorManagementWUnlock();
    }
}

void MutatorManager::DumpMutators(uint32_t timeoutTimes)
{
    constexpr size_t bufferSize = 4096;
    char buf[bufferSize];
    int index = 0;
    size_t visitedCount = 0;
    size_t visitedSaferegion = 0;
    int firstNotStoppedTid = -1;
    index += sprintf_s(buf, sizeof(buf), "not stopped: ");
    CHECK_DETAIL(index != -1, "Dump mutators state failed");
    size_t mutatorCount = 0;
    VisitAllMutators([&](const Mutator& mut) {
        mutatorCount++;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        mut.DumpMutator();
#endif
        if (!mut.InSaferegion()) {
            if (firstNotStoppedTid == -1) {
                firstNotStoppedTid = static_cast<int>(mut.GetTid());
            }
            int ret = sprintf_s(buf + index, sizeof(buf) - index, "%u ", mut.GetTid());
            CHECK_DETAIL(ret != -1, "Dump mutators state failed");
            index += ret;
        } else {
            ++visitedSaferegion;
        }
        ++visitedCount;
    });
    LOG(RTLOG_ERROR, "MutatorList size : %zu", mutatorCount);

    CHECK_DETAIL(sprintf_s(buf + index, sizeof(buf) - index, ", total: %u, visited: %zu/%zu",
                           GetSuspensionMutatorCount(), visitedSaferegion, visitedCount) != -1,
                 "Dump mutators state failed");
    CHECK_DETAIL(timeoutTimes <= MAX_TIMEOUT_TIMES, "Waiting mutators entering saferegion timeout status info:%s", buf);
    LOG(RTLOG_ERROR, "STW status info:%s", buf);
}

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
void MutatorManager::DumpForDebug()
{
    size_t count = 0;
    auto func = [&count](Mutator& mutator) {
        mutator.DumpMutator();
        count++;
    };
    VisitAllMutators(func);
    LOG(RTLOG_INFO, "MutatorList size : %zu", count);
}

void MutatorManager::DumpAllGcInfos()
{
    auto func = [](Mutator& mutator) { mutator.DumpGCInfos(); };
    VisitAllMutators(func);
}
#endif

extern "C" void MRT_FlushGCInfo()
{
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    // MutatorManager::Instance().DumpAllGcInfos();
    Mutator::GetMutator()->DumpGCInfos();
#endif
}

#ifdef __APPLE__
extern "C" MRT_EXPORT void CJ_MRT_FlushGCInfo();
__asm__(".global _CJ_MRT_FlushGCInfo\n\t.set _CJ_MRT_FlushGCInfo, _MRT_FlushGCInfo");
#else
extern "C" MRT_EXPORT void CJ_MRT_FlushGCInfo() __attribute__((alias("MRT_FlushGCInfo")));
#endif
} // namespace MapleRuntime

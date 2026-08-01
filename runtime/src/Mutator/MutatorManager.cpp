// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "MutatorManager.h"

#include <algorithm>
#include <memory>
#include <thread>
#include "Base/TimeUtils.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Collector/TracingCollector.h"
#include "Heap/Heap.h"
#include "Heap/HeapWork.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MCollector/MSMarkWorkerPolicy.h"
#include "Heap/MCollector/MSEnumRootsWorkerSession.h"
#include "Mutator.inline.h"
#include "schedule.h"
#include "CpuProfiler/CpuProfiler.h"
namespace MapleRuntime {

#ifdef MRT_USE_MARK_SWEEP
namespace {
constexpr size_t kMSEnumGCClaimBatch = 8;
constexpr size_t kMSEnumGCClaimProbeLimit = 256;
constexpr size_t kMSEnumClaimProbeMultiplier = 4;
constexpr size_t kMSEnumClaimMinProbe = 32;

size_t ComputeMSEnumClaimProbeLimit(
    size_t entryCount, size_t batchSize, size_t requestedProbeLimit)
{
    if (entryCount == 0 || batchSize == 0) {
        return 0;
    }
    size_t scaledBatch = entryCount;
    if (batchSize <= entryCount / kMSEnumClaimProbeMultiplier) {
        scaledBatch = batchSize * kMSEnumClaimProbeMultiplier;
    }
    return std::min(entryCount,
        std::max({kMSEnumClaimMinProbe, scaledBatch, requestedProbeLimit}));
}
} // namespace
#endif

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

void MutatorManager::StopTheWorld(bool syncGCPhase, GCPhase phase)
{
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
    syncMutex.lock();
    syncTriggered.store(true);

    AcquireMutatorManagementWLock();

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // If current mutator saferegion state changed,
    // we should restore it after the mutator called StartTheWorld().
    saferegionStateChanged = saferegionEntered;
#endif

    size_t mutatorCount = GetMutatorCount();
    if (UNLIKELY(mutatorCount == 0)) {
        worldStopped.store(true, std::memory_order_release);
        if (syncGCPhase) { TransitionAllMutatorsToGCPhase(phase); }
        return;
    }
    // set mutatorCount as countOfMutatorsToStop.
    SetSuspensionMutatorCount(static_cast<uint32_t>(mutatorCount));
    DemandSuspensionForSync();
    WaitUntilAllMutatorStopped();

    // the world is stopped.
    worldStopped.store(true, std::memory_order_release);
    if (syncGCPhase) { TransitionAllMutatorsToGCPhase(phase); }
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

void MutatorManager::WaitUntilAllMutatorStopped()
{
    uint64_t beginTime = TimeUtil::MilliSeconds();
    std::list<Mutator*> unstoppedMutators;
    auto func = [&unstoppedMutators](Mutator& mutator) {
        if ((!mutator.InSaferegion())) {
            unstoppedMutators.emplace_back(&mutator);
        }
    };
    VisitAllMutators(func);

    size_t remainMutatorsSize = unstoppedMutators.size();
    if (remainMutatorsSize == 0) {
        return;
    }

    // Synchronize operation to ensure that all mutators complete phase transition
    // Use unstoppedMutators to avoid traversing the entire mutatorList
    int timeoutTimes = 0;
    while (true) {
        for (auto it = unstoppedMutators.begin(); it != unstoppedMutators.end();) {
            Mutator* mutator = *it;
            if (mutator->InSaferegion()) {
                // current it(mutator) is finished by GC
                it = unstoppedMutators.erase(it);
            } else {
                ++it; // skip current round & check it next round
            }
        }

        if (unstoppedMutators.size() == 0) {
            return;
        }

        if (UNLIKELY(TimeUtil::MilliSeconds() - beginTime >
            (((remainMutatorsSize / STW_TIMEOUTS_THREADS_BASE_COUNT) * STW_TIMEOUTS_BASE_MS) + STW_TIMEOUTS_BASE_MS))) {
            timeoutTimes++;
            beginTime = TimeUtil::MilliSeconds();
            DumpMutators(timeoutTimes);
        }

        (void)sched_yield();
    }
}

void MutatorManager::EnsurePhaseTransition(GCPhase phase, std::list<Mutator*> &undoneMutators)
{
    // Traverse through undoneMutators to select mutators that have not yet completed transition
    // 1. ignore mutators which have completed transition
    // 2. gc compete phase transition with mutators which are in saferegion
    // 3. fill mutators which are running state in undoneMutators
    while (undoneMutators.size() > 0) {
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
    }
}

#ifdef MRT_USE_MARK_SWEEP
void MutatorManager::EnsureMSMarkSetupPhaseTransition(
    size_t initialMutatorCount, MSEnumRootsWorkerSession* workerSession)
{
    lastMSEnumRootsHelperCount_.store(0, std::memory_order_release);
    size_t helperCount = workerSession != nullptr
        ? workerSession->GetHelperCount()
        : 0;
    uint64_t poolWaitUs = 0;
    if (helperCount != 0) {
        workerSession->RunStage([this](size_t) {
            RunMSEnumTransition(EnumTransitionExecutor::GC_WORKER);
        });
    }
    RunMSEnumTransition(EnumTransitionExecutor::GC_MAIN);
    if (helperCount != 0) {
        poolWaitUs = workerSession->WaitStage();
    }

    lastMSEnumRootsHelperCount_.store(helperCount, std::memory_order_release);

    VLOG(REPORT,
         "[MS-GC] EnumRoots transition: helpers=%zu selfScanned=%zu gcMainScanned=%zu "
         "gcWorkerScanned=%zu mutatorAssistScanned=%zu readyPolls=%llu "
         "poolWait=%sus initialMutators=%zu",
         helperCount,
         enumTransitionContext_.selfScanned.load(std::memory_order_relaxed),
         enumTransitionContext_.gcMainScanned.load(std::memory_order_relaxed),
         enumTransitionContext_.gcWorkerScanned.load(std::memory_order_relaxed),
         enumTransitionContext_.mutatorAssistScanned.load(std::memory_order_relaxed),
         static_cast<unsigned long long>(
             enumTransitionContext_.readyPolls.load(std::memory_order_relaxed)),
         Pretty(poolWaitUs).Str(), initialMutatorCount);
}
#endif

void MutatorManager::TransitionAllMutatorsToGCPhase(
    GCPhase phase, MSEnumRootsWorkerSession* workerSession)
{
#ifdef MRT_USE_MARK_SWEEP
    const bool measureEnumHandshake = (phase == GCPhase::GC_PHASE_MS_MARK_SETUP);
    const bool enableEnumAssist = measureEnumHandshake && CangjieRuntime::IsMSEnumAssistEnabled();
    uint64_t enumHandshakeInstallUs = 0;
    uint64_t enumHandshakeBroadcastUs = 0;
    uint64_t enumHandshakeWaitUs = 0;
    uint64_t enumHandshakeAssistUs = 0;
#endif
    // Try to occupy mutatorListLock prevent some mutators from exiting
    bool worldStopped = WorldStopped();
    if (!worldStopped) {
        AcquireMutatorManagementWLock();
    }

    DLOG(GCPHASE, "transition gc: %s(%u) -> %s(%u)",
         Collector::GetGCPhaseName(Heap::GetHeap().GetGCPhase()), Heap::GetHeap().GetGCPhase(),
         Collector::GetGCPhaseName(phase), phase);

    // Set global gc phase in the scope of mutatorlist lock
#ifdef MRT_USE_MARK_SWEEP
    uint64_t installT0 = TimeUtil::MicroSeconds();
#endif
    Heap::GetHeap().InstallBarrier(phase);
    Heap::GetHeap().SetGCPhase(phase);
#ifdef MRT_USE_MARK_SWEEP
    if (measureEnumHandshake) {
        enumHandshakeInstallUs = TimeUtil::MicroSeconds() - installT0;
    }
#endif

    std::list<Mutator*> undoneMutators;
#ifdef MRT_USE_MARK_SWEEP
    std::vector<Mutator*> enumMutators;
#endif
    // Broadcast mutator phase transition signal to all mutators
#ifdef MRT_USE_MARK_SWEEP
    uint64_t broadcastT0 = TimeUtil::MicroSeconds();
#endif
    VisitAllMutators([&](Mutator& mutator) {
        mutator.SetSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_GC_PHASE);
        mutator.SetSafepointActive(true);
#ifdef MRT_USE_MARK_SWEEP
        if (measureEnumHandshake) {
            enumMutators.push_back(&mutator);
            return;
        }
#endif
        undoneMutators.push_back(&mutator);
    });
#ifdef MRT_USE_MARK_SWEEP
    if (measureEnumHandshake) {
        enumHandshakeBroadcastUs = TimeUtil::MicroSeconds() - broadcastT0;
        PrepareMSEnumTransitionContext(phase, enumMutators, enableEnumAssist);
    }
#endif
#ifdef MRT_USE_MARK_SWEEP
    uint64_t waitT0 = TimeUtil::MicroSeconds();
#endif
#ifdef MRT_USE_MARK_SWEEP
    if (phase == GCPhase::GC_PHASE_MS_MARK_SETUP) {
        EnsureMSMarkSetupPhaseTransition(enumMutators.size(), workerSession);
    } else {
        EnsurePhaseTransition(phase, undoneMutators);
    }
#else
    EnsurePhaseTransition(phase, undoneMutators);
#endif
#ifdef MRT_USE_MARK_SWEEP
    if (measureEnumHandshake) {
        uint64_t assistT0 = TimeUtil::MicroSeconds();
        FinalizeEnumAssistContext(phase);
        if (enableEnumAssist) {
            VisitAllMutators([](Mutator& mutator) {
                mutator.ClearSuspensionFlag(Mutator::SuspensionType::SUSPENSION_FOR_GC_ENUM_ASSIST);
                if (!mutator.HasAnySuspensionRequest() && !mutator.HasObserver()) {
                    mutator.SetSafepointActive(false);
                }
            });
        }
        enumHandshakeAssistUs = TimeUtil::MicroSeconds() - assistT0;
        enumHandshakeWaitUs = TimeUtil::MicroSeconds() - waitT0;
    }
#endif
#ifdef MRT_USE_MARK_SWEEP
    if (measureEnumHandshake) {
        VLOG(REPORT,
             "[MS-GC] EnumRoots handshake: install=%sus broadcast=%sus wait=%sus assistDrain=%sus",
             Pretty(enumHandshakeInstallUs).Str(), Pretty(enumHandshakeBroadcastUs).Str(),
             Pretty(enumHandshakeWaitUs).Str(), Pretty(enumHandshakeAssistUs).Str());
    }
#endif
    if (!worldStopped) {
        MutatorManagementWUnlock();
    }
}

#ifdef MRT_USE_MARK_SWEEP
bool MutatorManager::HasPendingEnumAssist(GCPhase phase)
{
    return phase == GCPhase::GC_PHASE_MS_MARK_SETUP &&
        enumTransitionContext_.active.load(std::memory_order_acquire) &&
        enumTransitionContext_.assistEnabled.load(std::memory_order_acquire) &&
        enumTransitionContext_.remaining.load(std::memory_order_acquire) != 0;
}

void MutatorManager::PrepareMSEnumTransitionContext(
    GCPhase phase, const std::vector<Mutator*>& candidates,
    bool assistEnabled)
{
    EnumTransitionContext& context = enumTransitionContext_;
    std::lock_guard<std::mutex> guard(context.lock);
    CHECK_DETAIL(!context.active.load(std::memory_order_relaxed) &&
                 context.inFlight.load(std::memory_order_relaxed) == 0,
                 "preparing an active EnumRoots transition context");
    if (candidates.size() > context.entryCapacity) {
        std::unique_ptr<EnumTransitionEntry[]> entries(
            new (std::nothrow) EnumTransitionEntry[candidates.size()]);
        CHECK_DETAIL(entries != nullptr,
                     "failed to allocate EnumRoots transition entries");
        context.entries = std::move(entries);
        context.entryCapacity = candidates.size();
    }
    context.entryCount = candidates.size();
    for (size_t i = 0; i < candidates.size(); ++i) {
        context.entries[i].mutator = candidates[i];
        context.entries[i].state.store(
            EnumTransitionState::PENDING, std::memory_order_relaxed);
    }
    context.phase = phase;
    context.claimCursor.store(0, std::memory_order_relaxed);
    context.inFlight.store(0, std::memory_order_relaxed);
    context.selfScanned.store(0, std::memory_order_relaxed);
    context.gcMainScanned.store(0, std::memory_order_relaxed);
    context.gcWorkerScanned.store(0, std::memory_order_relaxed);
    context.mutatorAssistScanned.store(0, std::memory_order_relaxed);
    context.readyPolls.store(0, std::memory_order_relaxed);
    context.remaining.store(candidates.size(), std::memory_order_release);
    context.assistEnabled.store(assistEnabled, std::memory_order_release);
    context.active.store(true, std::memory_order_release);
}

size_t MutatorManager::ClaimMSEnumTransitionBatch(
    size_t batchSize, size_t requestedProbeLimit, Mutator* excluded,
    std::vector<size_t>& claimedEntries)
{
    claimedEntries.clear();
    if (batchSize == 0) {
        return 0;
    }

    EnumTransitionContext& context = enumTransitionContext_;
    if (!context.active.load(std::memory_order_acquire) ||
        context.remaining.load(std::memory_order_relaxed) == 0) {
        return 0;
    }

    const size_t entryCount = context.entryCount;
    const size_t probeLimit = ComputeMSEnumClaimProbeLimit(
        entryCount, batchSize, requestedProbeLimit);
    if (probeLimit == 0) {
        return 0;
    }
    size_t index = context.claimCursor.fetch_add(
        probeLimit, std::memory_order_relaxed) % entryCount;
    for (size_t probed = 0;
         probed < probeLimit && claimedEntries.size() < batchSize;
         ++probed, index = (index + 1) % entryCount) {

        EnumTransitionEntry& entry = context.entries[index];
        if (entry.state.load(std::memory_order_acquire) !=
            EnumTransitionState::PENDING) {
            continue;
        }
        Mutator* mutator = entry.mutator;
        if (mutator->GetMutatorPhase() == context.phase &&
            mutator->FinishedTransition()) {
            EnumTransitionState expected = EnumTransitionState::PENDING;
            if (entry.state.compare_exchange_strong(
                    expected, EnumTransitionState::DONE,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                context.remaining.fetch_sub(1, std::memory_order_release);
                context.selfScanned.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        if (mutator == excluded || !mutator->InSaferegion()) {
            continue;
        }
        EnumTransitionState expected = EnumTransitionState::PENDING;
        if (entry.state.compare_exchange_strong(
                expected, EnumTransitionState::CLAIMED,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            claimedEntries.push_back(index);
        }
    }
    context.inFlight.fetch_add(
        claimedEntries.size(), std::memory_order_acq_rel);
    return claimedEntries.size();
}

void MutatorManager::CompleteMSEnumTransitionBatch(
    const std::vector<size_t>& claimedEntries,
    EnumTransitionExecutor executor)
{
    if (claimedEntries.empty()) {
        return;
    }

    EnumTransitionContext& context = enumTransitionContext_;
    std::vector<uint8_t> transitioned(claimedEntries.size(), 0);
    for (size_t i = 0; i < claimedEntries.size(); ++i) {
        transitioned[i] = context.entries[claimedEntries[i]].mutator
            ->TransitionGCPhase(false) ? 1 : 0;
    }

    for (size_t i = 0; i < claimedEntries.size(); ++i) {
        EnumTransitionEntry& entry = context.entries[claimedEntries[i]];
        CHECK_DETAIL(entry.state.load(std::memory_order_acquire) ==
                     EnumTransitionState::CLAIMED,
                     "completing an unclaimed EnumRoots entry");
        bool selfCompleted = !transitioned[i] &&
            entry.mutator->GetMutatorPhase() == context.phase &&
            entry.mutator->FinishedTransition();
        if (transitioned[i] || selfCompleted) {
            entry.state.store(
                EnumTransitionState::DONE, std::memory_order_release);
            context.remaining.fetch_sub(1, std::memory_order_release);
            if (selfCompleted) {
                context.selfScanned.fetch_add(1, std::memory_order_relaxed);
            } else if (executor == EnumTransitionExecutor::GC_MAIN) {
                context.gcMainScanned.fetch_add(1, std::memory_order_relaxed);
            } else if (executor == EnumTransitionExecutor::GC_WORKER) {
                context.gcWorkerScanned.fetch_add(1, std::memory_order_relaxed);
            } else {
                context.mutatorAssistScanned.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            entry.state.store(
                EnumTransitionState::PENDING, std::memory_order_release);
        }
    }
    size_t previousInFlight = context.inFlight.fetch_sub(
        claimedEntries.size(), std::memory_order_acq_rel);
    CHECK_DETAIL(previousInFlight >= claimedEntries.size(),
                 "EnumRoots transition inFlight underflow");
}

void MutatorManager::RunMSEnumTransition(EnumTransitionExecutor executor)
{
    std::vector<size_t> claimedEntries;
    claimedEntries.reserve(kMSEnumGCClaimBatch);
    for (;;) {
        if (ClaimMSEnumTransitionBatch(
                kMSEnumGCClaimBatch, kMSEnumGCClaimProbeLimit,
                nullptr, claimedEntries) != 0) {
            CompleteMSEnumTransitionBatch(claimedEntries, executor);
            continue;
        }
        EnumTransitionContext& context = enumTransitionContext_;
        if (context.remaining.load(std::memory_order_acquire) == 0) {
            if (context.inFlight.load(std::memory_order_acquire) == 0) {
                return;
            }
        }
        context.readyPolls.fetch_add(1, std::memory_order_relaxed);
        (void)sched_yield();
    }
}

size_t MutatorManager::AssistEnumRoots(
    GCPhase phase, size_t batchSize, Mutator* assistant)
{
    if (!HasPendingEnumAssist(phase) || batchSize == 0) {
        return 0;
    }
    std::vector<size_t> claimedEntries;
    claimedEntries.reserve(batchSize);
    size_t claimed = ClaimMSEnumTransitionBatch(
        batchSize, 0, assistant, claimedEntries);
    CompleteMSEnumTransitionBatch(
        claimedEntries, EnumTransitionExecutor::MUTATOR_ASSIST);
    return claimed;
}

void MutatorManager::FinalizeEnumAssistContext(GCPhase phase)
{
    if (phase != GCPhase::GC_PHASE_MS_MARK_SETUP) {
        return;
    }
    EnumTransitionContext& context = enumTransitionContext_;
    std::unique_lock<std::mutex> lock(context.lock);
    if (context.phase != phase) {
        return;
    }
    context.assistEnabled.store(false, std::memory_order_release);
    context.active.store(false, std::memory_order_release);
    CHECK_DETAIL(context.inFlight.load(std::memory_order_acquire) == 0,
                 "finalizing EnumRoots with in-flight transitions");
    CHECK_DETAIL(context.remaining.load(std::memory_order_relaxed) == 0,
                 "finalizing EnumRoots with unfinished transitions");
    context.phase = GCPhase::GC_PHASE_UNDEF;
    context.claimCursor.store(0, std::memory_order_relaxed);
    context.remaining.store(0, std::memory_order_release);
    context.entryCount = 0;
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

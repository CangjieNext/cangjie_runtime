// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CangjieRuntime.h"

#include <thread>

#include "Base/Log.h"
#include "Base/EnvParam.h"
#include "ExceptionManager.h"
#include "Heap/Heap.h"
#include "HeapManager.h"
#include "ObjectManager.h"
#include "StackManager.h"
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
#include "SignalManager.h"
#endif
#include "TypeInfoManager.h"
#ifdef _WIN64
#include "WinModuleManager.h"
#endif
#include "Common/ScopedObjectAccess.h"
#include "Concurrency/Concurrency.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Heap.h"
#include "LoaderManager.h"
#include "LogManager.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#ifndef MRT_USE_MARK_SWEEP
#include "Heap/Allocator/RegionSpace.h"
#else
#include "Heap/MCollector/MSGCConfig.h"
#include "Heap/MCollector/MSMarkWorkerPolicy.h"
#endif
#ifdef CANGJIE_GWPASAN_SUPPORT
#include "Sanitizer/SanitizerInterface.h"
#endif
namespace MapleRuntime {
StackGrowConfig CangjieRuntime::stackGrowConfig = StackGrowConfig::UNDEF;
size_t CangjieRuntime::msMaxVirtualHeapSizeKB = 0;
#ifdef MRT_USE_MARK_SWEEP
size_t CangjieRuntime::msSoftHeapLimitBytes = 0;
MSAllocatorScaleConfig CangjieRuntime::msAllocatorScaleConfig {};
size_t CangjieRuntime::msGCThreadCapacity = 1;
bool CangjieRuntime::msEnumAssistEnabled = true;
bool CangjieRuntime::msAssistDisabled = false;

void CangjieRuntime::ConfigureMSAllocatorScale(uint32_t processorCount, size_t maxVirtualHeapSizeKB)
{
    size_t configuredShardCount = ReadCountParameter(
        "cjMSShardCount", 0, 0, EnvBoundKind::CLOSED);
    size_t configuredInitialSegmentCount = ReadCountParameter(
        "cjMSInitialSegmentCount", 0, 0, EnvBoundKind::CLOSED,
        kMSMaxInitialSegmentCount);
    uint32_t shardCount = ResolveMSShardCount(processorCount, configuredShardCount);
    size_t initialSegmentCount = ResolveMSInitialSegmentCount(configuredInitialSegmentCount);
    CHECK_DETAIL(TryResolveMSRequiredVirtualHeapSizeKB(
                     initialSegmentCount, maxVirtualHeapSizeKB, msMaxVirtualHeapSizeKB),
                 "[MS-GC] allocator scale exceeds addressable size: initialSegments=%zu maxVirtualKB=%zu",
                 initialSegmentCount, maxVirtualHeapSizeKB);

    msAllocatorScaleConfig = {
        .shardCount = shardCount,
        .processorsPerShard = ComputeMSProcessorsPerShard(processorCount, shardCount),
        .initialSegmentCount = initialSegmentCount,
    };
}
#endif
extern "C" MRT_EXPORT void MRT_LibraryOnLoad(uint64_t address, bool enableGC)
{
    ScopedEntryTrace trace("CJRT_LOAD_LIBRARY");
    if (address == 0) {
        return;
    }
    if (!enableGC) {
        Heap::GetHeap().EnableGC(false);
    }
    LoaderManager* loaderManager = LoaderManager::GetInstance();
    loaderManager->LoadFile(address);
}

extern "C" MRT_EXPORT void MRT_PreInitializePackage(uint64_t address)
{
    LoaderManager* loaderManager = LoaderManager::GetInstance();
    loaderManager->PreInitializePackage(address);
}

extern "C" MRT_EXPORT void MRT_LibraryUnLoad(uint64_t address)
{
    if (address == 0 || LoaderManager::GetReleaseStatus()) {
        return;
    }
    LoaderManager* loaderManager = LoaderManager::GetInstance();
    loaderManager->UnloadFile(address);
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
void DumpAllStackTrace0()
{
    LOG(RTLOG_INFO, "---------------------------Dump all cjthread stack trace--------------------------- \n");
#ifdef MRT_USE_MARK_SWEEP
    uint32_t backgroundTid = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor().GetTid();
#endif
    MutatorManager::Instance().VisitAllMutators([&](Mutator& mutator) {
        // don't print finalizerProcessorThread or backgroundProcessorThread
        if (mutator.GetTid() != Heap::GetHeap().GetFinalizerProcessor().GetTid()
#ifdef MRT_USE_MARK_SWEEP
            && mutator.GetTid() != backgroundTid
#endif
        ) {
            LOG(RTLOG_INFO, "cjthread thread:%d mutator:%p :\n", mutator.GetCJThreadId(), &mutator);
            StackManager::PrintStackTrace(&(mutator.GetUnwindContext()));
            LOG(RTLOG_INFO, "\n");
        }
    });
}

// dump and print StackTraces of all cjthreads
extern "C" void MRT_DumpAllStackTrace()
{
    ScopedEnterSaferegion enterSaferegion(false);
    if (MutatorManager::Instance().WorldStopped()) {
        DumpAllStackTrace0();
    } else {
        ScopedStopTheWorld stw("dump all stack trace");
        DumpAllStackTrace0();
    }
}
#ifdef __APPLE__
extern "C" MRT_EXPORT void CJ_MRT_DumpAllStackTrace();
__asm__(".global _CJ_MRT_DumpAllStackTrace\n\t.set _CJ_MRT_DumpAllStackTrace, _MRT_DumpAllStackTrace");
#else
extern "C" MRT_EXPORT void CJ_MRT_DumpAllStackTrace() __attribute__((alias("MRT_DumpAllStackTrace")));
#endif
#endif

// Runtime implementation
Runtime* Runtime::runtime = nullptr;
std::atomic<bool> g_initialized = { false };

void CangjieRuntime::CreateAndInit(const RuntimeParam& runtimeParam)
{
    ScopedEntryTrace trace("CJRT_CreateAndInit");
    if (runtime != nullptr) {
    }
    // need to create runtime
    if (g_initialized.load(std::memory_order_acquire)) {
        return;
    }
#ifdef MRT_USE_MARK_SWEEP
    unsigned int hardwareThreads = std::thread::hardware_concurrency();
    size_t logicalCpuCount = hardwareThreads != 0
        ? static_cast<size_t>(hardwareThreads)
        : static_cast<size_t>(runtimeParam.coParam.processorNum);
    SetMSGCThreadCapacity(ResolveMSGCThreadCapacity(
        static_cast<size_t>(runtimeParam.coParam.processorNum), logicalCpuCount));
#endif
    auto cjRuntime = new (std::nothrow) CangjieRuntime(runtimeParam);
    CHECK_DETAIL(cjRuntime != nullptr, "new CangjieRuntime failed");
    Runtime::runtime = cjRuntime;
    cjRuntime->Init();
    g_initialized.store(true, std::memory_order_release);
}

void CangjieRuntime::SetGCThreshold(uint64_t threshold)
{
    size_t heapSizeBytes = (Heap::heapCurrentEnd > Heap::heapStartAddr)
        ? static_cast<size_t>(Heap::heapCurrentEnd - Heap::heapStartAddr)
        : static_cast<size_t>(param.heapParam.heapSize) * KB;
    if (threshold != 0) {
        LOG(RTLOG_INFO,
            "Ignore requested gcThreshold=%llu KB; gcThreshold is derived from current heap size.",
            static_cast<unsigned long long>(threshold));
    }
    SyncGCThresholdWithHeapSize(heapSizeBytes);
}

void CangjieRuntime::SyncGCThresholdWithHeapSize(size_t heapSizeBytes)
{
    if (heapSizeBytes == 0) {
        return;
    }
    param.gcParam.gcThreshold = heapSizeBytes;
}

void CangjieRuntime::FiniAndDelete()
{
    ScopedEntryTrace trace("CJRT_FiniAndDelete");
    if (Runtime::runtime == nullptr) {
        LOG(RTLOG_ERROR, "Fini called but Cangjie runtime is not running");
        return;
    }
    auto cjRuntime = reinterpret_cast<CangjieRuntime*>(Runtime::runtime);
    Runtime::runtime = nullptr;
    cjRuntime->Fini();
    delete cjRuntime;
}

CangjieRuntime::CangjieRuntime(const RuntimeParam& runtimeParam) : param(runtimeParam)
{
#ifdef MRT_USE_MARK_SWEEP
    MSGCConfigInput configInput;
    configInput.enumAssistEnabled =
        ReadBoolParameter("cjMSEnumAssist", true);
    configInput.assistDisabled =
        ReadBoolParameter("CJ_MS_DISABLE_ASSIST", false);
    MSGCResolvedConfig config = ResolveMSGCConfig(configInput);
    msEnumAssistEnabled = config.enumAssistEnabled != 0;
    msAssistDisabled = config.assistDisabled != 0;
#endif
    SyncGCThresholdWithHeapSize(static_cast<size_t>(param.heapParam.heapSize) * KB);
}

template<typename T>
inline T* NewAndInit()
{
    T* temp = new (std::nothrow) T();
    CHECK_DETAIL(temp != nullptr, "NewAndInit failed");
    temp->Init();
    return temp;
}

template<typename T, typename A>
inline T* NewAndInit(A arg)
{
    T* temp = new (std::nothrow) T();
    CHECK_DETAIL(temp != nullptr, "NewAndInit failed");
    temp->Init(arg);
    return temp;
}

// LoaderManager must be initialized before ConcurrencyModel because of the reservedStackSize field.
// ConcurrencyModel must be initialized before SignalManager because of the stackGuardCheck field.
// HeapManager must be initialized before HeapManager because of inherited properties of signal handlers.
void CangjieRuntime::Init()
{
    PagePool::Instance().Init(param.heapParam.heapSize * KB / MRT_PAGE_SIZE);
#ifdef CANGJIE_GWPASAN_SUPPORT
    // setup gwpasan before heap initialized
    MapleRuntime::Sanitizer::SetupGwpAsanAsNeeded();
#endif
#ifdef _WIN64
    winModuleManager = NewAndInit<WinModuleManager>();
#endif
    stackManager = NewAndInit<StackManager>();
    logManager = NewAndInit<LogManager>();
    mutatorManager = NewAndInit<MutatorManager>();
    concurrencyModel = NewAndInit<Concurrency>(param.coParam);
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
    signalManager = NewAndInit<SignalManager>();
#endif
    heapManager = NewAndInit<HeapManager>(param.heapParam);
    typeInfoManager = &TypeInfoManager::GetTypeInfoManager();
    typeInfoManager->Init();
    loaderManager = LoaderManager::GetInstance();
    loaderManager->Init();
    objectManager = NewAndInit<ObjectManager>();
    exceptionManager = NewAndInit<ExceptionManager>();

    LOG(RTLOG_INFO, "Cangjie runtime started.");
    // MSGC has a separate policy surface from CopyGC. Report only effective
    // MSGC controls here; RuntimeParam keeps its ABI-reserved fields but they
    // are intentionally not presented as active tuning knobs.
#ifdef MRT_USE_MARK_SWEEP
    const GCParam& gcParam = param.gcParam;
    MSAllocatorScaleConfig scale = GetMSAllocatorScaleConfig();
    VLOG(REPORT,
         "[MS-GC] config: heap=%.2fMB region=%.2fMB gcThreads=%d P=%d "
         "markBudget=%lluus sweepSlice=%lluus",
         static_cast<double>(param.heapParam.heapSize) / 1024.0,
         static_cast<double>(param.heapParam.regionSize) / 1024.0,
         gcParam.gcThreads, param.coParam.processorNum,
         static_cast<unsigned long long>(gcParam.msMarkTimeBudget / 1000ULL),
         static_cast<unsigned long long>(gcParam.msBackgroundSweepMaxTime / 1000ULL));
    VLOG(REPORT,
         "[MS-GC] config: markAssist=%d enumAssist=%d enumBatch=%zu "
         "assistMaxMultiplier=%zu sweepAssistMaxRegions=%zu "
         "background(sweep=%u parallel=%u helpers=%zu scavenge=%u shrink=%u) "
         "verify=%u/%u adaptiveGrowth=%u softLimit=%.2fMB allocator(shards=%u processorsPerShard=%u initialSegments=%zu)",
         static_cast<int>(!IsMSAssistDisabled()),
         static_cast<int>(IsMSEnumAssistEnabled()), gcParam.msEnumAssistBatchSize,
         gcParam.msAssistMaxMultiplier, gcParam.msAssistSweepMaxRegionsPerCall,
         static_cast<unsigned>(gcParam.msBackgroundSweep),
         static_cast<unsigned>(gcParam.msParallelSweep),
         gcParam.msParallelSweepMaxHelpers,
         static_cast<unsigned>(gcParam.msBackgroundScavenge),
         static_cast<unsigned>(gcParam.msBackgroundShrink),
         static_cast<unsigned>(gcParam.msVerifyMark),
         static_cast<unsigned>(gcParam.msVerifyMarkDetail),
         static_cast<unsigned>(gcParam.msAdaptiveGrowth),
         static_cast<double>(GetMSSoftHeapLimitBytes()) / (1024.0 * 1024.0),
         scale.shardCount, scale.processorsPerShard, scale.initialSegmentCount);
#else
    // Record runtime parameter to report. heap growth value needs to plus 1.
#if not defined(__EULER__)
    VLOG(REPORT,
        "Runtime parameter:\n\tHeap size: %zu(KB)\n\tRegion size: %zu(KB)\n\tExemption threshold: %.2f\n\t"
        "Heap utilization: %.2f\n\tHeap growth: %.2f\n\tAllocation rate: %.2f(MB/s)\n\tAllocation wait time: %zuns\n\t"
        "GC Threshold: %zu(KB)\n\tGarbage threshold: %.2f\n\tGC interval: %llums\n\tBackup GC interval: %llus\n\t"
        "Log level: %d\n\tThread stack size: %zu(KB)\n\tCangjie stack size: %zu(KB)\n\t"
        "Processor number: %d", param.heapParam.heapSize, param.heapParam.regionSize,
        param.heapParam.exemptionThreshold, param.heapParam.heapUtilization, 1 + param.heapParam.heapGrowth,
        param.heapParam.allocationRate, param.heapParam.allocationWaitTime,
        param.gcParam.gcThreshold / KB, param.gcParam.garbageThreshold,
        param.gcParam.gcInterval / MILLI_SECOND_TO_NANO_SECOND, param.gcParam.backupGCInterval / SECOND_TO_NANO_SECOND,
        static_cast<int>(param.logParam.logLevel), param.coParam.thStackSize,
        param.coParam.coStackSize, param.coParam.processorNum);
#else
    VLOG(REPORT,
        "Runtime parameter:\n\tHeap size: %zu(KB)\n\tRegion size: %zu(KB)\n\tExemption threshold: %.2f\n\t"
        "Heap utilization: %.2f\n\tCache Ratio: %.2f\n\tHeap growth: %.2f\n\tAllocation rate: %.2f(MB/s)\n\t"
        "Allocation wait time: %zuns\n\tGC Threshold: %zu(KB)\n\t"
        "Garbage threshold: %.2f\n\tGC interval: %zums\n\tBackup GC interval: %zus\n\t"
        "Log level: %d\n\tThread stack size: %zu(KB)\n\tCangjie stack size: %zu(KB)\n\tProcessor number: %d",
        param.heapParam.heapSize, param.heapParam.regionSize, param.heapParam.exemptionThreshold,
        param.heapParam.heapUtilization, reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).
        GetRegionManager().GetCacheRatio(), 1 + param.heapParam.heapGrowth,
        param.heapParam.allocationRate, param.heapParam.allocationWaitTime,
        param.gcParam.gcThreshold / KB, param.gcParam.garbageThreshold,
        param.gcParam.gcInterval / MILLI_SECOND_TO_NANO_SECOND, param.gcParam.backupGCInterval / SECOND_TO_NANO_SECOND,
        static_cast<int>(param.logParam.logLevel), param.coParam.thStackSize,
        param.coParam.coStackSize, param.coParam.processorNum);
#endif
#endif // MRT_USE_MARK_SWEEP
}

void* CangjieRuntime::CreateSubSchedulerAndInit(ScheduleType type)
{
    std::lock_guard<std::mutex> guard(mtx);
    auto concurrency = new (std::nothrow) Concurrency();
    CHECK_DETAIL(concurrency != nullptr, "NewAndInit failed");
    auto defaultParam = CangjieRuntime::GetConcurrencyParam();
    struct ConcurrencyParam coParam = {
        .thStackSize = defaultParam.thStackSize,
        .coStackSize = defaultParam.coStackSize,
        .processorNum = 1,
    };
    concurrency->Init(coParam, type);
    void* scheduler = concurrency->GetThreadScheduler();
    subModelMap.insert({scheduler, concurrency});
    return scheduler;
}

void* CangjieRuntime::CreateSingleThreadScheduler()
{
    auto defaultParam = CangjieRuntime::GetConcurrencyParam();
    ScheduleAttr attr;
    ScheduleAttrInit(&attr);
    ScheduleAttrThstackSizeSet(&attr, defaultParam.thStackSize * KB);
    ScheduleAttrCostackSizeSet(&attr, defaultParam.coStackSize * KB);
    ScheduleAttrProcessorNumSet(&attr, 1);
    ScheduleAttrStackGrowSet(&attr, false);
    ScheduleAttrStackProtectSet(&attr, false);

    ScheduleGetTlsHookRegister((GetTlsHookFunc)MRT_GetThreadLocalData);

    // should not use system page size to calculate reserved stack size,
    // because the page size could be different in different system.
#ifdef _WIN64
    constexpr uint32_t reservedStackSize = 24 * KB;
#else
    constexpr uint32_t reservedStackSize = 8 * KB;
#endif
    CJThreadStackReversedSet(reservedStackSize);
    return ScheduleNew(SCHEDULE_UI_THREAD, &attr);
}

bool CangjieRuntime::CheckSubSchedulerValid(void* scheduler)
{
    // Check scheduler exist.
    auto found = subModelMap.find(scheduler);
    if (found == subModelMap.end()) {
        return false;
    }
    return true;
}

template<typename T>
inline void CheckAndFini(T*& module)
{
    if (module != nullptr) {
        module->Fini();
    }

    delete module;
    module = nullptr;
}

void CangjieRuntime::Fini()
{
    // To avoid foreign thread access finalized runtime.
    ThreadLocal::ThreadLocalFini();
    // since there might be failure during initialization,
    // here we need to check and call fini.
#ifdef _WIN64
    CheckAndFini<WinModuleManager>(winModuleManager);
#endif
    CheckAndFini<ObjectManager>(objectManager);
    CheckAndFini<HeapManager>(heapManager);
    CheckAndFini<ExceptionManager>(exceptionManager);
    CheckAndFini<StackManager>(stackManager);
    loaderManager->Fini();
    typeInfoManager->Fini();
    CheckAndFini<MutatorManager>(mutatorManager);
    CheckAndFini<ConcurrencyModel>(concurrencyModel);
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
    CheckAndFini<SignalManager>(signalManager);
#endif
    CheckAndFini<LogManager>(logManager);
    PagePool::Instance().Fini();
    LOG(RTLOG_INFO, "Cangjie runtime shutdown.");
}

bool CangjieRuntime::FiniSubScheduler(void* scheduler)
{
    std::lock_guard<std::mutex> guard(mtx);
    if (!CheckSubSchedulerValid(scheduler)) {
        return false;
    }
    auto concurrency = subModelMap[scheduler];
    CheckAndFini<ConcurrencyModel>(concurrency);
    subModelMap.erase(scheduler);
    return true;
}

extern "C" bool MRT_CJLibInit(const char* libName) { return LoaderManager::GetInstance()->LibInit(libName); }
#ifdef __APPLE__
extern "C" MRT_EXPORT void CJ_MRT_LibraryOnLoad(uint64_t address, bool enableGC);
__asm__(".global _CJ_MRT_LibraryOnLoad\n\t.set _CJ_MRT_LibraryOnLoad, _MRT_LibraryOnLoad");
extern "C" MRT_EXPORT void CJ_MRT_LibraryUnLoad(uint64_t address);
__asm__(".global _CJ_MRT_LibraryUnLoad\n\t.set _CJ_MRT_LibraryUnLoad, _MRT_LibraryUnLoad");
extern "C" MRT_EXPORT void CJ_MRT_PreInitializePackage(uint64_t address);
__asm__(".global _CJ_MRT_PreInitializePackage\n\t.set _CJ_MRT_PreInitializePackage, _MRT_PreInitializePackage");
#else
extern "C" MRT_EXPORT void CJ_MRT_LibraryOnLoad(uint64_t address, bool enableGC)
    __attribute__((alias("MRT_LibraryOnLoad")));
extern "C" MRT_EXPORT void CJ_MRT_LibraryUnLoad(uint64_t address) __attribute__((alias("MRT_LibraryUnLoad")));
extern "C" MRT_EXPORT void CJ_MRT_PreInitializePackage(uint64_t address)
    __attribute__((alias("MRT_PreInitializePackage")));
#endif
} // namespace MapleRuntime

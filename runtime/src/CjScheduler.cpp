// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CjScheduler.h"

#include <thread>
#include <limits>
#if defined(_WIN64)
#include <windows.h>
#elif defined(__APPLE__)
#include "sys/sysctl.h"
#else
#include "sys/sysinfo.h"
#endif

#include "Base/CString.h"
#include "Base/EnvParam.h"
#include "Sync/Sync.h"
#include "Base/Panic.h"
#include "Cangjie.h"
#include "CangjieRuntime.h"
#include "Mutator/MutatorManager.h"
#include "RuntimeConfig.h"
#include "schedule.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Concurrency/ProcessorScalePolicy.h"
#include "ExceptionManager.h"
#include "ExceptionManager.inline.h"
#include "Mutator/Mutator.inline.h"
#include "schedule.h"
#ifdef MRT_USE_MARK_SWEEP
#include "Heap/MCollector/MSAssistPacing.h"
#include "Heap/MCollector/MSMarkWorkerPolicy.h"
#include "Heap/MAllocator/MSShardRouting.h"
#endif
#if defined(CANGJIE_SANITIZER_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "CpuProfiler/CpuProfiler.h"
namespace MapleRuntime {
#ifdef __cplusplus
extern "C" {
#endif

static size_t g_initStackSize = 0;
static size_t g_sysmemSize = 1 * GB;

enum TimeUnit : uint32_t {
    SECOND = 0,
    MILLI_SECOND = 1,
    MICRO_SECOND = 2,
    NANO_SECOND = 3,
};

static void CheckSysmemSize()
{
#if defined(_WIN64)
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    bool ret = GlobalMemoryStatusEx(&memInfo);
    if (ret) {
        g_sysmemSize = memInfo.ullTotalPhys;
    } else {
        LOG(RTLOG_ERROR, "Get system memory failed.\n");
    }
#elif defined(__APPLE__)
    const int sz = 2;
    int mib[sz];
    size_t length = sizeof(g_sysmemSize);
    mib[0] = CTL_HW;
    mib[1] = HW_MEMSIZE;
    sysctl(mib, sz, &g_sysmemSize, &length, nullptr, 0);
#else
    struct sysinfo memInfo;
    int ret = sysinfo(&memInfo);
    if (ret == 0) {
        g_sysmemSize = memInfo.totalram * memInfo.mem_unit;
    } else {
        LOG(RTLOG_ERROR, "Get system memory failed. msg: %s.\n", strerror(errno));
    }
#endif
}

/**
 * Init runtime's heap size from environment variable.
 * The unit must be added when configuring "cjHeapSize", it supports "kb", "mb", "gb".
 * Valid heap size range is [4MB, system memory size].
 * for example:
 *     export cjHeapSize = 32GB
 */
#ifndef MRT_USE_MARK_SWEEP
static size_t InitHeapSize(size_t defaultParam)
{
#if defined(__OHOS__) || defined(__ANDROID__)
    // 64UL * KB: The minimum heap size in OHOS, measured in KB, the value is 64MB.
    size_t minSize = 64UL * KB;
#else
    // 4UL * KB: The minimum heap size, measured in KB, the value is 4MB.
    size_t minSize = 4UL * KB;
#endif
    size_t maxSize = g_sysmemSize / KB;
    return ReadSizeParameter("cjHeapSize", defaultParam, minSize, EnvBoundKind::CLOSED,
                             maxSize, EnvBoundKind::CLOSED);
}
#endif

static size_t InitRegionSize(size_t defaultParam)
{
    // The minimum region size is system page size, measured in KB.
    size_t minSize = MapleRuntime::MRT_PAGE_SIZE / KB;
    // 64UL: The maximum region size, measured in KB, the value is 2048 KB.
    size_t maxSize = 2 * 1024UL;
    return ReadSizeParameter("cjRegionSize", defaultParam, minSize, EnvBoundKind::CLOSED,
                             maxSize, EnvBoundKind::CLOSED);
}

#ifdef MRT_USE_MARK_SWEEP
static size_t ComputeMSInitialHeapSizeKB(size_t initialSegmentCount)
{
    size_t initialHeapSizeBytes = 0;
    CHECK_DETAIL(TryComputeMSInitialCommittedHeapBytes(initialSegmentCount, initialHeapSizeBytes),
                 "[MS-GC] initial Segment count exceeds addressable heap: %zu", initialSegmentCount);
    return initialHeapSizeBytes / KB;
}

static size_t InitMSMaxVirtualHeapSize(size_t minVirtualHeapSizeKB)
{
    static constexpr size_t requestedDefaultValueKB = 16ULL * GB / KB;
    static constexpr size_t defaultValueKB =
        requestedDefaultValueKB > kMSMaxVirtualHeapSizeKB
            ? kMSMaxVirtualHeapSizeKB
            : requestedDefaultValueKB;
    return ReadSizeParameter("cjMSMaxVirtualHeapSize", defaultValueKB,
                             minVirtualHeapSizeKB, EnvBoundKind::CLOSED,
                             kMSMaxVirtualHeapSizeKB, EnvBoundKind::CLOSED);
}
#endif

/**
 * Init light-weight-thread stack size from environment variable.
 * The unit must be added when configuring "cjStackSize", it supports "kb", "mb", "gb".
 * Valid stack size range is [64KB, 1GB].
 * for example:
 *     export cjStackSize = 100kb
 */
static size_t InitCoStackSize()
{
#ifdef _WIN64
    size_t defaultStackSize = 128; // default 128KB in windows, measured in KB
#elif defined(CANGJIE_SANITIZER_SUPPORT)
    // cus sanitizer calls tons of instrumentation, which use more stack memory than normal does
    size_t defaultStackSize = 2048; // default 2MB in sanitizer version, measured in KB
#elif defined(__OHOS__) || defined(__ANDROID__) || defined (__APPLE__)
    size_t defaultStackSize = 1024; // default 1MB in OHOS/ANDROID/MACOS, measured in KB
#else
    size_t defaultStackSize = 128; // default 128KB, measured in KB
#endif
#ifdef _WIN64
    const size_t minStackSize = 128; // min: 128KB.
#else
    const size_t minStackSize = 64; // min: 64KB.
#endif
    const size_t maxStackSize = 1UL * MB; // max: 1GB.
    return ReadSizeParameter("cjStackSize", defaultStackSize, minStackSize, EnvBoundKind::CLOSED,
                             maxStackSize, EnvBoundKind::CLOSED);
}

/**
 * Determine the max concurrency processors of cangjie program.
 * If the environment variable `cjProcessorNum` is set, validate it against the GC-specific processor limit.
 * MSGC permits up to CPU_CORE * 20 for high-core scale testing, while CopyGC keeps the CPU_CORE * 2 limit.
 * Otherwise check whether the value of CPU_CORE is 0, use it if not.
 * Otherwise use the default value 8.
 */
static uint32_t InitProcessorNum()
{
    unsigned int cpus = std::thread::hardware_concurrency();
    uint32_t defaultProcs = cpus != 0 ? static_cast<uint32_t>(cpus) : 8;
#ifdef MRT_USE_MARK_SWEEP
    size_t maxProcessorNum = ResolveProcessorLimit(cpus, true);
#else
    size_t maxProcessorNum = ResolveProcessorLimit(cpus, false);
#endif
    maxProcessorNum = std::min(maxProcessorNum, static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
    return static_cast<uint32_t>(ReadCountParameter("cjProcessorNum", defaultProcs, 0, EnvBoundKind::OPEN,
                                                    maxProcessorNum, EnvBoundKind::CLOSED));
}

void* StartMainTask(void* arg, unsigned int len)
{
    ScheduleHandle scheduler = MapleRuntime::Runtime::Current().GetConcurrencyModel().GetThreadScheduler();
    ASSERT(arg != nullptr);
    auto lwtData = static_cast<LWTData*>(arg);
    auto execute = lwtData->execute;
    Mutator* mutator = MutatorManager::Instance().CreateMutator();
    uintptr_t threadData = MRT_GetThreadLocalData();
    MRT_PreRunManagedCode(mutator, 0, reinterpret_cast<ThreadLocalData*>(threadData));
    reinterpret_cast<ThreadLocalData*>(threadData)->isCJProcessor = true;
    int32_t res = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(ExecuteCangjieStub(0, 0, 0, execute, reinterpret_cast<void*>(threadData), 0)));
#ifdef CANGJIE_TSAN_SUPPORT
    MapleRuntime::Sanitizer::TsanFinalize();
#endif
#ifdef __OHOS__
    TRACE_FINISH_ASYNC(TRACE_CJTHREAD_EXEC, CJThreadId());
    TRACE_START_ASYNC(TRACE_CJTHREAD_EXIT, CJThreadId());
#elif defined(__ANDROID__)
    TRACE_FINISH();
    TRACE_START(MapleRuntime::TraceInfoFormat(TRACE_CJTHREAD_EXIT, CJThreadId()));
#endif
    CpuProfiler::GetInstance().TryStopSampling();
    RTErrorCode rtCode = SetRuntimeFiniFlag();
    if (rtCode != E_OK) {
        LOG(RTLOG_FATAL, "Finish cj runtime failed for %d\n", rtCode);
    }
    MutatorManager::Instance().TransitMutatorToExit();
    ScheduleStop(scheduler);
    CangjieRuntime::FiniAndDelete();
#ifdef __OHOS__
    TRACE_FINISH_ASYNC(TRACE_CJTHREAD_EXIT, CJThreadId());
#elif defined(__ANDROID__)
    TRACE_FINISH();
#endif
    // If the return value is 0, func may not be successful.
    // If the value of res is a wild value (cj exception), -1 is returned.
    constexpr uint8_t errRet = 255;
    if (res > errRet || res < 0) {
        res = -1;
    }
    exit(res);
}

// The following functions are used to create threads with futures.
#if defined(__APPLE__) && defined(__aarch64__)
using FutureFunc = UnitType (*)(void*, void*, void*);
#else
using FutureFunc = UnitType (*)(...);
#endif
static UnitType g_ut;

void* WrapperTask(void* arg, unsigned int len)
{
    ASSERT(arg != nullptr);
    auto lwtData = static_cast<LWTData*>(arg);
    auto execute = lwtData->execute;
    uintptr_t threadData = MRT_GetThreadLocalData();
    // mutator has been set to a valid pointer before.
    Mutator* mutator = reinterpret_cast<ThreadLocalData*>(threadData)->mutator;
    MRT_PreRunManagedCode(mutator, 0, reinterpret_cast<ThreadLocalData*>(threadData));
    BaseObject* future = Heap::GetBarrier().ReadStaticRef(*(reinterpret_cast<RefField<>*>(&lwtData->obj)));
    TypeInfo* typeInfo = future->GetTypeInfo();
#if defined(__aarch64__)
    ExecuteCangjieStub(future, typeInfo, 0, execute, reinterpret_cast<void*>(threadData), &g_ut);
#elif defined(__arm__)
    ExecuteCangjieStub(&g_ut, future, typeInfo, execute, threadData);
#elif defined(__x86_64__)
    ExecuteCangjieStub(&g_ut, future, typeInfo, execute, threadData);
#endif
    MutatorManager::Instance().TransitMutatorToExit();
    return nullptr;
}

void* MCC_NewCJThread(void* execute, void* future, void* scheduler)
{
#if defined(CANGJIE_TSAN_SUPPORT)
    void *pc = __builtin_return_address(0);
    MapleRuntime::Sanitizer::TsanFuncEntry(pc);
#endif
    LWTData data;
    data.execute = execute;
    data.threadObject = nullptr;
    data.obj = nullptr;
    RefField<>* runtimeRootField = reinterpret_cast<RefField<>*>(&data.obj);
    Heap::GetBarrier().WriteStaticRef(*runtimeRootField, reinterpret_cast<BaseObject*>(future));
    if (!scheduler) {
        scheduler = MapleRuntime::Runtime::Current().GetConcurrencyModel().GetThreadScheduler();
    }
    CJThreadHandle handle = CJThreadNew(scheduler, nullptr, WrapperTask, &data, sizeof(LWTData));
    if (handle == nullptr) {
#if defined(CANGJIE_TSAN_SUPPORT)
        MapleRuntime::Sanitizer::TsanFuncExit();
#endif
        return nullptr;
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    MapleRuntime::Sanitizer::TsanFuncExit();
#endif
    return handle;
}

bool MRT_TryNewAndRunCJThread()
{
    if (ThreadLocal::IsCJProcessor() || ThreadLocal::GetMutator() != nullptr) {
        return false;
    }
    TRACE_START("CJRT_INVOKE_CJTASK");
    ScheduleHandle scheduler = nullptr;
    if (ThreadLocal::GetForeignCJThread() == nullptr) {
        auto runtime = reinterpret_cast<MapleRuntime::CangjieRuntime*>(&MapleRuntime::Runtime::Current());
        scheduler = runtime->CreateSubSchedulerAndInit(SCHEDULE_FOREIGN_THREAD);
        CJThreadAttr attr;
        CJThreadAttrInit(&attr);
        CJThreadAttrNameSet(&attr, "cangjie");
        LWTData data = {};
        CJThreadHandle cjthread = CJThreadNewToSchedule(scheduler, (const struct CJThreadAttr*)(&attr),
                                                        WrapperTask, &data, sizeof(LWTData));
        MutatorManager::Instance().SetMainThreadHandle(cjthread);
        CJThreadPreemptOffCntAdd();
        RebindCJThread(cjthread);
        ThreadLocal::SetForeignCJThread(cjthread);
        ThreadLocal::SetSchedule(scheduler);
        ThreadLocal::SetProtectAddr(nullptr);
        ScheduleNetpollInit(); // Initializes netpool only on the first execution.
    }

    void *cjthread = ThreadLocal::GetForeignCJThread();
    ThreadLocal::SetCJThread(cjthread);
    Mutator* mutator = reinterpret_cast<Mutator*>(CJThreadGetMutator());
    MutatorManager::Instance().BindMutator(*mutator);
    if (scheduler == nullptr) {
        scheduler = GetCJThreadScheduler();
        ThreadLocal::SetSchedule(scheduler);
        ThreadLocal::SetProtectAddr(nullptr);
    }
    mutator->InitForeignCJThread();
    // 1: state is SCHEDULE_RUNNING
    SetSchedulerState(1);
#ifdef __OHOS__
    TRACE_START_ASYNC(TRACE_CJTHREAD_EXEC, CJThreadGetId(cjthread));
#elif defined(__ANDROID__)
    TRACE_START(MapleRuntime::TraceInfoFormat(TRACE_CJTHREAD_EXEC, CJThreadGetId(cjthread)));
#endif
    return true;
}

bool MRT_EndCJThread()
{
    if (ThreadLocal::IsCJProcessor()) {
        return false;
    }
#if defined(__OHOS__)
    unsigned long long cjthreadId = CJThreadGetId(ThreadLocal::GetForeignCJThread());
    TRACE_FINISH_ASYNC(TRACE_CJTHREAD_EXEC, cjthreadId);
#elif defined(__ANDROID__)
    TRACE_FINISH();
#endif
    MapleRuntime::ExceptionManager::CheckAndDumpException();
    MapleRuntime::Runtime::Current().GetMutatorManager().TransitMutatorToExit();
    ThreadLocal::SetCJThread(nullptr);
    // 0: state is SCHEDULE_INIT
    SetSchedulerState(0);
    TRACE_FINISH();
    return true;
}

static void* WrapperExclusiveClosure(void* arg, unsigned int len)
{
    ASSERT(arg != nullptr);
    auto lwtData = static_cast<LWTData*>(arg);
    uintptr_t threadData = MRT_GetThreadLocalData();
    Mutator* mutator = reinterpret_cast<ThreadLocalData*>(threadData)->mutator;
    MRT_PreRunManagedCode(mutator, 0, reinterpret_cast<ThreadLocalData*>(threadData));
    lwtData->threadObject = nullptr;
    BaseObject* executeClosure = Heap::GetBarrier().ReadStaticRef(reinterpret_cast<RefField<false>&>(lwtData->execute));
    BaseObject* closureObj = Heap::GetBarrier().ReadStaticRef(reinterpret_cast<RefField<false>&>(lwtData->obj));
#if defined(__aarch64__)
    ExecuteExclusiveCangjieStub(closureObj, nullptr, executeClosure, reinterpret_cast<void*>(threadData), &g_ut);
#elif defined(__x86_64__)
    ExecuteExclusiveCangjieStub(&g_ut, nullptr, closureObj, executeClosure, reinterpret_cast<void*>(threadData));
#elif defined(__arm__)
    ExecuteExclusiveCangjieStub(&g_ut, nullptr, closureObj, executeClosure, reinterpret_cast<void*>(threadData));
#endif
    MutatorManager::Instance().TransitMutatorToExit();
    return nullptr;
}

void* MCC_NewExclusiveCJThread(void* executeClosure, void* closurePtr, void* futureTi) 
{
    LWTData data;
    data.execute = nullptr;
    data.obj = nullptr;
    data.threadObject = futureTi;
    RefField<>* executeRootField = reinterpret_cast<RefField<>*>(&data.obj);
    Heap::GetBarrier().WriteStaticRef(*executeRootField, reinterpret_cast<BaseObject*>(closurePtr));
    RefField<>* objRootField = reinterpret_cast<RefField<>*>(&data.execute);
    Heap::GetBarrier().WriteStaticRef(*objRootField, reinterpret_cast<BaseObject*>(executeClosure));
    return ExclusiveCJThreadNew(WrapperExclusiveClosure, &data, sizeof(LWTData));
}

/**
 * The thread entry function provided by runtime.
 * It is a wrapper function of the entry function from Cangjie frontend.
 */
static void* WrapperOfExecuteClosure(void* arg, unsigned int len)
{
    ASSERT(arg != nullptr);
    auto lwtData = static_cast<LWTData*>(arg);
    auto executeClosure = lwtData->execute;
    uintptr_t threadData = MRT_GetThreadLocalData();
    // mutator has been set to a valid pointer before.
    Mutator* mutator = reinterpret_cast<ThreadLocalData*>(threadData)->mutator;
    MRT_PreRunManagedCode(mutator, 0, reinterpret_cast<ThreadLocalData*>(threadData));
    TypeInfo* futureTi = static_cast<TypeInfo*>(lwtData->threadObject);
    // threadObject is used to pass TypeInfo of future. After use, need set to nullptr.
    lwtData->threadObject = nullptr;
    BaseObject* closureObj = Heap::GetBarrier().ReadStaticRef(reinterpret_cast<RefField<false>&>(lwtData->obj));
#if defined(__aarch64__)
    ExecuteCangjieStub(closureObj, futureTi, 0, executeClosure, reinterpret_cast<void*>(threadData), &g_ut);
#elif defined(__arm__)
    ExecuteCangjieStub(&g_ut, closureObj, futureTi, executeClosure, threadData);
#elif defined(__x86_64__)
    ExecuteCangjieStub(&g_ut,      /* arg0: sret */
                       closureObj, /* arg1 */
                       futureTi,
                       executeClosure, threadData);
#endif
    MutatorManager::Instance().TransitMutatorToExit();
    return nullptr;
}

void* MCC_NewCJThreadNoReturn(void* executeClosure, void* closurePtr, void* scheduler, void* futureTi)
{
    LWTData data;
    data.execute = executeClosure;
    data.obj = nullptr;
    data.threadObject = futureTi; // used to pass TypeInfo of future
    RefField<>* runtimeRootField = reinterpret_cast<RefField<>*>(&data.obj);
    Heap::GetBarrier().WriteStaticRef(*runtimeRootField, reinterpret_cast<BaseObject*>(closurePtr));
    if (!scheduler) {
        scheduler = MapleRuntime::Runtime::Current().GetConcurrencyModel().GetThreadScheduler();
    }
    CJThreadHandle handle =
        CJThreadNewToSchedule(scheduler, nullptr, WrapperOfExecuteClosure, &data, sizeof(LWTData));
    return handle;
}

/**
 * Determine the default stack size and initial heap size.
 * CopyGC derives heap size from system memory and cjHeapSize.
 * MSGC resolves Shard count and initial committed Segment count independently.
 */
static RuntimeParam InitRuntimeParam()
{
    CheckSysmemSize();
    unsigned int cpus = std::thread::hardware_concurrency();
    uint32_t logicalCpuCount = cpus != 0 ? static_cast<uint32_t>(cpus) : 8;
    uint32_t processorNum = InitProcessorNum();
#ifdef MRT_USE_MARK_SWEEP
    size_t maxVirtualHeapSizeKB = InitMSMaxVirtualHeapSize(kMSShardSegmentBytes / KB);
    CangjieRuntime::SetMSMaxVirtualHeapSizeKB(maxVirtualHeapSizeKB);
    CangjieRuntime::ConfigureMSAllocatorScale(processorNum, maxVirtualHeapSizeKB);
    MSAllocatorScaleConfig scaleConfig = CangjieRuntime::GetMSAllocatorScaleConfig();
    size_t initHeapSize = ComputeMSInitialHeapSizeKB(scaleConfig.initialSegmentCount);
    double heapGrowth = ReadDoubleParameter("cjHeapGrowth", 1.0, 0.0, EnvBoundKind::OPEN,
                                            std::numeric_limits<double>::max(), EnvBoundKind::CLOSED);
#else
    size_t initHeapSize = InitHeapSize(g_sysmemSize > 1 * GB ? 256 * KB : 64 * KB);
    double heapGrowth = ReadDoubleParameter("cjHeapGrowth", 0.25, 0.0, EnvBoundKind::OPEN,
                                            std::numeric_limits<double>::max(), EnvBoundKind::CLOSED);
#endif
    uint64_t gcInterval = ReadTimeParameter("cjGCInterval", 0, 0, EnvBoundKind::OPEN, true);
    RuntimeParam param = {
        .heapParam = {
#if defined(__OHOS__) || defined(__ANDROID__)
            // Default region size is 1024KB.
            .regionSize = InitRegionSize(1024UL),
#else
            // Default region size is 64KB.
            .regionSize = InitRegionSize(64UL),
#endif
            // Heap size is derived above and represents the initial committed heap size.
            .heapSize = initHeapSize,
            /*
             * The minimux live region threshold is 0% of region,
             * the maximum is 100% of region, default to 80% of region.
             */
            .exemptionThreshold = ReadDoubleParameter("cjExemptionThreshold", 0.8, 0.0, EnvBoundKind::OPEN,
                                                      1.0, EnvBoundKind::CLOSED),
            /*
             * The minimux heap utilization is 0% of heap,
             * the maximum is 100% of heap, default to 80% of heap.
             */
            .heapUtilization = ReadDoubleParameter("cjHeapUtilization", 0.8, 0.0, EnvBoundKind::OPEN,
                                                   1.0, EnvBoundKind::CLOSED),
            .heapGrowth = heapGrowth,
            // Default allocation rate is 10240MB/s.
            .allocationRate = ReadDoubleParameter("cjAllocationRate", 10240, 0.0, EnvBoundKind::OPEN,
                                                  std::numeric_limits<double>::max(), EnvBoundKind::CLOSED),
            // Default allocation wait time is 1000ns.
            .allocationWaitTime = static_cast<size_t>(ReadTimeParameter("cjAllocationWaitTime", 1000, 0,
                                                                        EnvBoundKind::OPEN)),
        },
        .gcParam = {
            // gcThreshold is derived from the current heap size and is not externally configurable.
            .gcThreshold = initHeapSize * KB,
            // Default garbage ration is 50% of from space.
            .garbageThreshold = ReadDoubleParameter("cjGarbageThreshold", 0.5, 0.0, EnvBoundKind::CLOSED,
                                                    1.0, EnvBoundKind::CLOSED),
            // Default GC interval is 0, which disables the interval gate.
            .gcInterval = gcInterval,
            // Default backup GC interval is 240s.
            .backupGCInterval = ReadTimeParameter("cjBackupGCInterval", 240 * SECOND_TO_NANO_SECOND, 0,
                                                  EnvBoundKind::OPEN),
#ifdef MRT_USE_MARK_SWEEP
            // Total MSGC mark threads, including the GC main thread.
            .gcThreads = static_cast<int32_t>(ReadCountParameter(
                "cjGCThreads", ResolveMSMarkTotalThreads(processorNum, logicalCpuCount, 0),
                0, EnvBoundKind::OPEN,
                processorNum, EnvBoundKind::CLOSED)),
#else
            // CopyGC keeps one GC main thread and one helper thread.
            .gcThreads = 2,
#endif
            .scavengeGrowth = ReadDoubleParameter("cjScavengeGrowth", heapGrowth, 0.0, EnvBoundKind::OPEN,
                                                  std::numeric_limits<double>::max(), EnvBoundKind::CLOSED),
#ifdef MRT_USE_MARK_SWEEP
            .msBackgroundSweepMaxTime = ReadTimeParameter("cjGCBGSweepMaxTime", 2 * MILLI_SECOND_TO_NANO_SECOND,
                                                          0, EnvBoundKind::OPEN, true),
            .msMarkTimeBudget = ReadTimeParameter("cjMSMarkTimeBudget", 20 * SECOND_TO_NANO_SECOND,
                                                  0, EnvBoundKind::OPEN, true),
            .msAssistBaseRatioPercent = ReadCountParameter(
                "cjMSAssistBaseRatioPercent", kMSAssistBaseRatioPercentDefault, 0,
                                                           EnvBoundKind::OPEN),
            .msAssistBacklogStepBuffers = ReadCountParameter("cjMSAssistBacklogStepBuffers", 16, 0,
                                                              EnvBoundKind::OPEN),
            .msAssistMaxMultiplier = ReadCountParameter("cjMSAssistMaxMultiplier", 4, 0,
                                                         EnvBoundKind::OPEN),
            .msAssistCreditDecayRounds = ReadCountParameter("cjMSAssistCreditDecayRounds", 100, 0,
                                                            EnvBoundKind::OPEN),
            .msAssistSweepMaxRegionsPerCall = ReadCountParameter("cjMSAssistSweepMaxRegionsPerCall", 4, 0,
                                                                 EnvBoundKind::OPEN),
            .msEnumAssistBatchSize = ReadCountParameter("cjMSEnumAssistBatchSize", 10, 0, EnvBoundKind::OPEN),
            .msReleasedBusyRetryLimit = 0,
            .msScavengeRounds = ReadCountParameter("cjMSScavengeRounds", 8, 0, EnvBoundKind::OPEN),
            .msBackgroundSweep = static_cast<uint8_t>(ReadBoolParameter("cjMSBackgroundSweep", true) ? 1U : 0U),
            .reservedMSBackgroundFeed = 0,
            .msBackgroundScavenge =
                static_cast<uint8_t>(ReadBoolParameter("cjMSBackgroundScavenge", true) ? 1U : 0U),
            .msBackgroundShrink =
                static_cast<uint8_t>(ReadBoolParameter("cjMSBackgroundShrink", false) ? 1U : 0U),
            .msVerifyMark = static_cast<uint8_t>(ReadBoolParameter("cjMSVerifyMark", false) ? 1U : 0U),
            .msVerifyMarkDetail =
                static_cast<uint8_t>(ReadBoolParameter("cjMSVerifyMarkDetail", false) ? 1U : 0U),
            .msParallelSweepMaxHelpers = ReadCountParameter(
                "cjMSParallelSweepMaxHelpers", 0, 0, EnvBoundKind::CLOSED),
            .msParallelSweep =
                static_cast<uint8_t>(ReadBoolParameter("cjMSParallelSweep", true) ? 1U : 0U),
#endif
        },
        .logParam = {
            .logLevel = LogFile::GetLogLevel(),
        },
        .coParam = {
            // Default thread stack size is 1 * KB KB = 1MB.
            .thStackSize = 1 * KB,
            .coStackSize = InitCoStackSize(),
            .processorNum = processorNum,
        }
    };
    return param;
}

void MRT_CjRuntimeInit()
{
    RuntimeParam param = InitRuntimeParam();
    CangjieRuntime::CreateAndInit(param);
    RTErrorCode rtCode = SetRuntimeInitFlag();
    if (rtCode != E_OK) {
        LOG(RTLOG_FATAL, "Init cj runtime failed for %d\n", rtCode);
    }
}

void MRT_SetCommandLineArgs(int argc, const char* argv[])
{
    reinterpret_cast<CangjieRuntime*>(&MapleRuntime::Runtime::Current())->SetCommandLinesArgs(argc, argv);
    return;
}

const char** MRT_GetCommandLineArgs()
{
    return reinterpret_cast<CangjieRuntime*>(&MapleRuntime::Runtime::Current())->GetCommandLineArgs();
}

void MRT_CjRuntimeStart(void* execute)
{
    ScheduleHandle scheduler = MapleRuntime::Runtime::Current().GetConcurrencyModel().GetThreadScheduler();
    LWTData lwtData;
    lwtData.execute = execute;
    lwtData.fn = nullptr;
    lwtData.obj = nullptr;
    lwtData.threadObject = nullptr;
    CJThreadAttr attr;
    CJThreadAttrInit(&attr);
    CJThreadAttrStackSizeSet(&attr, g_initStackSize * KB); // Set main task stack size.
    CJThreadAttrNameSet(&attr, "cangjie");
    CJThreadHandle cjthread = CJThreadNew(scheduler, &attr, StartMainTask, &lwtData, sizeof(LWTData));
    MutatorManager::Instance().SetMainThreadHandle(cjthread);
    ScheduleStart();
}

int MRT_NewWaitQueue(void* waitQueuePtr) { return WaitqueueNew(reinterpret_cast<Waitqueue*>(waitQueuePtr)); }

bool ReportWaitQueueRetMsg(int err)
{
    if (err == 0) {
        return true;
    }
    if (err == WAIT_QUEUE_TIMEOUT) {
        return false;
    }
    if (err == ERRNO_QUEUE_DOESNT_EXIST) {
        LOG(RTLOG_INFO, "The waiting queue passed by the user does not exist!\n");
    }
    if (err == ERRNO_TIMER_STOP_FAILED) {
    }
    if (err == ERRNO_QUEUE_IS_EMPTY) {
    }
    if (err == ERRNO_CALLBACK_RETURN_TRUE) {
    }
    if (err == ERRNO_MALLOC_FAILED) {
        LOG(RTLOG_INFO, "Memory allocation failed!\n");
    }
    if (err == ERRNO_TIMER_STATE_INVALID) {
        LOG(RTLOG_INFO, "Illegal waiting time: time cannot be negative!\n");
    }
    return true;
}

void ReportSemRetMsg(int err)
{
    if (err == 0) {
        return;
    }
    if (err == ERRNO_SEMA_DOESNT_EXIST) {
        LOG(RTLOG_ERROR, "The semaphore transferred by the user does not exist!\n");
    }
}

bool MRT_SuspendWithTimeout(void* wq, WqCallbackFunc callBack, void* callBackObj, int64_t timeOut)
{
    auto handle = reinterpret_cast<Waitqueue*>(wq);
    return ReportWaitQueueRetMsg(WaitqueuePark(handle, timeOut, callBack, callBackObj, false));
}

void MRT_ResumeOne(void* wq, WqCallbackFunc callBack, void* callBackObj)
{
    auto handle = reinterpret_cast<Waitqueue*>(wq);
    ReportWaitQueueRetMsg(WaitqueueWakeOne(handle, callBack, callBackObj));
}

void MRT_ResumeAll(void* wq, WqCallbackFunc callBack, void* callBackObj)
{
    auto handle = reinterpret_cast<Waitqueue*>(wq);
    ReportWaitQueueRetMsg(WaitqueueWakeAll(handle, callBack, callBackObj));
}

int MRT_NewSem(void* semPtr) { return SemaNew(reinterpret_cast<Sema*>(semPtr), 0); }

void MRT_SemAcquire(void* sem, bool isPushToHead)
{
    auto handle = reinterpret_cast<Sema*>(sem);
    ReportSemRetMsg(SemaAcquire(handle, isPushToHead));
}

void MRT_SemRelease(void* sem)
{
    auto handle = reinterpret_cast<Sema*>(sem);
    ReportSemRetMsg(SemaRelease(handle));
}

int64_t MRT_GetCurrentThreadID() { return static_cast<int64_t>(CJThreadId()); }

struct SubSchedulerContextData {
    ScheduleHandle schedule;
    std::condition_variable* conditionVar;
    std::atomic_bool* inited;
    std::string threadName;
};

void* MRT_StartSubScheduler(void* args)
{
    auto runtime = reinterpret_cast<CangjieRuntime*>(&MapleRuntime::Runtime::Current());
    struct SubSchedulerContextData* data = static_cast<struct SubSchedulerContextData*>(args);
    void* schedule = runtime->CreateSubSchedulerAndInit();
#ifdef __APPLE__
    CHECK_PTHREAD_CALL(pthread_setname_np, (data->threadName.c_str()), "set sub-scheduler thread name");
#elif defined(__linux__) || defined(hongmeng)
    CHECK_PTHREAD_CALL(prctl, (PR_SET_NAME, data->threadName.c_str()), "set sub-scheduler thread name");
#endif
    data->schedule = schedule;
    data->inited->store(true);
    data->conditionVar->notify_all();
    ThreadLocal::SetCJProcessorFlag(true);
    ScheduleStart();
    return nullptr;
}

int8_t MRT_StopSubScheduler(void* schedule)
{
    auto runtime = reinterpret_cast<CangjieRuntime*>(&MapleRuntime::Runtime::Current());
    if (!runtime->FiniSubScheduler(schedule)) {
        LOG(RTLOG_ERROR, "Fail to stop sub-scheduler");
        return 1;
    }
    return 0;
}

static void ResolveCycleRefImpl()
{
    Heap::GetHeap().GetCollector().ResolveCycleRef();
}
extern "C" void CJ_MRT_RolveCycleRef()
{
    RunResolveCycle(reinterpret_cast<void*>(&ResolveCycleRefImpl));
}

const void* MRT_RuntimeNewSubScheduler()
{
    static int id = 0;
    std::condition_variable conditionVariable;
    std::atomic_bool subScheduleInited = ATOMIC_VAR_INIT(false);
    std::mutex mtx;
    struct SubSchedulerContextData args = {
        .schedule = nullptr,
        .conditionVar = &conditionVariable,
        .inited = &subScheduleInited,
        .threadName = ""
    };

    pthread_t thread;
    pthread_attr_t attr;
    size_t stackSize = CangjieRuntime::GetConcurrencyParam().thStackSize * MapleRuntime::KB;
    std::string threadName = "sub-schedule" + std::to_string(id++);
    args.threadName = threadName;

    CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "init pthread attr");
    CHECK_PTHREAD_CALL(pthread_attr_setdetachstate, (&attr, PTHREAD_CREATE_JOINABLE), "set pthread joinable");
    CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stackSize), "set pthread stacksize");
    CHECK_PTHREAD_CALL(pthread_create, (&thread, &attr, MRT_StartSubScheduler, &args), "create sub-scheduler thread");
#ifdef __WIN64
    CHECK_PTHREAD_CALL(pthread_setname_np, (thread, threadName.c_str()), "set sub-scheduler thread name");
#endif
    CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "destroy pthread attr");
    // Waiting for runtime initialize completely before continue.
    std::unique_lock<std::mutex> lck(mtx);
    conditionVariable.wait(lck, [&subScheduleInited]() { return subScheduleInited.load(); });
    return args.schedule;
}

#ifdef __APPLE__
MRT_EXPORT void CJ_MRT_CjRuntimeInit();
__asm__(".global _CJ_MRT_CjRuntimeInit\n\t.set _CJ_MRT_CjRuntimeInit, _MRT_CjRuntimeInit");
MRT_EXPORT void CJ_MRT_CjRuntimeStart(void* execute);
__asm__(".global _CJ_MRT_CjRuntimeStart\n\t.set _CJ_MRT_CjRuntimeStart, _MRT_CjRuntimeStart");
MRT_EXPORT void* CJ_MCC_NewCJThread(void* execute, void* future, void* scheduler);
__asm__(
    ".global _CJ_MCC_NewCJThread\n\t.set _CJ_MCC_NewCJThread, _MCC_NewCJThread");
MRT_EXPORT void* CJ_MCC_NewCJThreadNoReturn(void* executeClosure, void* closurePtr, void* scheduler);
__asm__(
    ".global _CJ_MCC_NewCJThreadNoReturn\n\t.set _CJ_MCC_NewCJThreadNoReturn, "
    "_MCC_NewCJThreadNoReturn");
MRT_EXPORT void CJ_MRT_SetCommandLineArgs(int argc, const char* argv[]);
__asm__(".global _CJ_MRT_SetCommandLineArgs\n\t.set _CJ_MRT_SetCommandLineArgs, _MRT_SetCommandLineArgs");
MRT_EXPORT const char** CJ_MRT_GetCommandLineArgs();
__asm__(".global _CJ_MRT_GetCommandLineArgs\n\t.set _CJ_MRT_GetCommandLineArgs, _MRT_GetCommandLineArgs");
MRT_EXPORT const void* CJ_MRT_RuntimeNewSubScheduler();
__asm__(".global _CJ_MRT_RuntimeNewSubScheduler\n\t.set _CJ_MRT_RuntimeNewSubScheduler, _MRT_RuntimeNewSubScheduler");
MRT_EXPORT int8_t CJ_MRT_StopSubScheduler(void* schedule);
__asm__(".global _CJ_MRT_StopSubScheduler\n\t.set _CJ_MRT_StopSubScheduler, _MRT_StopSubScheduler");
#else
MRT_EXPORT void CJ_MRT_CjRuntimeInit() __attribute__((alias("MRT_CjRuntimeInit")));
MRT_EXPORT void CJ_MRT_CjRuntimeStart(void* execute)
    __attribute__((alias("MRT_CjRuntimeStart")));
MRT_EXPORT void* CJ_MCC_NewCJThread(void* execute, void* future, void *scheduler)
    __attribute__((alias("MCC_NewCJThread")));
MRT_EXPORT void* CJ_MCC_NewCJThreadNoReturn(void* executeClosure, void* closurePtr, void* scheduler)
    __attribute__((alias("MCC_NewCJThreadNoReturn")));
MRT_EXPORT void CJ_MRT_SetCommandLineArgs(int argc, const char* argv[])
    __attribute__((alias("MRT_SetCommandLineArgs")));
MRT_EXPORT const char** CJ_MRT_GetCommandLineArgs() __attribute__((alias("MRT_GetCommandLineArgs")));
MRT_EXPORT const void* CJ_MRT_RuntimeNewSubScheduler() __attribute__((alias("MRT_RuntimeNewSubScheduler")));
MRT_EXPORT int8_t CJ_MRT_StopSubScheduler(void* schedule) __attribute__((alias("MRT_StopSubScheduler")));
#endif
#ifdef __cplusplus
};
#endif
} // namespace MapleRuntime

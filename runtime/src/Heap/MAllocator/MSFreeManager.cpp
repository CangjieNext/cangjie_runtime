// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSFreeManager.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#ifndef _WIN64
#include <sys/mman.h>
#else
#include <windows.h>
#endif

#include "Base/CString.h"
#include "Base/MemUtils.h"
#include "Base/Log.h"
#include "Base/TimeUtils.h"
#include "Common/ScopedObjectAccess.h"

namespace MapleRuntime {

namespace {

class CartesianTreeBackend final : public FreeManager::Backend {
public:
    using UnitIndex = FreeManager::UnitIndex;
    using UnitCount = FreeManager::UnitCount;

    std::unique_ptr<Backend> CreateEmpty() const override
    {
        return std::make_unique<CartesianTreeBackend>();
    }

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
    mutable CartesianTree tree_;
};

bool EqualsIgnoreCase(const char* lhs, const char* rhs)
{
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    while (*lhs != '\0' && *rhs != '\0') {
        char lc = static_cast<char>(*lhs);
        char rc = static_cast<char>(*rhs);
        if (lc >= 'A' && lc <= 'Z') {
            lc = static_cast<char>(lc - 'A' + 'a');
        }
        if (rc >= 'A' && rc <= 'Z') {
            rc = static_cast<char>(rc - 'A' + 'a');
        }
        if (lc != rc) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == '\0' && *rhs == '\0';
}

bool ReadReleasedDiagEnabled()
{
    const char* env = std::getenv("CJ_MS_RELEASED_DIAG");
    if (env == nullptr) {
        return false;
    }
    if (env[0] == '\0') {
        return true;
    }
    if (strcmp(env, "0") == 0 || EqualsIgnoreCase(env, "false") ||
        EqualsIgnoreCase(env, "off") || EqualsIgnoreCase(env, "no")) {
        return false;
    }
    return true;
}

inline uint64_t NowNs()
{
    return TimeUtil::NanoSeconds();
}

inline double NsToMs(uint64_t ns)
{
    return static_cast<double>(ns) / 1000000.0;
}

inline double AverageUs(uint64_t totalNs, uint64_t count)
{
    if (count == 0) {
        return 0.0;
    }
    return static_cast<double>(totalNs) / static_cast<double>(count) / 1000.0;
}

} // namespace

FreeManager::FreeManager() : FreeManager(nullptr)
{
}

FreeManager::FreeManager(std::unique_ptr<Backend> backend)
    : backendPrototype_(backend == nullptr ? std::make_unique<CartesianTreeBackend>() : std::move(backend))
{
}

FreeManager::~FreeManager()
{
    ReportDiagnostics();
    if (producerBank_.backend != nullptr) {
        producerBank_.backend->Finalize();
    }
    if (consumerBank_.backend != nullptr) {
        consumerBank_.backend->Finalize();
    }
}

void FreeManager::Initialize(UnitCount maxUnitCount, size_t unitSize, size_t unitsPerRegion,
                             RegionCount committedRegionCount, RegionCount maxVirtualRegionCount,
                             uintptr_t heapStart, uintptr_t committedEnd, uintptr_t maxHeapEnd,
                             size_t segmentSize)
{
    CHECK_DETAIL(unitSize != 0, "[MS-GC] FreeManager unitSize must not be zero");
    CHECK_DETAIL(unitsPerRegion != 0, "[MS-GC] FreeManager unitsPerRegion must not be zero");
    CHECK_DETAIL(heapStart <= committedEnd, "[MS-GC] FreeManager committed end is below heap start");
    CHECK_DETAIL(committedEnd <= maxHeapEnd, "[MS-GC] FreeManager max heap end is below committed end");

    unitSize_ = unitSize;
    unitsPerRegion_ = unitsPerRegion;
    heapStart_ = heapStart;
    committedEnd_ = committedEnd;
    maxHeapEnd_ = maxHeapEnd;
    committedRegionCount_ = static_cast<size_t>(committedRegionCount);
    committedUnitCount_ = static_cast<size_t>(RegionCountToUnitCount(committedRegionCount));
    maxVirtualRegionCount_ = static_cast<size_t>(maxVirtualRegionCount);
    maxVirtualUnitCount_ = static_cast<size_t>(maxUnitCount);
    segmentSize_ = segmentSize;
    inactiveZone_.store(heapStart, std::memory_order_relaxed);
    diagnostics_.enabled = ReadReleasedDiagEnabled();

    producerBank_.backend = backendPrototype_->CreateEmpty();
    producerBank_.backend->Initialize(maxUnitCount);
    producerBank_.rangeStart = 0;
    producerBank_.rangeEnd = static_cast<UnitIndex>(maxVirtualUnitCount_);
    producerBank_.epoch = 0;
    producerBank_.totalUnits.store(0, std::memory_order_relaxed);
    producerBank_.maxBlockUnits.store(0, std::memory_order_relaxed);

    InitializeConsumerBank();
}

FreeManager::TryTakeResult FreeManager::TryTakeRegions(RegionCount num, RegionIndex& idx)
{
    const bool diagEnabled = diagnostics_.enabled;
    const uint64_t startNs = diagEnabled ? NowNs() : 0;
    UnitCount unitCount = RegionCountToUnitCount(num);
    bool busySeen = false;
    uint64_t visitedBanks = 0;
    bool publishTriggered = false;

    if (diagEnabled) {
        diagnostics_.takeCalls.fetch_add(1, std::memory_order_relaxed);
        diagnostics_.takeRequestedUnits.fetch_add(unitCount, std::memory_order_relaxed);
    }

    if (consumerBank_.backend == nullptr) {
        if (diagEnabled) {
            diagnostics_.takeEmptyReturns.fetch_add(1, std::memory_order_relaxed);
            diagnostics_.takeTotalNs.fetch_add(NowNs() - startNs, std::memory_order_relaxed);
        }
        return TryTakeResult::EMPTY;
    }

    for (size_t attempt = 0; attempt < 2; ++attempt) {
        ReleaseBank& bank = consumerBank_;
        ++visitedBanks;
        if (bank.totalUnits.load(std::memory_order_relaxed) < unitCount ||
            bank.maxBlockUnits.load(std::memory_order_relaxed) < unitCount) {
            if (diagEnabled) {
                diagnostics_.takeSummarySkipBanks.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        if (!bank.mutex.try_lock()) {
            busySeen = true;
            if (diagEnabled) {
                diagnostics_.takeLockBusyBanks.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        std::lock_guard<std::mutex> lg(bank.mutex, std::adopt_lock);

        UnitIndex unitIdx = 0;
        if (!bank.backend->TakeUnits(unitCount, unitIdx)) {
            RefreshBankSummaryLocked(bank);
            if (diagEnabled) {
                diagnostics_.takeBackendMissBanks.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            RefreshBankSummaryLocked(bank);
            idx = UnitToRegionIdx(unitIdx);
            if (diagEnabled) {
                diagnostics_.takeVisitedBanks.fetch_add(visitedBanks, std::memory_order_relaxed);
                diagnostics_.takeSuccessCalls.fetch_add(1, std::memory_order_relaxed);
                diagnostics_.takeTotalNs.fetch_add(NowNs() - startNs, std::memory_order_relaxed);
            }
            return TryTakeResult::SUCCESS;
        }

        if (busySeen || publishTriggered || producerBank_.totalUnits.load(std::memory_order_relaxed) == 0) {
            break;
        }

        publishTriggered = true;
        if (diagEnabled) {
            diagnostics_.takePublishOnMissAttempts.fetch_add(1, std::memory_order_relaxed);
        }
        if (!PublishProducerToConsumers(false)) {
            break;
        }
        if (diagEnabled) {
            diagnostics_.takePublishOnMissSuccess.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (diagEnabled) {
        diagnostics_.takeVisitedBanks.fetch_add(visitedBanks, std::memory_order_relaxed);
        if (busySeen) {
            diagnostics_.takeBusyReturns.fetch_add(1, std::memory_order_relaxed);
        } else {
            diagnostics_.takeEmptyReturns.fetch_add(1, std::memory_order_relaxed);
            if (producerBank_.totalUnits.load(std::memory_order_relaxed) != 0) {
                diagnostics_.takeEmptyWithProducerUnits.fetch_add(1, std::memory_order_relaxed);
            }
            UnitCount consumerUnits = consumerBank_.totalUnits.load(std::memory_order_relaxed);
            if (consumerUnits != 0) {
                diagnostics_.takeEmptyWithConsumerUnits.fetch_add(1, std::memory_order_relaxed);
            }
        }
        diagnostics_.takeTotalNs.fetch_add(NowNs() - startNs, std::memory_order_relaxed);
    }

    return busySeen ? TryTakeResult::BUSY : TryTakeResult::EMPTY;
}

void FreeManager::RecordTakeBusyFallback()
{
    if (!diagnostics_.enabled) {
        return;
    }
    diagnostics_.takeBusyFallbacks.fetch_add(1, std::memory_order_relaxed);
}

bool FreeManager::TryIncreaseInactiveZone(RegionCount num, RegionIndex& idx)
{
    size_t byteCount = static_cast<size_t>(num) * GetRegionSizeBytes();
    uintptr_t oldAddr = inactiveZone_.load(std::memory_order_relaxed);
    do {
        if (oldAddr + byteCount > committedEnd_) {
            return false;
        }
    } while (!inactiveZone_.compare_exchange_weak(oldAddr, oldAddr + byteCount,
                                                  std::memory_order_relaxed));

    idx = AddrToRegionIdx(oldAddr);
    return true;
}

void FreeManager::InsertRegions(RegionIndex idx, RegionCount num)
{
    InsertRegionsToBank(producerBank_, idx, num);
}

void FreeManager::InsertRegionsToBank(ReleaseBank& bank, RegionIndex idx, RegionCount num)
{
    std::lock_guard<std::mutex> lg(bank.mutex);
    InsertRegionsToBankLocked(bank, idx, num);
}

void FreeManager::InsertRegionsToBankLocked(ReleaseBank& bank, RegionIndex idx, RegionCount num)
{
    const bool diagEnabled = diagnostics_.enabled;
    const uint64_t startNs = diagEnabled ? NowNs() : 0;
    UnitIndex unitIdx = RegionToUnitIdx(idx);
    UnitCount unitCount = RegionCountToUnitCount(num);
    if (!bank.backend->MergeInsert(unitIdx, unitCount)) {
        LOG(RTLOG_FATAL, "[MS-GC] FreeManager: failed to insert released regions [%u+%u, %u)",
            idx, num, idx + num);
    }
    RefreshBankSummaryLocked(bank);
    if (diagEnabled) {
        const uint64_t elapsedNs = NowNs() - startNs;
        if (&bank == &producerBank_) {
            diagnostics_.producerPublishCalls.fetch_add(1, std::memory_order_relaxed);
            diagnostics_.producerPublishUnits.fetch_add(unitCount, std::memory_order_relaxed);
            diagnostics_.producerPublishNs.fetch_add(elapsedNs, std::memory_order_relaxed);
        } else {
            diagnostics_.consumerPublishCalls.fetch_add(1, std::memory_order_relaxed);
            diagnostics_.consumerPublishUnits.fetch_add(unitCount, std::memory_order_relaxed);
            diagnostics_.consumerPublishNs.fetch_add(elapsedNs, std::memory_order_relaxed);
        }
    }
}

void FreeManager::RefreshBankSummaryLocked(ReleaseBank& bank) const
{
    bank.totalUnits.store(bank.backend->GetTotalCount(), std::memory_order_relaxed);
    bank.maxBlockUnits.store(bank.backend->GetMaxBlockCount(), std::memory_order_relaxed);
}

void FreeManager::ReleaseUnits(UnitIndex idx, UnitCount num) const
{
    uintptr_t addr = heapStart_ + static_cast<uintptr_t>(idx) * unitSize_;
    size_t size = static_cast<size_t>(num) * unitSize_;
#if defined(_WIN64)
    CHECK_E(UNLIKELY(!VirtualFree(reinterpret_cast<void*>(addr), size, MEM_DECOMMIT)),
            "VirtualFree failed in FreeManager::ReleaseUnits");
#elif defined(__APPLE__)
    MemorySet(addr, size, 0, size);
    void* ret = mmap(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (ret == MAP_FAILED) {
        LOG(RTLOG_ERROR, "[MS-GC] FreeManager::ReleaseUnits mmap fixed failed");
    }
#else
    (void)madvise(reinterpret_cast<void*>(addr), size, MADV_DONTNEED);
#endif
}

void FreeManager::ReclaimRegionsReleased(RegionIndex idx, RegionCount num)
{
    ReleaseUnits(RegionToUnitIdx(idx), RegionCountToUnitCount(num));
    ScopedEnterSaferegion enterSaferegion(true);
    InsertRegions(idx, num);
}

bool FreeManager::GrowOneSegment(HeapRangeHandler& handler)
{
    const bool diagEnabled = diagnostics_.enabled;
    const uint64_t startNs = diagEnabled ? NowNs() : 0;
    if (diagEnabled) {
        diagnostics_.growCalls.fetch_add(1, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lg(growMutex_);

    if (committedEnd_ >= maxHeapEnd_) {
        if (diagEnabled) {
            diagnostics_.growFailAtMaxHeap.fetch_add(1, std::memory_order_relaxed);
            diagnostics_.growTotalNs.fetch_add(NowNs() - startNs, std::memory_order_relaxed);
        }
        return false;
    }

    uintptr_t oldCommittedEnd = committedEnd_;
    size_t growBytes = segmentSize_;
    if (oldCommittedEnd + growBytes > maxHeapEnd_) {
        growBytes = maxHeapEnd_ - oldCommittedEnd;
    }

    RegionCount growRegions = static_cast<RegionCount>(growBytes / GetRegionSizeBytes());
    UnitCount growUnits = static_cast<UnitCount>(growBytes / unitSize_);
    RegionIndex oldRegionCount = static_cast<RegionIndex>(committedRegionCount_);

    handler.InitializeFreshRegions(oldRegionCount, growRegions);

    committedRegionCount_ += static_cast<size_t>(growRegions);
    committedUnitCount_ += static_cast<size_t>(growUnits);
    committedEnd_ = oldCommittedEnd + growBytes;

    handler.OnCommittedHeapExtended(committedEnd_);
    heapGrowCount_++;
    PublishProducerToConsumers(true);

    if (diagEnabled) {
        diagnostics_.growSuccessCalls.fetch_add(1, std::memory_order_relaxed);
        diagnostics_.growBytes.fetch_add(growBytes, std::memory_order_relaxed);
        diagnostics_.growTotalNs.fetch_add(NowNs() - startNs, std::memory_order_relaxed);
    }

    return true;
}

void FreeManager::InitializeConsumerBank()
{
    consumerBank_.backend = backendPrototype_->CreateEmpty();
    consumerBank_.backend->Initialize(static_cast<UnitCount>(maxVirtualUnitCount_));
    consumerBank_.rangeStart = 0;
    consumerBank_.rangeEnd = static_cast<UnitIndex>(maxVirtualUnitCount_);
    consumerBank_.epoch = 0;
    consumerBank_.totalUnits.store(0, std::memory_order_relaxed);
    consumerBank_.maxBlockUnits.store(0, std::memory_order_relaxed);
}

void FreeManager::ResetBankLocked(ReleaseBank& bank)
{
    bank.backend->Finalize();
    bank.backend->Initialize(static_cast<UnitCount>(maxVirtualUnitCount_));
    bank.totalUnits.store(0, std::memory_order_relaxed);
    bank.maxBlockUnits.store(0, std::memory_order_relaxed);
}

void FreeManager::RotateReleasedBanksOnGrow()
{
    (void)PublishProducerToConsumers(true);
}

bool FreeManager::PublishProducerToConsumers(bool dueToGrow)
{
    const bool diagEnabled = diagnostics_.enabled;
    const uint64_t totalStartNs = diagEnabled ? NowNs() : 0;
    if (diagEnabled && dueToGrow) {
        diagnostics_.rotateCalls.fetch_add(1, std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> rotateLock(rotateMutex_);

    if (producerBank_.totalUnits.load(std::memory_order_relaxed) == 0) {
        if (diagEnabled && dueToGrow) {
            diagnostics_.rotateTotalNs.fetch_add(NowNs() - totalStartNs, std::memory_order_relaxed);
        }
        return false;
    }

    std::unique_ptr<Backend> frozenBackend;
    std::vector<UnitSpan> frozenSpans;
    {
        std::lock_guard<std::mutex> producerLock(producerBank_.mutex);
        if (producerBank_.backend == nullptr || producerBank_.backend->GetTotalCount() == 0) {
            if (diagEnabled && dueToGrow) {
                diagnostics_.rotateTotalNs.fetch_add(NowNs() - totalStartNs, std::memory_order_relaxed);
            }
            return false;
        }
        frozenBackend = std::move(producerBank_.backend);
        producerBank_.backend = backendPrototype_->CreateEmpty();
        producerBank_.backend->Initialize(static_cast<UnitCount>(maxVirtualUnitCount_));
        producerBank_.totalUnits.store(0, std::memory_order_relaxed);
        producerBank_.maxBlockUnits.store(0, std::memory_order_relaxed);
        if (dueToGrow) {
            producerBank_.epoch = heapGrowCount_;
        }
    }

    const uint64_t collectStartNs = diagEnabled ? NowNs() : 0;
    frozenBackend->CollectUnitSpans(frozenSpans);
    frozenBackend->Finalize();
    if (diagEnabled && dueToGrow) {
        diagnostics_.rotateCollectNs.fetch_add(NowNs() - collectStartNs, std::memory_order_relaxed);
    }

    if (frozenSpans.empty()) {
        if (diagEnabled && dueToGrow) {
            diagnostics_.rotateTotalNs.fetch_add(NowNs() - totalStartNs, std::memory_order_relaxed);
        }
        return false;
    }

    if (diagEnabled && dueToGrow) {
        uint64_t movedUnits = 0;
        for (const UnitSpan& span : frozenSpans) {
            movedUnits += span.num;
        }
        diagnostics_.rotatePayloadCalls.fetch_add(1, std::memory_order_relaxed);
        diagnostics_.rotateMovedSpans.fetch_add(frozenSpans.size(), std::memory_order_relaxed);
        diagnostics_.rotateMovedUnits.fetch_add(movedUnits, std::memory_order_relaxed);
    }

    const uint64_t distributeStartNs = diagEnabled ? NowNs() : 0;
    DistributeFrozenSpansToConsumers(frozenSpans);
    if (diagEnabled && dueToGrow) {
        diagnostics_.rotateDistributeNs.fetch_add(NowNs() - distributeStartNs, std::memory_order_relaxed);
        diagnostics_.rotateTotalNs.fetch_add(NowNs() - totalStartNs, std::memory_order_relaxed);
    }
    return true;
}

void FreeManager::DistributeFrozenSpansToConsumers(const std::vector<UnitSpan>& spans)
{
    if (consumerBank_.backend == nullptr || spans.empty()) {
        return;
    }

    std::lock_guard<std::mutex> bankLock(consumerBank_.mutex);
    for (const UnitSpan& span : spans) {
        InsertRegionsToBankLocked(consumerBank_, UnitToRegionIdx(span.idx), UnitCountToRegionCount(span.num));
    }
    consumerBank_.epoch = heapGrowCount_;
    RefreshBankSummaryLocked(consumerBank_);
}

    void FreeManager::ReportDiagnostics() const
    {
        if (!diagnostics_.enabled) {
        return;
        }

        const uint64_t producerPublishCalls = diagnostics_.producerPublishCalls.load(std::memory_order_relaxed);
        const uint64_t producerPublishUnits = diagnostics_.producerPublishUnits.load(std::memory_order_relaxed);
        const uint64_t producerPublishNs = diagnostics_.producerPublishNs.load(std::memory_order_relaxed);
        const uint64_t consumerPublishCalls = diagnostics_.consumerPublishCalls.load(std::memory_order_relaxed);
        const uint64_t consumerPublishUnits = diagnostics_.consumerPublishUnits.load(std::memory_order_relaxed);
        const uint64_t consumerPublishNs = diagnostics_.consumerPublishNs.load(std::memory_order_relaxed);
        const uint64_t rotateCalls = diagnostics_.rotateCalls.load(std::memory_order_relaxed);
        const uint64_t rotatePayloadCalls = diagnostics_.rotatePayloadCalls.load(std::memory_order_relaxed);
        const uint64_t rotateMovedSpans = diagnostics_.rotateMovedSpans.load(std::memory_order_relaxed);
        const uint64_t rotateMovedUnits = diagnostics_.rotateMovedUnits.load(std::memory_order_relaxed);
        const uint64_t rotateCollectNs = diagnostics_.rotateCollectNs.load(std::memory_order_relaxed);
        const uint64_t rotateDistributeNs = diagnostics_.rotateDistributeNs.load(std::memory_order_relaxed);
        const uint64_t rotateTotalNs = diagnostics_.rotateTotalNs.load(std::memory_order_relaxed);
        const uint64_t takeCalls = diagnostics_.takeCalls.load(std::memory_order_relaxed);
        const uint64_t takeRequestedUnits = diagnostics_.takeRequestedUnits.load(std::memory_order_relaxed);
        const uint64_t takeVisitedBanks = diagnostics_.takeVisitedBanks.load(std::memory_order_relaxed);
        const uint64_t takeSummarySkipBanks = diagnostics_.takeSummarySkipBanks.load(std::memory_order_relaxed);
        const uint64_t takeLockBusyBanks = diagnostics_.takeLockBusyBanks.load(std::memory_order_relaxed);
        const uint64_t takeBackendMissBanks = diagnostics_.takeBackendMissBanks.load(std::memory_order_relaxed);
        const uint64_t takeSuccessCalls = diagnostics_.takeSuccessCalls.load(std::memory_order_relaxed);
        const uint64_t takeBusyReturns = diagnostics_.takeBusyReturns.load(std::memory_order_relaxed);
        const uint64_t takeBusyFallbacks = diagnostics_.takeBusyFallbacks.load(std::memory_order_relaxed);
        const uint64_t takeEmptyReturns = diagnostics_.takeEmptyReturns.load(std::memory_order_relaxed);
        const uint64_t takeEmptyWithProducerUnits = diagnostics_.takeEmptyWithProducerUnits.load(std::memory_order_relaxed);
        const uint64_t takeEmptyWithConsumerUnits = diagnostics_.takeEmptyWithConsumerUnits.load(std::memory_order_relaxed);
        const uint64_t takePublishOnMissAttempts = diagnostics_.takePublishOnMissAttempts.load(std::memory_order_relaxed);
        const uint64_t takePublishOnMissSuccess = diagnostics_.takePublishOnMissSuccess.load(std::memory_order_relaxed);
        const uint64_t takeTotalNs = diagnostics_.takeTotalNs.load(std::memory_order_relaxed);
        const uint64_t growCalls = diagnostics_.growCalls.load(std::memory_order_relaxed);
        const uint64_t growSuccessCalls = diagnostics_.growSuccessCalls.load(std::memory_order_relaxed);
        const uint64_t growFailAtMaxHeap = diagnostics_.growFailAtMaxHeap.load(std::memory_order_relaxed);
        const uint64_t growBytes = diagnostics_.growBytes.load(std::memory_order_relaxed);
        const uint64_t growTotalNs = diagnostics_.growTotalNs.load(std::memory_order_relaxed);

        fprintf(stderr,
            "[RELEASED-DIAG] take calls=%llu success=%llu busy=%llu empty=%llu requested_units=%llu visited_banks=%llu total_ms=%.3f avg_us=%.3f\n",
            static_cast<unsigned long long>(takeCalls),
            static_cast<unsigned long long>(takeSuccessCalls),
            static_cast<unsigned long long>(takeBusyReturns),
            static_cast<unsigned long long>(takeEmptyReturns),
            static_cast<unsigned long long>(takeRequestedUnits),
            static_cast<unsigned long long>(takeVisitedBanks),
            NsToMs(takeTotalNs), AverageUs(takeTotalNs, takeCalls));
        fprintf(stderr,
                "[RELEASED-DIAG] take summary_skip=%llu lock_busy=%llu backend_miss=%llu busy_fallbacks=%llu empty_with_producer=%llu empty_with_consumer=%llu publish_on_miss=%llu/%llu consumer_shards=%d\n",
            static_cast<unsigned long long>(takeSummarySkipBanks),
            static_cast<unsigned long long>(takeLockBusyBanks),
            static_cast<unsigned long long>(takeBackendMissBanks),
                static_cast<unsigned long long>(takeBusyFallbacks),
            static_cast<unsigned long long>(takeEmptyWithProducerUnits),
            static_cast<unsigned long long>(takeEmptyWithConsumerUnits),
                static_cast<unsigned long long>(takePublishOnMissSuccess),
                static_cast<unsigned long long>(takePublishOnMissAttempts),
            1);
        fprintf(stderr,
            "[RELEASED-DIAG] publish producer_calls=%llu producer_units=%llu producer_ms=%.3f producer_avg_us=%.3f consumer_calls=%llu consumer_units=%llu consumer_ms=%.3f consumer_avg_us=%.3f\n",
            static_cast<unsigned long long>(producerPublishCalls),
            static_cast<unsigned long long>(producerPublishUnits),
            NsToMs(producerPublishNs), AverageUs(producerPublishNs, producerPublishCalls),
            static_cast<unsigned long long>(consumerPublishCalls),
            static_cast<unsigned long long>(consumerPublishUnits),
            NsToMs(consumerPublishNs), AverageUs(consumerPublishNs, consumerPublishCalls));
        fprintf(stderr,
            "[RELEASED-DIAG] rotate calls=%llu payload_calls=%llu moved_spans=%llu moved_units=%llu collect_ms=%.3f distribute_ms=%.3f total_ms=%.3f avg_us=%.3f\n",
            static_cast<unsigned long long>(rotateCalls),
            static_cast<unsigned long long>(rotatePayloadCalls),
            static_cast<unsigned long long>(rotateMovedSpans),
            static_cast<unsigned long long>(rotateMovedUnits),
            NsToMs(rotateCollectNs), NsToMs(rotateDistributeNs), NsToMs(rotateTotalNs),
            AverageUs(rotateTotalNs, rotateCalls));
        fprintf(stderr,
            "[RELEASED-DIAG] grow calls=%llu success=%llu fail_at_max=%llu bytes=%llu total_ms=%.3f avg_us=%.3f heap_grow_count=%llu\n",
            static_cast<unsigned long long>(growCalls),
            static_cast<unsigned long long>(growSuccessCalls),
            static_cast<unsigned long long>(growFailAtMaxHeap),
            static_cast<unsigned long long>(growBytes),
            NsToMs(growTotalNs), AverageUs(growTotalNs, growCalls),
            static_cast<unsigned long long>(heapGrowCount_));
    }

bool FreeManager::TakeTrailingRegions(RegionIndex targetEnd, RegionIndex& idx, RegionCount& num,
                                      ReleaseBank** sourceBank)
{
    if (TakeTrailingRegionsFromBank(producerBank_, targetEnd, idx, num)) {
        if (sourceBank != nullptr) {
            *sourceBank = &producerBank_;
        }
        return true;
    }

    if (targetEnd == 0 || consumerBank_.backend == nullptr) {
        return false;
    }

    ReleaseBank& bank = consumerBank_;
    if (!TakeTrailingRegionsFromBank(bank, targetEnd, idx, num)) {
        return false;
    }
    if (sourceBank != nullptr) {
        *sourceBank = &bank;
    }
    return true;
}

bool FreeManager::TakeTrailingRegionsFromBank(ReleaseBank& bank, RegionIndex targetEnd,
                                              RegionIndex& idx, RegionCount& num)
{
    std::lock_guard<std::mutex> lg(bank.mutex);

    UnitIndex unitIdx = 0;
    UnitCount unitCount = 0;
    if (!bank.backend->TakeTrailingUnits(RegionToUnitIdx(targetEnd), unitIdx, unitCount)) {
        return false;
    }

    RefreshBankSummaryLocked(bank);
    idx = UnitToRegionIdx(unitIdx);
    num = UnitCountToRegionCount(unitCount);
    return true;
}

size_t FreeManager::RewindInactiveZoneUsingReleasedTail()
{
    size_t reclaimedRegions = 0;
    while (true) {
        uintptr_t tail = inactiveZone_.load(std::memory_order_relaxed);
        if (tail <= heapStart_) {
            break;
        }

        RegionIndex targetEnd = AddrToRegionIdx(tail);
        RegionIndex idx = 0;
        RegionCount num = 0;
        ReleaseBank* sourceBank = nullptr;
        if (!TakeTrailingRegions(targetEnd, idx, num, &sourceBank)) {
            break;
        }

        uintptr_t newTail = RegionIdxToAddr(idx);
        if (!inactiveZone_.compare_exchange_strong(tail, newTail, std::memory_order_relaxed)) {
            ScopedEnterSaferegion enterSaferegion(true);
            if (sourceBank != nullptr) {
                InsertRegionsToBank(*sourceBank, idx, num);
            }
            break;
        }
        reclaimedRegions += num;
    }
    return reclaimedRegions;
}

void FreeManager::ShrinkHeap()
{
    std::lock_guard<std::mutex> lg(growMutex_);

    RewindInactiveZoneUsingReleasedTail();

    uintptr_t iz = inactiveZone_.load(std::memory_order_relaxed);
    if (committedEnd_ <= iz) {
        return;
    }

    size_t shrinkGap = committedEnd_ - iz;
    size_t shrinkSegments = shrinkGap / segmentSize_;
    if (shrinkSegments == 0) {
        return;
    }

    size_t shrinkBytes = shrinkSegments * segmentSize_;
    uintptr_t newCommitted = committedEnd_ - shrinkBytes;

    if (newCommitted < heapStart_ + segmentSize_) {
        newCommitted = heapStart_ + segmentSize_;
        if (newCommitted >= committedEnd_) {
            return;
        }
    }

    committedEnd_ = newCommitted;
    committedRegionCount_ = (newCommitted - heapStart_) / GetRegionSizeBytes();
    committedUnitCount_ = (newCommitted - heapStart_) / unitSize_;
}

FreeManager::UnitCount FreeManager::GetReleasedUnitCount() const
{
    return static_cast<UnitCount>(producerBank_.totalUnits.load(std::memory_order_relaxed) +
        consumerBank_.totalUnits.load(std::memory_order_relaxed));
}

FreeManager::UnitCount FreeManager::GetFreeUnitCount() const
{
    CheckAlignmentConfig();
    UnitCount releasedUnits = GetReleasedUnitCount();
    UnitCount inactiveUnits = 0;
    uintptr_t inactiveZone = inactiveZone_.load(std::memory_order_relaxed);
    if (committedEnd_ > inactiveZone) {
        inactiveUnits = static_cast<UnitCount>((committedEnd_ - inactiveZone) / unitSize_);
    }
    return releasedUnits + inactiveUnits;
}

size_t FreeManager::GetReleasedBytes() const
{
    return static_cast<size_t>(GetReleasedUnitCount()) * unitSize_;
}

uint32_t FreeManager::GetNodeCount() const
{
    uint32_t nodeCount = 0;
    {
        std::lock_guard<std::mutex> lg(producerBank_.mutex);
        nodeCount += producerBank_.backend->GetNodeCount();
    }
    {
        std::lock_guard<std::mutex> lg(consumerBank_.mutex);
        nodeCount += consumerBank_.backend->GetNodeCount();
    }
    return nodeCount;
}

uint32_t FreeManager::GetMaxBlockRegions() const
{
    UnitCount maxBlock = producerBank_.maxBlockUnits.load(std::memory_order_relaxed);
    maxBlock = std::max(maxBlock, consumerBank_.maxBlockUnits.load(std::memory_order_relaxed));
    return UnitCountToRegionCount(maxBlock);
}

size_t FreeManager::GetMetadataBytesInUse() const
{
    size_t bytes = 0;
    {
        std::lock_guard<std::mutex> lg(producerBank_.mutex);
        bytes += producerBank_.backend->GetMetadataBytesInUse();
    }
    {
        std::lock_guard<std::mutex> lg(consumerBank_.mutex);
        bytes += consumerBank_.backend->GetMetadataBytesInUse();
    }
    return bytes;
}

size_t FreeManager::GetNodeBytesInUse() const
{
    size_t bytes = 0;
    {
        std::lock_guard<std::mutex> lg(producerBank_.mutex);
        bytes += producerBank_.backend->GetNodeBytesInUse();
    }
    {
        std::lock_guard<std::mutex> lg(consumerBank_.mutex);
        bytes += consumerBank_.backend->GetNodeBytesInUse();
    }
    return bytes;
}

void FreeManager::CollectRegionSpans(std::vector<RegionSpan>& spans) const
{
    std::vector<UnitSpan> unitSpans;
    {
        std::lock_guard<std::mutex> lg(producerBank_.mutex);
        producerBank_.backend->CollectUnitSpans(unitSpans);
    }

    std::vector<UnitSpan> consumerSpans;
    {
        std::lock_guard<std::mutex> lg(consumerBank_.mutex);
        consumerBank_.backend->CollectUnitSpans(consumerSpans);
    }
    unitSpans.insert(unitSpans.end(), consumerSpans.begin(), consumerSpans.end());

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

void FreeManager::CheckAlignmentConfig() const
{
    CHECK_DETAIL(unitSize_ != 0, "[MS-GC] FreeManager unitSize must not be zero");
    CHECK_DETAIL(unitsPerRegion_ != 0, "[MS-GC] FreeManager unitsPerRegion must not be zero");
}

size_t FreeManager::GetRegionSizeBytes() const
{
    CheckAlignmentConfig();
    return unitSize_ * unitsPerRegion_;
}

FreeManager::RegionIndex FreeManager::AddrToRegionIdx(uintptr_t addr) const
{
    CHECK_DETAIL(addr >= heapStart_, "[MS-GC] address is below heap start: addr=0x%zx, heapStart=0x%zx",
                 addr, heapStart_);
    uintptr_t delta = addr - heapStart_;
    size_t regionSize = GetRegionSizeBytes();
    CHECK_DETAIL(delta % regionSize == 0,
                 "[MS-GC] address is not region aligned: addr=0x%zx, heapStart=0x%zx, regionSize=%zu",
                 addr, heapStart_, regionSize);
    return static_cast<RegionIndex>(delta / regionSize);
}

uintptr_t FreeManager::RegionIdxToAddr(RegionIndex regionIdx) const
{
    return heapStart_ + static_cast<uintptr_t>(regionIdx) * GetRegionSizeBytes();
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
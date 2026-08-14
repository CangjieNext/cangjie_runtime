// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MCollector/MSMarkScheduler.h"

#include <atomic>
#include <cstdlib>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace MapleRuntime {

namespace {

template<typename T>
void* AllocateMSCacheLineAlignedArray(size_t size, const char* description)
{
    void* storage = nullptr;
#ifdef _WIN32
    storage = _aligned_malloc(size, alignof(T));
#else
    int result = posix_memalign(&storage, alignof(T), size);
    CHECK_DETAIL(result == 0,
                 "[MS-GC] aligned %s allocation failed: %d",
                 description, result);
#endif
    CHECK_DETAIL(storage != nullptr,
                 "[MS-GC] aligned %s allocation returned null", description);
    return storage;
}

template<typename T>
void FreeMSCacheLineAlignedArray(void* ptr) noexcept
{
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

} // namespace

static_assert(ATOMIC_INT_LOCK_FREE == 2,
              "MSGC requires always-lock-free 32-bit atomics");
static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
              "MSGC requires always-lock-free 64-bit atomics");
static_assert(sizeof(uintptr_t) == 8, "MSGC Step62 supports only 64-bit targets");

void* MSMarkReadyStack::operator new[](size_t size)
{
    return AllocateMSCacheLineAlignedArray<MSMarkReadyStack>(
        size, "Region ready shard");
}

void MSMarkReadyStack::operator delete[](void* ptr) noexcept
{
    FreeMSCacheLineAlignedArray<MSMarkReadyStack>(ptr);
}

void* MSMarkWorkHintShard::operator new[](size_t size)
{
    return AllocateMSCacheLineAlignedArray<MSMarkWorkHintShard>(
        size, "Mark work-hint shard");
}

void MSMarkWorkHintShard::operator delete[](void* ptr) noexcept
{
    FreeMSCacheLineAlignedArray<MSMarkWorkHintShard>(ptr);
}

bool MSMarkReadyStack::Push(MSRegionInfo* region)
{
    CHECK_DETAIL(region != nullptr, "[MS-GC] ready push with null Region");
    CHECK_DETAIL(
        region->GetMarkTaskNextRegionIdx() == MSRegionInfo::INVALID_REGION_IDX,
        "[MS-GC] ready push of linked Region idx=%zu next=%u",
        MSRegionInfoArray::GetRegionIdx(region),
        region->GetMarkTaskNextRegionIdx());

    int32_t idx = static_cast<int32_t>(MSRegionInfoArray::GetRegionIdx(region));
    uint64_t oldValue = taggedHead_.load(std::memory_order_acquire);
    uint64_t newValue;
    do {
        region->SetMarkTaskNextRegionIdx(
            static_cast<uint32_t>(IdxOf(oldValue)));
        newValue = Pack(idx, TagOf(oldValue) + 1);
    } while (!taggedHead_.compare_exchange_weak(
        oldValue, newValue,
        std::memory_order_release, std::memory_order_acquire));
    return IdxOf(oldValue) == MSRegionInfo::INVALID_IDX;
}

MSRegionInfo* MSMarkReadyStack::Pop()
{
    uint64_t oldValue = taggedHead_.load(std::memory_order_acquire);
    MSRegionInfo* region;
    uint64_t newValue;
    do {
        int32_t idx = IdxOf(oldValue);
        if (idx == MSRegionInfo::INVALID_IDX) {
            return nullptr;
        }
        region = MSRegionInfoArray::GetRegionInfoByIdx(idx);
        newValue = Pack(
            static_cast<int32_t>(region->GetMarkTaskNextRegionIdx()),
            TagOf(oldValue) + 1);
    } while (!taggedHead_.compare_exchange_weak(
        oldValue, newValue,
        std::memory_order_acq_rel, std::memory_order_acquire));

    region->SetMarkTaskNextRegionIdx(MSRegionInfo::INVALID_REGION_IDX);
    return region;
}

bool MSMarkReadyStack::IsEmpty() const
{
    return IdxOf(taggedHead_.load(std::memory_order_acquire)) ==
        MSRegionInfo::INVALID_IDX;
}

void MSMarkReadyStack::ResetQuiescent()
{
    uint64_t oldValue = taggedHead_.load(std::memory_order_acquire);
    CHECK_DETAIL(
        IdxOf(oldValue) == MSRegionInfo::INVALID_IDX,
        "[MS-GC] reset non-empty ready stack: head=%d",
        IdxOf(oldValue));
    taggedHead_.store(
        Pack(MSRegionInfo::INVALID_IDX, TagOf(oldValue) + 1),
        std::memory_order_relaxed);
}

MSMarkScheduler::MSMarkScheduler()
    : regionReadyShards_(new MSMarkReadyStack[1]),
      workHintShards_(new MSMarkWorkHintShard[1])
{
}

void MSMarkScheduler::ConfigureRegionShards(size_t shardCount)
{
    shardCount = std::max<size_t>(1, shardCount);
    CHECK_DETAIL(
        ledger_.LoadForTermination() == 0 && ReadyEmptyForTermination(),
        "[MS-GC] configure Region ready shards while scheduler is active");
    if (shardCount == regionReadyShardCount_) {
        return;
    }
    std::unique_ptr<MSMarkReadyStack[]> replacement(
        new MSMarkReadyStack[shardCount]);
    std::unique_ptr<MSMarkWorkHintShard[]> workHintReplacement(
        new MSMarkWorkHintShard[shardCount]);
    regionReadyShards_ = std::move(replacement);
    workHintShards_ = std::move(workHintReplacement);
    regionReadyShardCount_ = shardCount;
}

void MSMarkScheduler::ResetCycle()
{
    ledger_.ResetQuiescent();
    for (size_t i = 0; i < regionReadyShardCount_; ++i) {
        regionReadyShards_[i].ResetQuiescent();
        workHintShards_[i].ResetQuiescent();
    }
    objectReady_.ResetQuiescent();
}

uint64_t MSMarkScheduler::PublishRegionWords(
    MSRegionInfo* region, uint64_t wordMask)
{
    CHECK_DETAIL(region != nullptr,
                 "[MS-GC] pending Region work publication with null Region");
    uint64_t requestedWords = static_cast<uint64_t>(
        __builtin_popcountll(wordMask));
    MSMarkWorkHintShard& shard = WorkHintShard(region);
    shard.Add(requestedWords);
    uint64_t newWords = region->PublishPendingMarkWords(wordMask);
    shard.Sub(requestedWords - static_cast<uint64_t>(
        __builtin_popcountll(newWords)));
    return newWords;
}

void MSMarkScheduler::CompleteRegionWork(
    MSRegionInfo* region, uint64_t wordMask)
{
    CHECK_DETAIL(region != nullptr,
                 "[MS-GC] completing pending Region work with null Region");
    WorkHintShard(region).Sub(static_cast<uint64_t>(
        __builtin_popcountll(wordMask)));
}

void MSMarkScheduler::CompleteObjectWork(MSRegionInfo* largePrimary)
{
    CHECK_DETAIL(largePrimary != nullptr,
                 "[MS-GC] completing Object work with null Region");
    WorkHintShard(largePrimary).Sub(1);
}

uint64_t MSMarkScheduler::OutstandingWorkHint() const
{
    uint64_t total = 0;
    for (size_t i = 0; i < regionReadyShardCount_; ++i) {
        total = SaturatingMSWUAdd(total, workHintShards_[i].LoadHint());
    }
    return total;
}

bool MSMarkScheduler::TryPublishRegion(
    MSRegionInfo* region, size_t preferredRegionShard)
{
    CHECK_DETAIL(
        region != nullptr &&
        region->GetRegionKind() == MSRegionKind::SMALL &&
        region->GetScanClass() == MSScanClass::HAS_REF,
        "[MS-GC] invalid Region task publication");

    // A QUEUED/OWNED Region already has a ledger-protected task. Its owner
    // consumes pending work before releasing; the post-release recheck and the
    // MarkTermination reconciliation cover the residual producer race.
    if (region->GetScanState() != MSScanState::IDLE) {
        return false;
    }

    ledger_.AddProvisional();
    for (;;) {
        if (!region->CasScanState(MSScanState::IDLE, MSScanState::QUEUED)) {
            ledger_.CancelProvisional();
            return false;
        }
        if (region->LoadPendingMarkWordsAcquire() != 0) {
            return RegionReadyShard(preferredRegionShard).Push(region);
        }

        // A delayed producer can still win IDLE -> QUEUED after another owner
        // consumed its bit. Retract that empty admission; the second pending
        // check then retries publication for newly arrived work.
        CHECK_DETAIL(
            region->CasScanState(MSScanState::QUEUED, MSScanState::IDLE),
            "[MS-GC] failed to retract empty Region task idx=%zu state=%u",
            MSRegionInfoArray::GetRegionIdx(region),
            static_cast<unsigned>(region->GetScanState()));
        if (region->LoadPendingMarkWordsAcquire() == 0) {
            ledger_.CancelProvisional();
            return false;
        }
    }
}

bool MSMarkScheduler::PublishObject(MSRegionInfo* largePrimary)
{
    CHECK_DETAIL(
        largePrimary != nullptr &&
        largePrimary->GetRegionKind() == MSRegionKind::LARGE &&
        largePrimary->GetScanClass() == MSScanClass::HAS_REF &&
        largePrimary->IsMarked(),
                 "[MS-GC] invalid Object task publication");
    WorkHintShard(largePrimary).Add(1);
    ledger_.AddProvisional();
    return objectReady_.Push(largePrimary);
}

bool MSMarkScheduler::TryClaimFrom(
    MSMarkReadyStack& ready, MSMarkTaskKind kind, MSTaskLease& lease)
{
    MSRegionInfo* region = ready.Pop();
    if (region == nullptr) {
        return false;
    }
    if (kind == MSMarkTaskKind::REGION) {
        CHECK_DETAIL(
            region->CasScanState(MSScanState::QUEUED, MSScanState::OWNED),
            "[MS-GC] claimed Region task with state=%u idx=%zu",
            static_cast<unsigned>(region->GetScanState()),
            MSRegionInfoArray::GetRegionIdx(region));
    }
    lease = MSTaskLease(
        &ledger_, kind,
        static_cast<uint32_t>(MSRegionInfoArray::GetRegionIdx(region)));
    return true;
}

bool MSMarkScheduler::TryClaimRegion(
    size_t preferredRegionShard, size_t& regionProbeCursor,
    MSTaskLease& lease)
{
    size_t rotationStart = regionProbeCursor++;
    bool fullFallback = ledger_.LoadHint() != 0;
    return TryMSMarkReadyShardProbe(
        regionReadyShardCount_, preferredRegionShard, rotationStart,
        fullFallback,
        [this, &lease](size_t shard) {
            return TryClaimFrom(
                regionReadyShards_[shard], MSMarkTaskKind::REGION, lease);
        });
}

bool MSMarkScheduler::TryClaimAssistRegion(
    size_t preferredRegionShard, size_t& regionProbeCursor,
    MSTaskLease& lease)
{
    return TryMSMarkReadyAssistShardProbe(
        regionReadyShardCount_, preferredRegionShard, regionProbeCursor,
        [this, &lease](size_t shard) {
            return TryClaimFrom(
                regionReadyShards_[shard], MSMarkTaskKind::REGION, lease);
        });
}

bool MSMarkScheduler::TryClaim(
    bool preferObject,
    size_t preferredRegionShard, size_t& regionProbeCursor,
    MSTaskLease& lease)
{
    CHECK_DETAIL(!lease.IsActive(),
                 "[MS-GC] TryClaim target already holds active lease");
    if (preferObject &&
        TryClaimFrom(objectReady_, MSMarkTaskKind::OBJECT, lease)) {
        return true;
    }
    if (TryClaimRegion(preferredRegionShard, regionProbeCursor, lease)) {
        return true;
    }
    return !preferObject &&
        TryClaimFrom(objectReady_, MSMarkTaskKind::OBJECT, lease);
}

bool MSMarkScheduler::TryClaimAssist(
    size_t preferredRegionShard, size_t& regionProbeCursor,
    MSTaskLease& lease)
{
    CHECK_DETAIL(!lease.IsActive(),
                 "[MS-GC] TryClaimAssist target already holds active lease");
    return TryClaimAssistRegion(
        preferredRegionShard, regionProbeCursor, lease);
}

bool MSMarkScheduler::ReadyEmptyForTermination() const
{
    if (!objectReady_.IsEmpty()) {
        return false;
    }
    for (size_t i = 0; i < regionReadyShardCount_; ++i) {
        if (!regionReadyShards_[i].IsEmpty()) {
            return false;
        }
    }
    return true;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

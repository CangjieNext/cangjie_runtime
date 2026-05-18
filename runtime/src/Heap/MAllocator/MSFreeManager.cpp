// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSFreeManager.h"

#include <algorithm>
#include <cstring>

#ifndef _WIN64
#include <sys/mman.h>
#else
#include <windows.h>
#endif

#include "Base/Log.h"
#include "Base/MemUtils.h"

namespace MapleRuntime {

#if defined(_WIN64)
namespace {
size_t GetOsPageSize()
{
    static size_t pageSize = []() {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return static_cast<size_t>(info.dwPageSize);
    }();
    return pageSize;
}

void CommitReservedPrefix(void* base, size_t bytes)
{
    if (base == nullptr || bytes == 0) {
        return;
    }
    size_t pageSize = GetOsPageSize();
    size_t commitBytes = ((bytes + pageSize - 1) / pageSize) * pageSize;
    void* ret = VirtualAlloc(base, commitBytes, MEM_COMMIT, PAGE_READWRITE);
    CHECK_DETAIL(ret != nullptr, "VirtualAlloc commit failed for FreeManager metadata");
}
}
#endif

FreeManager::~FreeManager()
{
    ReleaseMetadataStorage();
}

void FreeManager::ReserveMetadataStorage(size_t bitmapWordCapacity, size_t summaryChunkCapacity)
{
    ReleaseMetadataStorage();

    bitmapWordCapacity_ = bitmapWordCapacity;
    summaryChunkCapacity_ = summaryChunkCapacity;
    bitmapWordCountInUse_ = 0;
    summaryChunkCountInUse_ = 0;

    size_t bitmapBytes = bitmapWordCapacity_ * sizeof(uint64_t);
    size_t summaryBytes = summaryChunkCapacity_ * sizeof(ChunkSummary);

#if defined(_WIN64)
    allocBits_ = static_cast<uint64_t*>(VirtualAlloc(nullptr, bitmapBytes, MEM_RESERVE, PAGE_READWRITE));
    scavengedBits_ = static_cast<uint64_t*>(VirtualAlloc(nullptr, bitmapBytes, MEM_RESERVE, PAGE_READWRITE));
    chunkSummaries_ = static_cast<ChunkSummary*>(VirtualAlloc(nullptr, summaryBytes, MEM_RESERVE, PAGE_READWRITE));
    CHECK_DETAIL(allocBits_ != nullptr, "VirtualAlloc reserve failed for allocBits");
    CHECK_DETAIL(scavengedBits_ != nullptr, "VirtualAlloc reserve failed for scavengedBits");
    CHECK_DETAIL(chunkSummaries_ != nullptr, "VirtualAlloc reserve failed for chunkSummaries");
#else
    allocBits_ = static_cast<uint64_t*>(mmap(nullptr, bitmapBytes, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    scavengedBits_ = static_cast<uint64_t*>(mmap(nullptr, bitmapBytes, PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    chunkSummaries_ = static_cast<ChunkSummary*>(mmap(nullptr, summaryBytes, PROT_READ | PROT_WRITE,
                                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK_DETAIL(allocBits_ != MAP_FAILED, "mmap reserve failed for allocBits");
    CHECK_DETAIL(scavengedBits_ != MAP_FAILED, "mmap reserve failed for scavengedBits");
    CHECK_DETAIL(chunkSummaries_ != MAP_FAILED, "mmap reserve failed for chunkSummaries");
#endif
}

void FreeManager::ReleaseMetadataStorage()
{
#if defined(_WIN64)
    if (allocBits_ != nullptr) {
        (void)VirtualFree(allocBits_, 0, MEM_RELEASE);
    }
    if (scavengedBits_ != nullptr) {
        (void)VirtualFree(scavengedBits_, 0, MEM_RELEASE);
    }
    if (chunkSummaries_ != nullptr) {
        (void)VirtualFree(chunkSummaries_, 0, MEM_RELEASE);
    }
#else
    if (allocBits_ != nullptr) {
        (void)munmap(allocBits_, bitmapWordCapacity_ * sizeof(uint64_t));
    }
    if (scavengedBits_ != nullptr) {
        (void)munmap(scavengedBits_, bitmapWordCapacity_ * sizeof(uint64_t));
    }
    if (chunkSummaries_ != nullptr) {
        (void)munmap(chunkSummaries_, summaryChunkCapacity_ * sizeof(ChunkSummary));
    }
#endif
    allocBits_ = nullptr;
    scavengedBits_ = nullptr;
    chunkSummaries_ = nullptr;
    bitmapWordCapacity_ = 0;
    bitmapWordCountInUse_ = 0;
    summaryChunkCapacity_ = 0;
    summaryChunkCountInUse_ = 0;
}

void FreeManager::EnsureMetadataInitializedForRegionCount(size_t regionCount)
{
    size_t targetWordCount = RegionCountToChunkCount(regionCount);
    size_t targetChunkCount = RegionCountToChunkCount(regionCount);

    CHECK_DETAIL(targetWordCount <= bitmapWordCapacity_,
                 "[MS-GC] metadata word capacity exceeded: target=%zu capacity=%zu",
                 targetWordCount, bitmapWordCapacity_);
    CHECK_DETAIL(targetChunkCount <= summaryChunkCapacity_,
                 "[MS-GC] metadata chunk capacity exceeded: target=%zu capacity=%zu",
                 targetChunkCount, summaryChunkCapacity_);

#if defined(_WIN64)
    if (targetWordCount > bitmapWordCountInUse_) {
        CommitReservedPrefix(allocBits_, targetWordCount * sizeof(uint64_t));
        CommitReservedPrefix(scavengedBits_, targetWordCount * sizeof(uint64_t));
    }
    if (targetChunkCount > summaryChunkCountInUse_) {
        CommitReservedPrefix(chunkSummaries_, targetChunkCount * sizeof(ChunkSummary));
    }
#endif

    for (size_t wordIdx = bitmapWordCountInUse_; wordIdx < targetWordCount; ++wordIdx) {
        allocBits_[wordIdx] = ~0ULL;
        scavengedBits_[wordIdx] = 0;
    }
    for (size_t chunkIdx = summaryChunkCountInUse_; chunkIdx < targetChunkCount; ++chunkIdx) {
        chunkSummaries_[chunkIdx] = {};
    }

    bitmapWordCountInUse_ = targetWordCount;
    summaryChunkCountInUse_ = targetChunkCount;
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
    heapGrowCount_ = 0;
    freeRegionCount_ = committedRegionCount_;
    scavengedRegionCount_ = 0;
    searchIdx_ = 0;

    size_t wordCount = RegionCountToChunkCount(maxVirtualRegionCount_);
    ReserveMetadataStorage(wordCount, wordCount);
    EnsureMetadataInitializedForRegionCount(committedRegionCount_);

    for (size_t regionIdx = 0; regionIdx < committedRegionCount_; ++regionIdx) {
        ClearBit(allocBits_, regionIdx);
    }
    for (size_t chunkIdx = 0; chunkIdx < summaryChunkCountInUse_; ++chunkIdx) {
        UpdateChunkSummaryLocked(chunkIdx);
    }
}

FreeManager::TryTakeResult FreeManager::TryTakeRegions(RegionCount num, RegionIndex& idx)
{
    if (num == 0) {
        return TryTakeResult::EMPTY;
    }

    bool reusedScavenged = false;
    {
        std::lock_guard<std::mutex> lg(pageHeapMutex_);
        if (!FindFreeRunLocked(num, idx)) {
            return TryTakeResult::EMPTY;
        }

        MarkAllocatedLocked(idx, num, reusedScavenged);
        if (idx <= searchIdx_) {
            searchIdx_ = FindNextFreeRegionLocked(idx + num);
        }
    }

    if (reusedScavenged) {
        ReuseUnits(RegionToUnitIdx(idx), RegionCountToUnitCount(num));
    }
    return TryTakeResult::SUCCESS;
}

bool FreeManager::TryIncreaseInactiveZone(RegionCount num, RegionIndex& idx)
{
    (void)num;
    (void)idx;
    return false;
}

void FreeManager::ReclaimRegionsReleased(RegionIndex idx, RegionCount num)
{
    if (num == 0) {
        return;
    }

    std::lock_guard<std::mutex> lg(pageHeapMutex_);
    MarkFreeLocked(idx, num);
    searchIdx_ = std::min(searchIdx_, idx);
}

bool FreeManager::GrowOneSegment(HeapRangeHandler& handler)
{
    std::lock_guard<std::mutex> growLock(growMutex_);
    if (committedEnd_ >= maxHeapEnd_) {
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
    size_t newCommittedRegionCount = committedRegionCount_ + static_cast<size_t>(growRegions);

    handler.InitializeFreshRegions(oldRegionCount, growRegions);

    {
        std::lock_guard<std::mutex> pageLock(pageHeapMutex_);
        EnsureMetadataInitializedForRegionCount(newCommittedRegionCount);
        committedRegionCount_ = newCommittedRegionCount;
        committedUnitCount_ += static_cast<size_t>(growUnits);
        committedEnd_ = oldCommittedEnd + growBytes;
        MarkFreeLocked(oldRegionCount, growRegions);
        searchIdx_ = std::min(searchIdx_, oldRegionCount);
    }

    handler.OnCommittedHeapExtended(committedEnd_);
    heapGrowCount_++;
    return true;
}

void FreeManager::ShrinkHeap()
{
    std::lock_guard<std::mutex> growLock(growMutex_);
    std::lock_guard<std::mutex> pageLock(pageHeapMutex_);

    size_t trailingFree = CountTrailingFreeRegionsLocked();
    if (trailingFree == 0) {
        return;
    }

    size_t trailingBytes = trailingFree * GetRegionSizeBytes();
    size_t shrinkSegments = trailingBytes / segmentSize_;
    if (shrinkSegments == 0) {
        return;
    }

    size_t shrinkBytes = shrinkSegments * segmentSize_;
    size_t shrinkRegions = shrinkBytes / GetRegionSizeBytes();
    size_t minCommittedRegions = segmentSize_ / GetRegionSizeBytes();
    if (committedRegionCount_ <= minCommittedRegions) {
        return;
    }
    if (shrinkRegions > committedRegionCount_ - minCommittedRegions) {
        shrinkRegions = committedRegionCount_ - minCommittedRegions;
        shrinkBytes = shrinkRegions * GetRegionSizeBytes();
    }
    if (shrinkRegions == 0) {
        return;
    }

    RegionIndex shrinkStart = static_cast<RegionIndex>(committedRegionCount_ - shrinkRegions);
    ReleaseUnits(RegionToUnitIdx(shrinkStart), RegionCountToUnitCount(static_cast<RegionCount>(shrinkRegions)));
    MarkUnavailableLocked(shrinkStart, static_cast<RegionCount>(shrinkRegions));

    committedRegionCount_ -= shrinkRegions;
    committedUnitCount_ = committedRegionCount_ * unitsPerRegion_;
    committedEnd_ -= shrinkBytes;
    bitmapWordCountInUse_ = RegionCountToChunkCount(committedRegionCount_);
    summaryChunkCountInUse_ = RegionCountToChunkCount(committedRegionCount_);
    searchIdx_ = std::min(searchIdx_, static_cast<RegionIndex>(committedRegionCount_));
}

size_t FreeManager::ScavengeFreeRegions(bool releaseAll)
{
    std::lock_guard<std::mutex> pageLock(pageHeapMutex_);
    if (freeRegionCount_ == 0 || committedRegionCount_ == 0) {
        return 0;
    }

    size_t regionBudget = releaseAll ? freeRegionCount_ : std::max<size_t>(1, segmentSize_ / GetRegionSizeBytes());
    size_t scavengedRegions = 0;
    RegionIndex scan = static_cast<RegionIndex>(committedRegionCount_);

    while (scan > 0 && scavengedRegions < regionBudget) {
        RegionIndex end = scan;
        while (scan > 0) {
            RegionIndex idx = scan - 1;
            if (!IsRegionFreeLocked(idx) || IsRegionScavengedLocked(idx)) {
                break;
            }
            --scan;
        }
        if (scan == end) {
            --scan;
            continue;
        }

        RegionCount run = static_cast<RegionCount>(end - scan);
        if (!releaseAll && scavengedRegions + run > regionBudget) {
            RegionIndex partialStart = static_cast<RegionIndex>(end - (regionBudget - scavengedRegions));
            run = static_cast<RegionCount>(regionBudget - scavengedRegions);
            scan = partialStart;
        }

        ReleaseUnits(RegionToUnitIdx(scan), RegionCountToUnitCount(run));
        MarkScavengedLocked(scan, run);
        scavengedRegions += run;
    }

    return scavengedRegions * GetRegionSizeBytes();
}

FreeManager::UnitCount FreeManager::GetReleasedUnitCount() const
{
    std::lock_guard<std::mutex> lg(pageHeapMutex_);
    return static_cast<UnitCount>(scavengedRegionCount_ * unitsPerRegion_);
}

FreeManager::UnitCount FreeManager::GetFreeUnitCount() const
{
    std::lock_guard<std::mutex> lg(pageHeapMutex_);
    return static_cast<UnitCount>(freeRegionCount_ * unitsPerRegion_);
}

size_t FreeManager::GetReleasedBytes() const
{
    std::lock_guard<std::mutex> lg(pageHeapMutex_);
    return scavengedRegionCount_ * GetRegionSizeBytes();
}

uint32_t FreeManager::GetMaxBlockRegions() const
{
    std::lock_guard<std::mutex> lg(pageHeapMutex_);
    uint32_t maxRun = 0;
    for (size_t chunkIdx = 0; chunkIdx < summaryChunkCountInUse_; ++chunkIdx) {
        maxRun = std::max<uint32_t>(maxRun, chunkSummaries_[chunkIdx].max);
    }
    return maxRun;
}

size_t FreeManager::GetMetadataBytesInUse() const
{
    std::lock_guard<std::mutex> lg(pageHeapMutex_);
    return bitmapWordCountInUse_ * sizeof(uint64_t) * 2 +
           summaryChunkCountInUse_ * sizeof(ChunkSummary);
}

void FreeManager::CollectRegionSpans(std::vector<RegionSpan>& spans) const
{
    std::lock_guard<std::mutex> lg(pageHeapMutex_);
    spans.clear();

    RegionIndex idx = 0;
    while (idx < committedRegionCount_) {
        idx = FindNextFreeRegionLocked(idx);
        if (idx >= committedRegionCount_) {
            break;
        }
        RegionIndex start = idx;
        while (idx < committedRegionCount_ && IsRegionFreeLocked(idx)) {
            ++idx;
        }
        spans.push_back({ start, static_cast<RegionCount>(idx - start) });
    }
}

bool FreeManager::IsRegionFreeLocked(RegionIndex idx) const
{
    return idx < committedRegionCount_ && !TestBit(allocBits_, idx);
}

bool FreeManager::IsRegionScavengedLocked(RegionIndex idx) const
{
    return idx < committedRegionCount_ && TestBit(scavengedBits_, idx);
}

FreeManager::RegionIndex FreeManager::FindNextFreeRegionLocked(RegionIndex start) const
{
    for (RegionIndex idx = start; idx < committedRegionCount_; ++idx) {
        if (IsRegionFreeLocked(idx)) {
            return idx;
        }
    }
    return static_cast<RegionIndex>(committedRegionCount_);
}

bool FreeManager::PreciseFitFromLocked(RegionIndex start, RegionCount num) const
{
    if (start + num > committedRegionCount_) {
        return false;
    }
    for (RegionIndex idx = start; idx < start + num; ++idx) {
        if (!IsRegionFreeLocked(idx)) {
            return false;
        }
    }
    return true;
}

bool FreeManager::FindFreeRunLocked(RegionCount num, RegionIndex& idx) const
{
    if (num == 0 || num > committedRegionCount_ || freeRegionCount_ < num) {
        return false;
    }

    RegionIndex cursor = std::min(searchIdx_, static_cast<RegionIndex>(committedRegionCount_));
    RegionIndex carryStart = cursor;
    uint32_t carryLen = 0;
    size_t startChunk = cursor / kChunkBits;

    for (size_t chunkIdx = startChunk; chunkIdx < summaryChunkCountInUse_; ++chunkIdx) {
        const ChunkSummary& chunk = chunkSummaries_[chunkIdx];
        if (chunk.max == 0 && chunk.start == 0 && chunk.end == 0) {
            carryLen = 0;
            carryStart = static_cast<RegionIndex>((chunkIdx + 1) * kChunkBits);
            continue;
        }

        RegionIndex chunkStart = static_cast<RegionIndex>(chunkIdx * kChunkBits);
        RegionIndex candidate = std::max(cursor, chunkStart);
        if (chunk.max >= num) {
            RegionIndex chunkEnd = std::min<RegionIndex>(static_cast<RegionIndex>(committedRegionCount_),
                                                         static_cast<RegionIndex>(chunkStart + kChunkBits));
            for (RegionIndex probe = candidate; probe + num <= chunkEnd; ++probe) {
                if (PreciseFitFromLocked(probe, num)) {
                    idx = probe;
                    return true;
                }
            }
        }
        if (carryLen != 0 && static_cast<uint32_t>(carryLen + chunk.start) >= num &&
            PreciseFitFromLocked(carryStart, num)) {
            idx = carryStart;
            return true;
        }
        if (chunk.end == kChunkBits) {
            if (carryLen == 0) {
                carryStart = chunkStart;
            }
            carryLen += chunk.end;
        } else if (chunk.end != 0) {
            carryStart = static_cast<RegionIndex>(chunkStart + kChunkBits - chunk.end);
            carryLen = chunk.end;
        } else {
            carryLen = 0;
            carryStart = static_cast<RegionIndex>(chunkStart + kChunkBits);
        }
    }
    return false;
}

size_t FreeManager::CountScavengedInRangeLocked(RegionIndex idx, RegionCount num) const
{
    size_t count = 0;
    for (RegionIndex cur = idx; cur < idx + num; ++cur) {
        if (IsRegionScavengedLocked(cur)) {
            ++count;
        }
    }
    return count;
}

size_t FreeManager::CountFreeInRangeLocked(RegionIndex idx, RegionCount num) const
{
    size_t count = 0;
    for (RegionIndex cur = idx; cur < idx + num; ++cur) {
        if (IsRegionFreeLocked(cur)) {
            ++count;
        }
    }
    return count;
}

void FreeManager::MarkAllocatedLocked(RegionIndex idx, RegionCount num, bool& reusedScavenged)
{
    size_t scavengedCount = CountScavengedInRangeLocked(idx, num);
    reusedScavenged = scavengedCount != 0;
    scavengedRegionCount_ -= scavengedCount;
    freeRegionCount_ -= CountFreeInRangeLocked(idx, num);

    for (RegionIndex cur = idx; cur < idx + num; ++cur) {
        SetBit(allocBits_, cur);
        ClearBit(scavengedBits_, cur);
    }
    UpdateChunkSummaryRangeLocked(idx, num);
}

void FreeManager::MarkFreeLocked(RegionIndex idx, RegionCount num)
{
    for (RegionIndex cur = idx; cur < idx + num && cur < committedRegionCount_; ++cur) {
        if (TestBit(allocBits_, cur)) {
            ++freeRegionCount_;
        }
        if (TestBit(scavengedBits_, cur)) {
            ClearBit(scavengedBits_, cur);
            --scavengedRegionCount_;
        }
        ClearBit(allocBits_, cur);
    }
    UpdateChunkSummaryRangeLocked(idx, num);
}

void FreeManager::MarkScavengedLocked(RegionIndex idx, RegionCount num)
{
    for (RegionIndex cur = idx; cur < idx + num && cur < committedRegionCount_; ++cur) {
        if (IsRegionFreeLocked(cur) && !TestBit(scavengedBits_, cur)) {
            SetBit(scavengedBits_, cur);
            ++scavengedRegionCount_;
        }
    }
}

void FreeManager::MarkUnavailableLocked(RegionIndex idx, RegionCount num)
{
    scavengedRegionCount_ -= CountScavengedInRangeLocked(idx, num);
    freeRegionCount_ -= CountFreeInRangeLocked(idx, num);

    for (RegionIndex cur = idx; cur < idx + num; ++cur) {
        SetBit(allocBits_, cur);
        ClearBit(scavengedBits_, cur);
    }
    UpdateChunkSummaryRangeLocked(idx, num);
}

void FreeManager::UpdateChunkSummaryLocked(size_t chunkIdx)
{
    size_t chunkStart = chunkIdx * kChunkBits;
    size_t chunkEnd = std::min(committedRegionCount_, chunkStart + kChunkBits);

    ChunkSummary summary {};
    uint16_t prefix = 0;
    while (chunkStart + prefix < chunkEnd &&
           !TestBit(allocBits_, chunkStart + prefix)) {
        ++prefix;
    }
    summary.start = prefix;

    uint16_t suffix = 0;
    while (chunkEnd > chunkStart + suffix &&
           !TestBit(allocBits_, chunkEnd - suffix - 1)) {
        ++suffix;
    }
    summary.end = suffix;

    uint16_t current = 0;
    uint16_t best = 0;
    for (size_t regionIdx = chunkStart; regionIdx < chunkEnd; ++regionIdx) {
        if (!TestBit(allocBits_, regionIdx)) {
            ++current;
            best = std::max(best, current);
        } else {
            current = 0;
        }
    }
    summary.max = best;
    chunkSummaries_[chunkIdx] = summary;
}

void FreeManager::UpdateChunkSummaryRangeLocked(RegionIndex idx, RegionCount num)
{
    if (summaryChunkCountInUse_ == 0) {
        return;
    }
    size_t firstChunk = idx / kChunkBits;
    size_t lastChunk = (idx + std::max<RegionCount>(1, num) - 1) / kChunkBits;
    for (size_t chunkIdx = firstChunk; chunkIdx <= lastChunk && chunkIdx < summaryChunkCountInUse_; ++chunkIdx) {
        UpdateChunkSummaryLocked(chunkIdx);
    }
}

size_t FreeManager::CountTrailingFreeRegionsLocked() const
{
    size_t count = 0;
    RegionIndex idx = static_cast<RegionIndex>(committedRegionCount_);
    while (idx > 0 && IsRegionFreeLocked(idx - 1)) {
        --idx;
        ++count;
    }
    return count;
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

void FreeManager::ReuseUnits(UnitIndex idx, UnitCount num) const
{
#if defined(_WIN64)
    uintptr_t addr = heapStart_ + static_cast<uintptr_t>(idx) * unitSize_;
    size_t size = static_cast<size_t>(num) * unitSize_;
    void* ret = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_COMMIT, PAGE_READWRITE);
    CHECK_DETAIL(ret != nullptr, "VirtualAlloc failed in FreeManager::ReuseUnits");
#else
    (void)idx;
    (void)num;
#endif
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
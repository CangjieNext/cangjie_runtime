// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/RegionManager.h"

#include <algorithm>
#include <cmath>
#include <unistd.h>

#include "Allocator/LOSManager.h"
#include "Allocator/RegionSpace.h"
#include "Allocator/ReserveManager.h"
#include "Base/CString.h"
#include "Collector/Collector.h"
#include "Collector/CopyCollector.h"
#include "Collector/FinalizerProcessor.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"

namespace MapleRuntime {
uintptr_t RegionInfo::UnitInfo::totalUnitCount = 0;
uintptr_t RegionInfo::UnitInfo::heapStartAddress = 0;

static size_t GetPageSize() noexcept
{
#if defined(_WIN64)
    SYSTEM_INFO systeminfo;
    GetSystemInfo(&systeminfo);
    if (systeminfo.dwPageSize != 0) {
        return systeminfo.dwPageSize;
    } else {
        // default page size is 4KB if get system page size failed.
        return 4 * KB;
    }
#elif defined(__APPLE__)
    // default page size is 16KB in MacOS
    return 16 * KB;
#else
    return getpagesize();
#endif
}

// System default page size
const size_t MRT_PAGE_SIZE = GetPageSize();
const size_t AllocatorUtils::ALLOC_PAGE_SIZE = MapleRuntime::MRT_PAGE_SIZE;
// region unit size: same as system page size
const size_t RegionInfo::UNIT_SIZE = MapleRuntime::MRT_PAGE_SIZE;
// regarding a object as a large object when the size is greater than 32KB or one page size,
// depending on the system page size.
const size_t RegionInfo::LARGE_OBJECT_DEFAULT_THRESHOLD = MapleRuntime::MRT_PAGE_SIZE > (32 * KB) ?
                                                            MapleRuntime::MRT_PAGE_SIZE : 32 * KB;
// max size of per region is 128KB.
const size_t RegionManager::MAX_UNIT_COUNT_PER_REGION = (128 * KB) / MapleRuntime::MRT_PAGE_SIZE;
// size of huge page is 2048KB.
const size_t RegionManager::HUGE_PAGE = (2048 * KB) / MapleRuntime::MRT_PAGE_SIZE;;

class ForwardTask : public HeapWork {
public:
    ForwardTask(RegionManager& manager, RegionList& fromSpace)
        : regionManager(manager), fromRegionList(fromSpace) {}

    ~ForwardTask() = default;

    void Execute(size_t) override
    {
        while (true) {
            RegionInfo* region = fromRegionList.TakeHeadRegion(RegionInfo::RegionType::LONE_FROM_REGION);
            if (region == nullptr) { break; }
            regionManager.ForwardRegion(region);
        }
    }

private:
    RegionManager& regionManager;
    RegionList& fromRegionList;
};

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
void RegionInfo::DumpRegionInfo(LogType type) const
{
    DLOG(type, "Region index: %zu, type: %s, address: 0x%zx-0x%zx, allocated(B) %zu, live(B) %zu", GetUnitIdx(),
         GetTypeName(), GetRegionStart(), GetRegionEnd(), GetRegionAllocatedSize(), GetLiveByteCount());
}

const char* RegionInfo::GetTypeName() const
{
    static constexpr const char* regionNames[] = {
        "undefined region",
        "thread local region",
        "recent fullregion",
        "from region",
        "unmovable from region",
        "to region",
        "full pinned region",
        "recent pinned region",
        "raw pointer pinned region",
        "tl raw pointer region",
        "large region",
        "recent large region",
        "garbage region",
    };
    return regionNames[static_cast<uint8_t>(GetRegionType())];
}
#endif

void RegionInfo::VisitAllObjects(const std::function<void(BaseObject*)>&& func)
{
    if (IsLargeRegion()) {
        func(reinterpret_cast<BaseObject*>(GetRegionStart()));
    } else if (IsSmallRegion()) {
        uintptr_t position = GetRegionStart();
        uintptr_t allocPtr = GetRegionAllocPtr();
        while (position < allocPtr) {
            // GetAllocSize should before call func, because object maybe destroy in compact gc.
            size_t size = RegionSpace::GetAllocSize(*reinterpret_cast<BaseObject*>(position));
            func(reinterpret_cast<BaseObject*>(position));
            position += size;
        }
    }
}

bool RegionInfo::VisitLiveObjectsUntilFalse(const std::function<bool(BaseObject*)>&& func)
{
    // no need to visit this region.
    if (GetLiveByteCount() == 0) {
        return true;
    }
    if (IsLargeRegion()) {
        return func(reinterpret_cast<BaseObject*>(GetRegionStart()));
    }
    if (IsSmallRegion()) {
        uintptr_t position = GetRegionStart();
        size_t offset = 0;
        uintptr_t allocPtr = GetRegionAllocPtr();

        while (position < allocPtr) {
            BaseObject* obj = reinterpret_cast<BaseObject*>(position);
            size_t allocSize = RegionSpace::GetAllocSize(*obj);
            position += allocSize;
            if (IsSurvivedObject(offset) && !func(obj)) { return false; }
            offset += allocSize;
        }
    }
    return true;
}

void RegionList::MergeRegionList(RegionList& srcList, RegionInfo::RegionType regionType)
{
    RegionList regionList("region list cache");
    srcList.MoveTo(regionList);
    RegionInfo* head = regionList.GetHeadRegion();
    RegionInfo* tail = regionList.GetTailRegion();
    if (head == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(listMutex);
    regionList.SetElementType(regionType);
    IncCounts(regionList.GetRegionCount(), regionList.GetUnitCount());
    if (listHead == nullptr) {
        listHead = head;
        listTail = tail;
    } else {
        tail->SetNextRegion(listHead);
        listHead->SetPrevRegion(tail);
        listHead = head;
    }
}

void RegionList::PrependRegion(RegionInfo* region, RegionInfo::RegionType type)
{
    std::lock_guard<std::mutex> lock(listMutex);
    PrependRegionLocked(region, type);
}

void RegionList::PrependRegionLocked(RegionInfo* region, RegionInfo::RegionType type)
{
    if (region == nullptr) {
        return;
    }

    DLOG(REGION, "list %p (%zu, %zu)+(%zu, %zu) prepend region %p@[%#zx+%zu, %#zx) type %u->%u", this,
        regionCount, unitCount, 1llu, region->GetUnitCount(), region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType(), type);

    region->SetRegionType(type);
    region->SetPrevRegion(nullptr);
    IncCounts(1, region->GetUnitCount());
    region->SetNextRegion(listHead);
    if (listHead == nullptr) {
        MRT_ASSERT(listTail == nullptr, "PrependRegion listTail is not null");
        listTail = region;
    } else {
        listHead->SetPrevRegion(region);
    }
    listHead = region;
}

void RegionList::DeleteRegionLocked(RegionInfo* del)
{
    if (del == nullptr) {
        return;
    }
    if (listHead == nullptr || listTail == nullptr) {
        DLOG(REGION, "list %p delete skip: empty list for region %p", this, del);
        return;
    }

    // Safety guard: locate `del` within this list and use traversal neighbors
    // instead of relying on del->prev/next (may be stale if region was wrongly
    // linked by another list path).
    RegionInfo* pre = nullptr;
    RegionInfo* cur = listHead;
    while (cur != nullptr && cur != del) {
        pre = cur;
        cur = cur->GetNextRegion();
    }
    if (cur == nullptr) {
        DLOG(REGION, "list %p delete skip: region %p not in list", this, del);
        return;
    }

    RegionInfo* next = cur->GetNextRegion();

    del->SetNextRegion(nullptr);
    del->SetPrevRegion(nullptr);

    DLOG(REGION, "list %p (%zu, %zu)-(%zu, %zu) delete region %p@[%#zx+%zu, %#zx) type %u", this,
        regionCount, unitCount, 1llu, del->GetUnitCount(),
        del, del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(), del->GetRegionType());

    DecCounts(1, del->GetUnitCount());

    if (pre == nullptr) { // delete head
        listHead = next;
        if (listHead == nullptr) { // now empty
            listTail = nullptr;
            return;
        }
        listHead->SetPrevRegion(nullptr);
    } else {
        pre->SetNextRegion(next);
    }

    if (next == nullptr) { // delete tail
        listTail = pre;
        if (listTail == nullptr) { // now empty
            listHead = nullptr;
            return;
        }
    } else {
        next->SetPrevRegion(pre);
    }
}

#ifdef MRT_DEBUG
void RegionList::DumpRegionList(const char* msg)
{
    DLOG(REGION, "dump region list %s", msg);
    std::lock_guard<std::mutex> lock(listMutex);
    for (RegionInfo *region = listHead; region != nullptr; region = region->GetNextRegion()) {
        DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) units [%zu+%zu, %zu) type %u prev %p next %p", region,
            region->GetRegionStart(), region->GetRegionAllocatedSize(), region->GetRegionEnd(),
            region->GetUnitIdx(), region->GetUnitCount(), region->GetUnitIdx() + region->GetUnitCount(),
            region->GetRegionType(), region->GetPrevRegion(), region->GetNextRegion());
    }
}
#endif
inline void RegionManager::TagHugePage(RegionInfo* region, size_t num) const
{
#if defined (__linux__) || defined(__OHOS__) || defined(__ANDROID__)
    (void)madvise(reinterpret_cast<void*>(region->GetRegionStart()), num * RegionInfo::UNIT_SIZE, MADV_HUGEPAGE);
#else
    (void)region;
    (void)num;
#endif
}

inline void RegionManager::UntagHugePage(RegionInfo* region, size_t num) const
{
#if defined (__linux__) || defined(__OHOS__) || defined(__ANDROID__)
    (void)madvise(reinterpret_cast<void*>(region->GetRegionStart()), num * RegionInfo::UNIT_SIZE, MADV_NOHUGEPAGE);
#else
    (void)region;
    (void)num;
#endif
}

size_t FreeRegionManager::ReleaseGarbageRegions(size_t targetCachedSize)
{
    size_t dirtyBytes = dirtyUnitTree.GetTotalCount() * RegionInfo::UNIT_SIZE;
    if (dirtyBytes <= targetCachedSize) {
        VLOG(REPORT, "release heap garbage memory 0 bytes, cache %zu(%zu) bytes", dirtyBytes, targetCachedSize);
        return 0;
    }

    size_t releasedBytes = 0;
    while (dirtyBytes > targetCachedSize) {
        std::lock_guard<std::mutex> lock1(dirtyUnitTreeMutex);
        auto node = dirtyUnitTree.RootNode();
        if (node == nullptr) { break; }
        Index idx = node->GetIndex();
        UnitCount num = node->GetCount();
        dirtyUnitTree.ReleaseRootNode();

        std::lock_guard<std::mutex> lock2(releasedUnitTreeMutex);
        CHECK_DETAIL(releasedUnitTree.MergeInsert(idx, num, true), "tid %d: failed to release garbage units[%u+%u, %u)",
                     GetTid(), idx, num, idx + num);
        releasedBytes += (num * RegionInfo::UNIT_SIZE);
        dirtyBytes = dirtyUnitTree.GetTotalCount() * RegionInfo::UNIT_SIZE;
    }
    VLOG(REPORT, "release heap garbage memory %zu bytes, cache %zu(%zu) bytes",
         releasedBytes, dirtyBytes, targetCachedSize);
    return releasedBytes;
}

void RegionManager::SetMaxUnitCountForRegion()
{
    maxUnitCountPerRegion = CangjieRuntime::GetHeapParam().regionSize * KB / RegionInfo::UNIT_SIZE;
}

void RegionManager::SetMaxUnitCountForPinnedRegion()
{
    auto env = std::getenv("cjPinnedRegionSize");
    if (env == nullptr) {
        maxUnitCountPerPinnedRegion = maxUnitCountPerRegion;
        return;
    }
    size_t size = CString::ParseSizeFromEnv(env);
    // The minimum region size is system page size, measured in KB.
    size_t minSize = MapleRuntime::MRT_PAGE_SIZE / KB;
    if (size >= minSize && size <= CangjieRuntime::GetHeapParam().regionSize) {
        maxUnitCountPerPinnedRegion = size * KB / RegionInfo::UNIT_SIZE;
    } else {
        LOG(RTLOG_ERROR, "Unsupported cjPinnedRegionSize parameter. Valid cjPinnedRegionSize"
            "range is [%zuKB, %zuKB].\n", minSize, CangjieRuntime::GetHeapParam().regionSize);
    }
}

void RegionManager::SetLargeObjectThreshold()
{
    auto env = std::getenv("cjLargeThresholdSize");
    if (env == nullptr) {
        // default value is 32 KB
        largeObjectThreshold = 32 * KB;
    }
    size_t size = CString::ParseSizeFromEnv(env);
    // The minimum region size is system page size, measured in KB.
    size_t minSize = MapleRuntime::MRT_PAGE_SIZE / KB;
    // 64UL: The maximum region size, measured in KB, the value is 2048 KB.
    size_t maxSize = 10 * 1024UL;
    if (size >= minSize && size <= maxSize) {
        largeObjectThreshold = size * KB;
    } else if (size != 0) {
        LOG(RTLOG_ERROR, "Unsupported cjLargeThresholdSize parameter. Valid cjLargeThresholdSize"
            "range is [%zuKB, 2048KB].\n", minSize);
    }
    size_t regionSize = CangjieRuntime::GetHeapParam().regionSize * KB;
    largeObjectThreshold = largeObjectThreshold > regionSize ? regionSize :  largeObjectThreshold;
}

void RegionManager::SetGarbageThreshold()
{
    fromSpaceGarbageThreshold = CangjieRuntime::GetGCParam().garbageThreshold;
}

#if defined(__EULER__)
void RegionManager::SetCacheRatio(double minSize, double maxSize, double defaultParam)
{
    auto env = std::getenv("cjCacheRatio");
    if (env == nullptr) {
        cacheRatio = defaultParam;
        return;
    }
    double size = CString::ParsePosDecFromEnv(env);
    if (size - minSize >= 0 && maxSize - size >= 0) {
        cacheRatio = size;
        return;
    } else {
        LOG(RTLOG_ERROR, "Unsupported cjCacheRatio parameter.Valid cjCacheRatio range is [%f, %f].\n",
            minSize, maxSize);
    }
    cacheRatio = defaultParam;
}
#endif

// Helper function to get space ratio from environment variable
static int GetSpaceRatio(const char* envName, int defaultValue)
{
    auto env = std::getenv(envName);
    if (env != nullptr) {
        int value = std::atoi(env);
        if (value >= 0) {
            return value;
        }
    }
    return defaultValue;
}

void RegionManager::Initialize(size_t nUnit, uintptr_t regionInfoAddr)
{
    size_t metadataSize = GetMetadataSize(nUnit);
#ifdef _WIN64
    MemMap::CommitMemory(reinterpret_cast<void*>(regionInfoAddr), metadataSize);
#endif
    this->regionInfoStart = regionInfoAddr;
    this->regionHeapStart = regionInfoAddr + metadataSize;
    this->regionHeapEnd = regionHeapStart + nUnit * RegionInfo::UNIT_SIZE;

    // Read space ratios from environment variables (default 6:3:1 for MAIN:RESERVE:LOS)
    int mainRatio = GetSpaceRatio("cjMainRatio", 6);
    int reserveRatio = GetSpaceRatio("cjReserveRatio", 3);
    int losRatio = GetSpaceRatio("cjLOSRatio", 1);
    int totalRatio = mainRatio + reserveRatio + losRatio;

    // Calculate unit counts for each space
    size_t mainUnitCount = nUnit;
    size_t reserveUnitCount = 0;
    size_t losUnitCount = 0;

    if (totalRatio > 0 && (reserveRatio > 0 || losRatio > 0)) {
        // Calculate based on ratios
        losUnitCount = (nUnit * losRatio) / totalRatio;
        reserveUnitCount = (nUnit * reserveRatio) / totalRatio;
        mainUnitCount = nUnit - losUnitCount - reserveUnitCount;
    }

    // Calculate boundaries
    uintptr_t mainEnd = regionHeapStart + mainUnitCount * RegionInfo::UNIT_SIZE;
    uintptr_t reserveStart = mainEnd;
    uintptr_t reserveEnd = reserveStart + reserveUnitCount * RegionInfo::UNIT_SIZE;
    uintptr_t losStart = reserveEnd;
    uintptr_t losEnd = regionHeapEnd;

    // For MAIN space, update regionHeapEnd to exclude RESERVE and LOS
    // Note: We keep regionHeapEnd as the full end for now, but inactiveZone controls MAIN allocation
    this->inactiveZone = regionHeapStart;
    this->mainEnd = mainEnd;  // M4b: store MAIN upper boundary

    // Initialize RESERVE manager if reserveRatio > 0
    if (reserveRatio > 0 && reserveUnitCount > 0) {
        reserveManager = new (std::nothrow) ReserveManager();
        if (reserveManager != nullptr) {
            reserveManager->Initialize(reserveStart, reserveEnd, RegionInfo::UNIT_SIZE);
        }
    }

    // Initialize LOS manager if losRatio > 0
    if (losRatio > 0 && losUnitCount > 0) {
        size_t losUnitStart = mainUnitCount + reserveUnitCount;
        size_t losUnitEnd = nUnit;
        losManager = new (std::nothrow) LOSManager(
            *this, static_cast<CartesianTree::Index>(losUnitStart), reserveManager);
        if (losManager != nullptr) {
            losManager->Initialize(losStart, losEnd, losUnitStart, losUnitEnd);
        }
    }

    SetMaxUnitCountForRegion();
    SetMaxUnitCountForPinnedRegion();
    SetLargeObjectThreshold();
    SetGarbageThreshold();
#if defined(__EULER__)
    SetCacheRatio(0.0, 1.0, 1.0);
#endif
    // propagate region heap layout - only for MAIN space units
    RegionInfo::Initialize(nUnit, regionHeapStart);
    // FreeRegionManager only manages MAIN space units
    freeRegionManager.Initialize(mainUnitCount);
    // [BISECT] lowAddressFirst disabled
    freeRegionManager.SetLowAddressFirst(false);
    this->exemptedRegionThreshold = CangjieRuntime::GetHeapParam().exemptionThreshold;

    // Log space partitioning
    DLOG(REPORT, "region info @0x%zx+%zu, heap [0x%zx, 0x%zx), unit count %zu", regionInfoAddr, metadataSize,
         regionHeapStart, regionHeapEnd, nUnit);
    DLOG(REPORT, "Space partition ratio %d:%d:%d (MAIN:RESERVE:LOS)", mainRatio, reserveRatio, losRatio);
    DLOG(REPORT, "  MAIN:    [0x%zx, 0x%zx) %zu units", regionHeapStart, mainEnd, mainUnitCount);
    if (reserveManager != nullptr) {
        DLOG(REPORT, "  RESERVE: [0x%zx, 0x%zx) %zu units", reserveStart, reserveEnd, reserveUnitCount);
    }
    if (losManager != nullptr) {
        DLOG(REPORT, "  LOS:     [0x%zx, 0x%zx) %zu units", losStart, losEnd, losUnitCount);
    }
}

void RegionManager::ReclaimRegion(RegionInfo* region)
{
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "reclaim region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

    region->InitFreeUnits();

    // M5: If region is in RESERVE-borrowed territory, defer for return to RESERVE.
    // Protected by pendingReserveReturnsMutex (push_back from mutator/GC/FP threads concurrently).
    if (IsInReserveTerritory(region->GetRegionStart())) {
        std::lock_guard<std::mutex> lock(pendingReserveReturnsMutex);
        pendingReserveReturns.push_back({unitIndex, num});
        DLOG(REGION, "MAIN reclaim: RESERVE-borrowed region deferred for return, units[%zu+%zu)",
             unitIndex, num);
        return;
    }

    freeRegionManager.AddGarbageUnits(unitIndex, num);
}

size_t RegionManager::ReleaseRegion(RegionInfo* region)
{
    size_t res = region->GetRegionSize();
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "release region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

    region->InitFreeUnits();
    RegionInfo::ReleaseUnits(unitIndex, num);

    // M5: If region is in RESERVE-borrowed territory, defer for return to RESERVE.
    if (IsInReserveTerritory(region->GetRegionStart())) {
        std::lock_guard<std::mutex> lock(pendingReserveReturnsMutex);
        pendingReserveReturns.push_back({unitIndex, num});
        DLOG(REGION, "MAIN release: RESERVE-borrowed region deferred for return, units[%zu+%zu)",
             unitIndex, num);
        return res;
    }

    freeRegionManager.AddReleaseUnits(unitIndex, num);
    return res;
}

void RegionManager::ReassembleFromSpace()
{
    fromRegionList.MergeRegionList(unmovableFromRegionList, RegionInfo::RegionType::FROM_REGION);
}

void RegionManager::CountLiveObject(const BaseObject* obj)
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    region->AddLiveByteCount(obj->GetSize());
}

void RegionManager::AssembleSmallGarbageCandidates()
{
    fromRegionList.MergeRegionList(rawPointerPinnedRegionList, RegionInfo::RegionType::FROM_REGION);
    fromRegionList.MergeRegionList(recentFullRegionList, RegionInfo::RegionType::FROM_REGION);
    fromRegionList.MergeRegionList(unmovableFromRegionList, RegionInfo::RegionType::FROM_REGION);

    fromRegionList.VisitAllRegions([](RegionInfo* region) { region->ClearLiveInfo(); });
}

void RegionManager::AssembleLargeGarbageCandidates()
{
    oldLargeRegionList.MergeRegionList(recentLargeRegionList, RegionInfo::RegionType::LARGE_REGION);
    for (RegionInfo* region = oldLargeRegionList.GetHeadRegion(); region != nullptr; region = region->GetNextRegion()) {
        region->ClearLiveInfo();
    }
}

void RegionManager::AssemblePinnedGarbageCandidates(bool collectAll)
{
    oldPinnedRegionList.MergeRegionList(recentPinnedRegionList, RegionInfo::RegionType::FULL_PINNED_REGION);
    RegionInfo* region = oldPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* nextRegion = region->GetNextRegion();
        if (collectAll && (region->GetRawPointerObjectCount() > 0)) {
            oldPinnedRegionList.DeleteRegion(region);
            rawPointerPinnedRegionList.PrependRegion(region, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
        }
        region->ClearLiveInfo();
        region = nextRegion;
    }
}

// M3-fix: Select MAIN large-object migration candidates BEFORE trace.
// Candidates are moved from oldLargeRegionList to migrationCandidateList with
// FROM_REGION type so that GetAndTryTagRefField/IsFromObject tags incoming
// references during Enum/Trace.  This must run BEFORE AssembleLargeGarbageCandidates
// (which merges recentLargeRegionList into oldLargeRegionList).
void RegionManager::AssembleMigrationCandidates()
{
    // Safety gate: opt-in via environment variable.
    const char* migrateEnabled = std::getenv("cjEnableLOSMigration");
    if (migrateEnabled == nullptr || migrateEnabled[0] != '1') {
        return;
    }
    if (losManager == nullptr) {
        return;
    }
    if (losManager->IsCompacting() || losManager->IsCompactionFrozen()) {
        return;
    }

    RegionInfo* region = oldLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();

        // Cannot move raw-pointer objects (future, monitor).
        if (region->GetRawPointerObjectCount() != 0) {
            region = next;
            continue;
        }
        // Only migrate from MAIN space proper (skip RESERVE-borrowed regions).
        if (mainEnd > 0 && region->GetRegionStart() >= mainEnd) {
            region = next;
            continue;
        }

        // Move to migrationCandidateList with FROM_REGION type → IsFromObject() = true
        // during Enum/Trace, so references to this object will be tagged.
        oldLargeRegionList.DeleteRegion(region);
        migrationCandidateList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
        region->ClearLiveInfo();
        region->ResetMarkBit();  // M3-fix: reset isMarked so trace re-marks and counts live bytes

        region = next;
    }
}

// M3-fix: After forward/collect, move any unmigrated candidates back to oldLargeRegionList.
void RegionManager::CleanupMigrationCandidates()
{
    oldLargeRegionList.MergeRegionList(migrationCandidateList, RegionInfo::RegionType::LARGE_REGION);
}
void RemoveRegionLocked(RegionList* regionList, RegionInfo* region)
{
    regionList->DeleteRegionLocked(region);
}

// forward only regions whose garbage bytes is greater than or equal to exemptedRegionThreshold.
size_t RegionManager::ExemptFromRegions()
{
    size_t forwardBytes = 0;
    size_t floatingGarbage = 0;
    size_t oldFromBytes = fromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    double exempt = exemptedRegionThreshold;
    auto visitor = [this, exempt, &floatingGarbage](RegionInfo* fromRegion) {
        size_t threshold = static_cast<size_t>(exempt * fromRegion->GetRegionSize());
        size_t liveBytes = fromRegion->GetLiveByteCount();
        long rawPtrCnt = fromRegion->GetRawPointerObjectCount();
        if (liveBytes > threshold) { // ignore this region
            RegionInfo* del = fromRegion;
            DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) exempted by forwarding: %zu units, %u live bytes", del,
                del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(),
                del->GetUnitCount(), del->GetLiveByteCount());

            CHECK(del->IsFromRegion());
            RemoveRegionLocked(&fromRegionList, del);
            ExemptFromRegion(del);
            floatingGarbage += (del->GetRegionSize() - del->GetLiveByteCount());
        } else if (rawPtrCnt > 0) {
            RegionInfo* del = fromRegion;
            DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) pinned by forwarding: %zu units, %u live bytes rawPtr cnt %u",
                del, del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(),
                del->GetUnitCount(), del->GetLiveByteCount(), rawPtrCnt);
            CHECK(del->IsFromRegion());
            RemoveRegionLocked(&fromRegionList, del);
            rawPointerPinnedRegionList.PrependRegion(del, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
            floatingGarbage += (del->GetRegionSize() - del->GetLiveByteCount());
        }
    };
    fromRegionList.VisitAllRegions(visitor);

    size_t newFromBytes = fromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    size_t exemptedFromBytes = unmovableFromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    VLOG(REPORT, "exempt from-space: %zu B - %zu B -> %zu B, %zu B floating garbage, %zu B to forward",
         oldFromBytes, exemptedFromBytes, newFromBytes, floatingGarbage, forwardBytes);
    return newFromBytes - forwardBytes;
}

void RegionManager::ForEachObjUnsafe(const std::function<void(BaseObject*)>& visitor) const
{
    for (uintptr_t regionAddr = regionHeapStart; regionAddr < inactiveZone;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddr);
        regionAddr = region->GetRegionEnd();
        if (!region->IsValidRegion() || region->IsFreeRegion() || region -> IsGarbageRegion()) {
            continue;
        }
        region->VisitAllObjects([&visitor](BaseObject* object) { visitor(object); });
    }

    // Also traverse LOS regions (they live outside [regionHeapStart, inactiveZone))
    if (losManager != nullptr) {
        auto visitList = [&visitor](RegionList& list) {
            for (RegionInfo* r = list.GetHeadRegion(); r != nullptr; r = r->GetNextRegion()) {
                r->VisitAllObjects([&visitor](BaseObject* object) { visitor(object); });
            }
        };
        visitList(losManager->GetOldLargeRegionList());
        visitList(losManager->GetRecentLargeRegionList());
    }
}

void RegionManager::ForEachObjSafe(const std::function<void(BaseObject*)>& visitor) const
{
    ScopedEnterSaferegion enterSaferegion(false);
    ScopedStopTheWorld stw("visit all objects");
    ForEachObjUnsafe(visitor);
}

RegionInfo* RegionManager::TakeRegion(size_t num, RegionInfo::UnitRole type, bool expectPhysicalMem)
{
    // a chance to invoke heuristic gc.
    if (!Heap::GetHeap().IsGcStarted()) {
        Collector& collector = Heap::GetHeap().GetCollector();
        size_t threshold = collector.GetGCStats().GetThreshold();
        size_t allocated = Heap::GetHeap().GetAllocator().AllocatedBytes();
        if (allocated >= threshold) {
            DLOG(ALLOC, "request heu gc: allocated %zu, threshold %zu", allocated, threshold);
            collector.RequestGC(GC_REASON_HEU, true);
        }
    }

    // check for allocation since we do not want gc threads and mutators do any harm to each other.
    size_t size = num * RegionInfo::UNIT_SIZE;
    RequestForRegion(size);

    RegionInfo* head = garbageRegionList.TakeHeadRegion();
    if (head != nullptr) {
        DLOG(REGION, "take garbage region %p@[%#zx, %#zx)", head, head->GetRegionStart(), head->GetRegionEnd());
        if (head->GetUnitCount() == num) {
            auto idx = head->GetUnitIdx();
            RegionInfo::ClearUnits(idx, num);
            DLOG(REGION, "reuse garbage region %p@[%#zx, %#zx)", head, head->GetRegionStart(), head->GetRegionEnd());
            return RegionInfo::InitRegion(idx, num, type);
        } else {
            DLOG(REGION, "reclaim garbage region %p@[%#zx, %#zx)", head, head->GetRegionStart(), head->GetRegionEnd());
            ReclaimRegion(head);
        }
    }

    RegionInfo* region = freeRegionManager.TakeRegion(num, type, expectPhysicalMem);
    if (region != nullptr) {
        if (num >= HUGE_PAGE) {
            TagHugePage(region, num);
        }
        return region;
    }

    // when free regions are not enough for allocation
    if (num <= GetInactiveUnitCount()) {
        uintptr_t addr = inactiveZone.fetch_add(size);
        if (addr < mainEnd - size) {  // M4b: use mainEnd instead of regionHeapEnd
            region = RegionInfo::InitRegionAt(addr, num, type);
            size_t idx = region->GetUnitIdx();
#ifdef _WIN64
            MemMap::CommitMemory(
                reinterpret_cast<void*>(RegionInfo::GetUnitAddress(idx)), num * RegionInfo::UNIT_SIZE);
#endif
            (void)idx; // eliminate compilation warning
            DLOG(REGION, "take inactive units [%zu+%zu, %zu) at [0x%zx, 0x%zx)", idx, num, idx + num,
                 RegionInfo::GetUnitAddress(idx), RegionInfo::GetUnitAddress(idx + num));
            if (num >= HUGE_PAGE) {
                TagHugePage(region, num);
            }
            if (expectPhysicalMem) {
                RegionInfo::ClearUnits(idx, num);
            }
            return region;
        } else {
            (void)inactiveZone.fetch_sub(size);
        }
    }

    return nullptr;
}

void RegionManager::ForwardFromRegions(GCThreadPool* threadPool)
{
    if (threadPool != nullptr) {
        int32_t threadNum = threadPool->GetMaxThreadNum() + 1;
        // We won't change fromRegionList during gc, so we can use it without lock.
        size_t regionCount = fromRegionList.GetRegionCount();
        if (UNLIKELY(regionCount == 0)) {
            return;
        }

        // we start threadPool before adding work so that we can concurrently add tasks;
        threadPool->Start();
        for (int32_t i = 0; i < threadNum; ++i) {
            threadPool->AddWork(new (std::nothrow) ForwardTask(*this, fromRegionList));
        }
        threadPool->WaitFinish();
    } else {
        ForwardFromRegions();
    }
}

void RegionManager::ExemptFromRegion(RegionInfo* region)
{
    unmovableFromRegionList.PrependRegion(region, RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
}

void RegionManager::ForwardFromRegions()
{
    RegionInfo* fromRegion = fromRegionList.GetHeadRegion();
    while (fromRegion != nullptr) {
        MRT_ASSERT(fromRegion->IsValidRegion(), "the head region of fromRegionList is invalid");
        RegionInfo* region = fromRegion;
        fromRegion = fromRegion->GetNextRegion();
        ForwardRegion(region);
    }

    VLOG(REPORT, "forward %zu from-region units", fromRegionList.GetUnitCount());

    AllocBuffer* allocBuffer = AllocBuffer::GetAllocBuffer();
    if (LIKELY(allocBuffer != nullptr)) {
        allocBuffer->ClearRegion(); // clear region for next GC
    }
}

size_t RegionManager::CollectFreePinnedSlots(RegionInfo* region)
{
    // traverse pinned region to reclaim free pinned objects.
    size_t start = region->GetRegionStart();
    size_t garbageSize = 0;
    region->VisitAllObjects([this, region, start, &garbageSize](BaseObject* object) {
        size_t offset = reinterpret_cast<MAddress>(object) - start;
        if (!region->IsSurvivedObject(offset)) {
            size_t objSize = object->GetSize();
            DLOG(ALLOC, "reclaim pinned obj %p<%p>(%zu)", object, object->GetTypeInfo(), objSize);
            garbageSize += objSize;
            std::lock_guard<std::mutex> lock(freePinnedSlotListMutex);
            ReleaseNativeResource(object);
            freePinnedSlotLists.PushFront(object);
        }
    });
    return garbageSize;
}

size_t RegionManager::CollectPinnedGarbage()
{
    {
        std::lock_guard<std::mutex> lock(freePinnedSlotListMutex);
        freePinnedSlotLists.Clear();
    }
    size_t garbageSize = 0;
    RegionInfo* region = oldPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        if (region->GetLiveByteCount() == 0) {
            RegionInfo* del = region;
            region = region->GetNextRegion();
            oldPinnedRegionList.DeleteRegion(del);

            auto fixToObj = [](BaseObject* obj) { ReleaseNativeResource(obj); };
            del->VisitAllObjects(fixToObj);

            garbageSize += CollectRegion(del);
            continue;
        } else {
            garbageSize += CollectFreePinnedSlots(region);
            region = region->GetNextRegion();
        }
    }
    return garbageSize;
}

size_t RegionManager::CollectLargeGarbage()
{
    size_t garbageSize = 0;
    RegionInfo* region = oldLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        // for large region, the offset of obj is 0
        if (!region->IsSurvivedObject(0)) {
            DLOG(REGION, "reclaim large region %p@[0x%zx+%zu, 0x%zx) type %u", region, region->GetRegionStart(),
                 region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

            RegionInfo* del = region;
            region = region->GetNextRegion();
            oldLargeRegionList.DeleteRegion(del);
            if (del->GetRegionSize() > RegionInfo::LARGE_OBJECT_RELEASE_THRESHOLD) {
                garbageSize += ReleaseRegion(del);
            } else {
                garbageSize += CollectRegion(del);
            }
        } else {
            region->ResetMarkBit();
            region = region->GetNextRegion();
        }
    }

    region = recentLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        region->ResetMarkBit();
        region = region->GetNextRegion();
    }

    return garbageSize;
}

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
void RegionManager::DumpRegionInfo() const
{
    if (!ENABLE_LOG(ALLOC)) {
        return;
    }
    for (uintptr_t regionAddr = regionHeapStart; regionAddr < inactiveZone;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddr);
        regionAddr = region->GetRegionEnd();
        if (!region->IsFreeRegion()) {
            region->DumpRegionInfo(ALLOC);
        }
    }
}
#endif

void RegionManager::DumpRegionStats(const char* msg) const
{
    size_t totalSize = regionHeapEnd - regionHeapStart;
    size_t totalUnits = totalSize / RegionInfo::UNIT_SIZE;
    size_t activeSize = inactiveZone - regionHeapStart;
    size_t activeUnits = activeSize / RegionInfo::UNIT_SIZE;

    size_t tlRegions = tlRegionList.GetRegionCount();
    size_t tlUnits = tlRegionList.GetUnitCount();
    size_t tlSize = tlUnits * RegionInfo::UNIT_SIZE;
    size_t allocTLSize = tlRegionList.GetAllocatedSize();

    size_t fromRegions = fromRegionList.GetRegionCount();
    size_t fromUnits = fromRegionList.GetUnitCount();
    size_t fromSize = fromUnits * RegionInfo::UNIT_SIZE;
    size_t allocFromSize = fromRegionList.GetAllocatedSize();

    size_t recentFullRegions = recentFullRegionList.GetRegionCount();
    size_t recentFullUnits = recentFullRegionList.GetUnitCount();
    size_t recentFullSize = recentFullUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentFullSize = recentFullRegionList.GetAllocatedSize();

    size_t garbageRegions = garbageRegionList.GetRegionCount();
    size_t garbageUnits = garbageRegionList.GetUnitCount();
    size_t garbageSize = garbageUnits * RegionInfo::UNIT_SIZE;
    size_t allocGarbageSize = garbageRegionList.GetAllocatedSize();

    size_t pinnedRegions = oldPinnedRegionList.GetRegionCount();
    size_t pinnedUnits = oldPinnedRegionList.GetUnitCount();
    size_t pinnedSize = pinnedUnits * RegionInfo::UNIT_SIZE;
    size_t allocPinnedSize = oldPinnedRegionList.GetAllocatedSize();

    size_t recentPinnedRegions = recentPinnedRegionList.GetRegionCount();
    size_t recentPinnedUnits = recentPinnedRegionList.GetUnitCount();
    size_t recentPinnedSize = recentPinnedUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentPinnedSize = recentPinnedRegionList.GetAllocatedSize();

    size_t rawPointerPinnedRegions = rawPointerPinnedRegionList.GetRegionCount();
    size_t rawPointerPinnedUnits = rawPointerPinnedRegionList.GetUnitCount();
    size_t rawPointerPinnedSize = rawPointerPinnedUnits * RegionInfo::UNIT_SIZE;
    size_t allocRawPointerPinnedSize = rawPointerPinnedRegionList.GetAllocatedSize();

    size_t largeRegions = oldLargeRegionList.GetRegionCount();
    size_t largeUnits = oldLargeRegionList.GetUnitCount();
    size_t largeSize = largeUnits * RegionInfo::UNIT_SIZE;
    size_t allocLargeSize = oldLargeRegionList.GetAllocatedSize();

    size_t recentlargeRegions = recentLargeRegionList.GetRegionCount();
    size_t recentlargeUnits = recentLargeRegionList.GetUnitCount();
    size_t recentLargeSize = recentlargeUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentLargeSize = recentLargeRegionList.GetAllocatedSize();

    size_t usedUnits = GetUsedUnitCount();
    size_t releasedUnits = freeRegionManager.GetReleasedUnitCount();
    size_t dirtyUnits = freeRegionManager.GetDirtyUnitCount();
    size_t listedUnits = fromUnits + garbageUnits + recentFullUnits + tlRegions +
        rawPointerPinnedRegions + largeUnits + recentlargeUnits + pinnedUnits + recentPinnedUnits;

    VLOG(REPORT, msg);

    VLOG(REPORT, "\ttotal units: %zu (%zu B)", totalUnits, totalSize);
    VLOG(REPORT, "\tactive units: %zu (%zu B)", activeUnits, activeSize);

    VLOG(REPORT, "\ttl-regions %zu: %zu units (%zu B, alloc %zu)", tlRegions,  tlUnits, tlSize, allocTLSize);
    VLOG(REPORT, "\tfrom-regions %zu: %zu units (%zu B, alloc %zu)", fromRegions,  fromUnits, fromSize, allocFromSize);
    VLOG(REPORT, "\trecent-full regions %zu: %zu units (%zu B, alloc %zu)",
         recentFullRegions, recentFullUnits, recentFullSize, allocRecentFullSize);
    VLOG(REPORT, "\tgarbage regions %zu: %zu units (%zu B, alloc %zu)",
         garbageRegions, garbageUnits, garbageSize, allocGarbageSize);
    VLOG(REPORT, "\tpinned regions %zu: %zu units (%zu B, alloc %zu)",
         pinnedRegions, pinnedUnits, pinnedSize, allocPinnedSize);
    VLOG(REPORT, "\trecent pinned regions %zu: %zu units (%zu B, alloc %zu)",
         recentPinnedRegions, recentPinnedUnits, recentPinnedSize, allocRecentPinnedSize);
    VLOG(REPORT, "\trawPointer pinned regions %zu: %zu units (%zu B, alloc %zu)",
         rawPointerPinnedRegions, rawPointerPinnedUnits, rawPointerPinnedSize, allocRawPointerPinnedSize);
    VLOG(REPORT, "\tlarge-object regions %zu: %zu units (%zu B, alloc %zu)",
         largeRegions, largeUnits, largeSize, allocLargeSize);
    VLOG(REPORT, "\trecent large-object regions %zu: %zu units (%zu B, alloc %zu)",
         recentlargeRegions, recentlargeUnits, recentLargeSize, allocRecentLargeSize);

    VLOG(REPORT, "\tlisted units: %zu (%zu B)", listedUnits, listedUnits * RegionInfo::UNIT_SIZE);
    VLOG(REPORT, "\tused units: %zu (%zu B)", usedUnits, usedUnits * RegionInfo::UNIT_SIZE);
    VLOG(REPORT, "\treleased units: %zu (%zu B)", releasedUnits, releasedUnits * RegionInfo::UNIT_SIZE);
    VLOG(REPORT, "\tdirty units: %zu (%zu B)", dirtyUnits, dirtyUnits * RegionInfo::UNIT_SIZE);

    TRACE_COUNT("CJRT_GC_totalSize", totalSize);
    TRACE_COUNT("CJRT_GC_totalUnits", totalUnits);
    TRACE_COUNT("CJRT_GC_activeSize", activeSize);
    TRACE_COUNT("CJRT_GC_activeUnits", activeUnits);
    TRACE_COUNT("CJRT_GC_tlRegions", tlRegions);
    TRACE_COUNT("CJRT_GC_tlUnits", tlUnits);
    TRACE_COUNT("CJRT_GC_tlSize", tlSize);
    TRACE_COUNT("CJRT_GC_allocTLSize", allocTLSize);
    TRACE_COUNT("CJRT_GC_fromRegions", fromRegions);
    TRACE_COUNT("CJRT_GC_fromUnits", fromUnits);
    TRACE_COUNT("CJRT_GC_fromSize", fromSize);
    TRACE_COUNT("CJRT_GC_allocFromSize", allocFromSize);
    TRACE_COUNT("CJRT_GC_recentFullRegions", recentFullRegions);
    TRACE_COUNT("CJRT_GC_recentFullUnits", recentFullUnits);
    TRACE_COUNT("CJRT_GC_recentFullSize", recentFullSize);
    TRACE_COUNT("CJRT_GC_allocRecentFullSize", allocRecentFullSize);
    TRACE_COUNT("CJRT_GC_garbageRegions", garbageRegions);
    TRACE_COUNT("CJRT_GC_garbageUnits", garbageUnits);
    TRACE_COUNT("CJRT_GC_garbageSize", garbageSize);
    TRACE_COUNT("CJRT_GC_allocGarbageSize", allocGarbageSize);
    TRACE_COUNT("CJRT_GC_pinnedRegions", pinnedRegions);
    TRACE_COUNT("CJRT_GC_pinnedUnits", pinnedUnits);
    TRACE_COUNT("CJRT_GC_pinnedSize", pinnedSize);
    TRACE_COUNT("CJRT_GC_allocPinnedSize", allocPinnedSize);
    TRACE_COUNT("CJRT_GC_recentPinnedRegions", recentPinnedRegions);
    TRACE_COUNT("CJRT_GC_recentPinnedUnits", recentPinnedUnits);
    TRACE_COUNT("CJRT_GC_recentPinnedSize", recentPinnedSize);
    TRACE_COUNT("CJRT_GC_allocRecentPinnedSize", allocRecentPinnedSize);
    TRACE_COUNT("CJRT_GC_rawPointerPinnedRegions", rawPointerPinnedRegions);
    TRACE_COUNT("CJRT_GC_rawPointerPinnedUnits", rawPointerPinnedUnits);
    TRACE_COUNT("CJRT_GC_rawPointerPinnedSize", rawPointerPinnedSize);
    TRACE_COUNT("CJRT_GC_allocRawPointerPinnedSize", allocRawPointerPinnedSize);
    TRACE_COUNT("CJRT_GC_largeRegions", largeRegions);
    TRACE_COUNT("CJRT_GC_largeUnits", largeUnits);
    TRACE_COUNT("CJRT_GC_largeSize", largeSize);
    TRACE_COUNT("CJRT_GC_allocLargeSize", allocLargeSize);
    TRACE_COUNT("CJRT_GC_recentlargeRegions", recentlargeRegions);
    TRACE_COUNT("CJRT_GC_recentlargeUnits", recentlargeUnits);
    TRACE_COUNT("CJRT_GC_recentLargeSize", recentLargeSize);
    TRACE_COUNT("CJRT_GC_allocRecentLargeSize", allocRecentLargeSize);
    TRACE_COUNT("CJRT_GC_usedUnits", usedUnits);
    TRACE_COUNT("CJRT_GC_releasedUnits", releasedUnits);
    TRACE_COUNT("CJRT_GC_dirtyUnits", dirtyUnits);
    TRACE_COUNT("CJRT_GC_listedUnits", listedUnits);

    // === Fragmentation Statistics ===
    // External fragmentation: free-space distribution in MAIN
    size_t totalFreeUnits = releasedUnits + dirtyUnits;
    size_t maxContigFreeUnits = freeRegionManager.GetMaxContiguousFreeUnits();
    size_t maxContigFreeBytes = maxContigFreeUnits * RegionInfo::UNIT_SIZE;
    float extFragIndex = (totalFreeUnits > 0)
        ? 1.0f - static_cast<float>(maxContigFreeUnits) / static_cast<float>(totalFreeUnits)
        : 0.0f;
    size_t freeFragCount = freeRegionManager.GetFreeFragmentCount();

    // Internal fragmentation: actual bump-pointer allocated vs region capacity
    size_t actualAllocBytes =
        tlRegionList.GetAllocatedSize(true) +
        fromRegionList.GetAllocatedSize(true) +
        recentFullRegionList.GetAllocatedSize(true) +
        oldPinnedRegionList.GetAllocatedSize(true) +
        recentPinnedRegionList.GetAllocatedSize(true) +
        rawPointerPinnedRegionList.GetAllocatedSize(true) +
        oldLargeRegionList.GetAllocatedSize(true) +
        recentLargeRegionList.GetAllocatedSize(true);
    size_t regionCapacityBytes = usedUnits * RegionInfo::UNIT_SIZE;
    size_t internalWaste = (regionCapacityBytes > actualAllocBytes)
        ? regionCapacityBytes - actualAllocBytes : 0;
    float heapUtilization = (activeSize > 0)
        ? static_cast<float>(actualAllocBytes) / static_cast<float>(activeSize) : 0.0f;

    // Large-in-MAIN impact (key LOS comparison metric)
    float largeInMainRatio = (activeUnits > 0)
        ? static_cast<float>(largeUnits + recentlargeUnits) / static_cast<float>(activeUnits)
        : 0.0f;

    VLOG(REPORT, "\tFRAG-EXT: index %.4f, maxContiguousFree %zu units (%zu B), "
         "freeFragments %zu, totalFree %zu units",
         extFragIndex, maxContigFreeUnits, maxContigFreeBytes,
         freeFragCount, totalFreeUnits);
    VLOG(REPORT, "\tFRAG-INT: internalWaste %zu B, actualAlloc %zu B, "
         "regionCapacity %zu B, heapUtil %.2f%%",
         internalWaste, actualAllocBytes, regionCapacityBytes,
         heapUtilization * 100);
    VLOG(REPORT, "\tFRAG-LARGE: largeInMainRatio %.2f%%, "
         "largeRegions %zu (%zu units), recentLargeRegions %zu (%zu units)",
         largeInMainRatio * 100,
         largeRegions, largeUnits, recentlargeRegions, recentlargeUnits);

    TRACE_COUNT("CJRT_GC_extFragIndex_x10000",
                static_cast<size_t>(extFragIndex * 10000));
    TRACE_COUNT("CJRT_GC_maxContigFreeUnits", maxContigFreeUnits);
    TRACE_COUNT("CJRT_GC_freeFragCount", freeFragCount);
    TRACE_COUNT("CJRT_GC_internalWaste", internalWaste);
    TRACE_COUNT("CJRT_GC_actualAllocBytes", actualAllocBytes);
    TRACE_COUNT("CJRT_GC_heapUtilization_x100",
                static_cast<size_t>(heapUtilization * 100));
    TRACE_COUNT("CJRT_GC_largeInMainRatio_x100",
                static_cast<size_t>(largeInMainRatio * 100));

    // M5: MAIN / RESERVE / LOS space snapshot
    if (reserveManager != nullptr) {
        size_t reserveCap = reserveManager->GetCapacity();
        size_t reserveAvail = reserveManager->GetAvailable();
        size_t reserveMainBorrowed = reserveManager->GetMainBorrowedSize();
        size_t reserveLOSBorrowed = reserveManager->GetLOSBorrowedSize();
        size_t reserveMainReturned = reserveManager->GetMainReturnBytes();
        size_t reserveLOSReturned = reserveManager->GetLOSReturnBytes();
        VLOG(REPORT, "\tRESERVE: capacity %zu B, available %zu B, "
             "mainBorrowed %zu B, losBorrowed %zu B, "
             "mainReturned %zu B, losReturned %zu B",
             reserveCap, reserveAvail,
             reserveMainBorrowed, reserveLOSBorrowed,
             reserveMainReturned, reserveLOSReturned);
        VLOG(REPORT, "\tRESERVE: mainBorrowCount %zu, losBorrowCount %zu, cooldownSkips %zu",
             reserveManager->GetMainChunkBorrowCount(),
             reserveManager->GetLOSChunkBorrowCount(),
             reserveManager->GetCooldownSkipCount());
        TRACE_COUNT("CJRT_GC_reserveCapacity", reserveCap);
        TRACE_COUNT("CJRT_GC_reserveAvailable", reserveAvail);
        TRACE_COUNT("CJRT_GC_reserveMainBorrowed", reserveMainBorrowed);
        TRACE_COUNT("CJRT_GC_reserveLOSBorrowed", reserveLOSBorrowed);
    }
    if (losManager != nullptr) {
        size_t losCap = losManager->GetCapacity();
        size_t losUsed = losManager->GetUsedSize();
        size_t losFree = losManager->GetFreeSize();
        size_t losAllocCnt = losManager->GetAllocCount();
        size_t losFailCnt = losManager->GetAllocFailCount();
        size_t losFallback = losManager->GetFallbackCount();
        size_t losReserveBorrow = losManager->GetReserveBorrowCount();
        size_t losCompactCnt = losManager->GetCompactCount();
        size_t losGarbage = losManager->GetGarbageCollectedBytes();
        VLOG(REPORT, "\tLOS: capacity %zu B, used %zu B, free %zu B, "
             "alloc %zu, fail %zu, fallback %zu, reserveBorrow %zu",
             losCap, losUsed, losFree, losAllocCnt, losFailCnt, losFallback, losReserveBorrow);
        VLOG(REPORT, "\tLOS: compactions %zu, garbageBytes %zu, "
             "compactMoved %zu B, compactFreed %zu B, compactShrunk %zu B",
             losCompactCnt, losGarbage,
             losManager->GetCompactMovedBytes(),
             losManager->GetCompactFreedBytes(),
             losManager->GetCompactShrunkBytes());
        TRACE_COUNT("CJRT_GC_losCapacity", losCap);
        TRACE_COUNT("CJRT_GC_losUsed", losUsed);
        TRACE_COUNT("CJRT_GC_losFree", losFree);
        TRACE_COUNT("CJRT_GC_losAllocCount", losAllocCnt);
        TRACE_COUNT("CJRT_GC_losCompactCount", losCompactCnt);
    }
    VLOG(REPORT, "\tMAIN: [0x%zx, 0x%zx) end 0x%zx, inactiveZone 0x%zx, reserveBorrow %zu",
         regionHeapStart, regionHeapEnd, mainEnd,
         inactiveZone.load(std::memory_order_relaxed), mainReserveBorrowCount);
}

// Map RegionType to a single display character for the memory map visualization.
static char RegionTypeToChar(RegionInfo::RegionType type, bool hasRawPtr)
{
    if (hasRawPtr) {
        return 'X';
    }
    switch (type) {
        case RegionInfo::RegionType::FREE_REGION:                return '.';
        case RegionInfo::RegionType::THREAD_LOCAL_REGION:        return 'T';
        case RegionInfo::RegionType::RECENT_FULL_REGION:         return 'R';
        case RegionInfo::RegionType::FROM_REGION:                return 'F';
        case RegionInfo::RegionType::LONE_FROM_REGION:           return 'f';
        case RegionInfo::RegionType::UNMOVABLE_FROM_REGION:      return 'U';
        case RegionInfo::RegionType::TO_REGION:                  return 'O';
        case RegionInfo::RegionType::FULL_PINNED_REGION:         return 'P';
        case RegionInfo::RegionType::RECENT_PINNED_REGION:       return 'p';
        case RegionInfo::RegionType::RAW_POINTER_PINNED_REGION:  return 'X';
        case RegionInfo::RegionType::TL_RAW_POINTER_REGION:      return 'x';
        case RegionInfo::RegionType::TL_LARGE_RAW_POINTER_REGION:return 'x';
        case RegionInfo::RegionType::LARGE_REGION:               return 'L';
        case RegionInfo::RegionType::RECENT_LARGE_REGION:        return 'l';
        case RegionInfo::RegionType::GARBAGE_REGION:             return 'G';
        default:                                                 return '?';
    }
}

void RegionManager::DumpMemoryMap() const
{
    // === Heap Memory Layout ===
    // [regionHeapStart .. mainEnd][reserveStart .. reserveEnd][losStart .. losEnd=regionHeapEnd]
    // |<------ MAIN ----------->||<------- RESERVE -------->||<----------- LOS -------------->|
    //
    // MAIN:    grows low→high, inactiveZone = frontier, units above are inactive.
    // LOS:     grows high→low, losInactiveZone = frontier, units below are inactive.
    // RESERVE: [mainWatermark, losWatermark) is unborrowed free space.
    //
    // This function builds a per-unit state array and outputs a consolidated block table
    // showing each contiguous block's unit range, size, type char, and status description.

    size_t totalUnits = (regionHeapEnd - regionHeapStart) / RegionInfo::UNIT_SIZE;
    if (totalUnits == 0) {
        VLOG(REPORT, "MEMORY-MAP: no heap units");
        return;
    }

    // --- Compute space boundaries (unit indices) ---
    size_t mainUnitEnd = (mainEnd - regionHeapStart) / RegionInfo::UNIT_SIZE;

    size_t reserveUnitStart = mainUnitEnd;
    size_t reserveUnitEnd = mainUnitEnd;
    uintptr_t mainWm = 0;
    uintptr_t losWm = 0;
    if (reserveManager != nullptr) {
        reserveUnitStart = (reserveManager->GetReserveStart() - regionHeapStart) / RegionInfo::UNIT_SIZE;
        reserveUnitEnd = (reserveManager->GetReserveEnd() - regionHeapStart) / RegionInfo::UNIT_SIZE;
        mainWm = reserveManager->GetMainWatermark();
        losWm = reserveManager->GetLOSWatermark();
    }

    size_t losUnitStart = totalUnits;
    size_t losUnitEnd = totalUnits;
    uintptr_t losInactive = 0;
    if (losManager != nullptr) {
        losUnitStart = losManager->GetUnitStart();
        losUnitEnd = losManager->GetUnitEnd();
        losInactive = losManager->GetInactiveZone();
    }

    uintptr_t mainInactive = inactiveZone.load(std::memory_order_relaxed);
    size_t mainInactiveUnit = (mainInactive > regionHeapStart)
        ? (mainInactive - regionHeapStart) / RegionInfo::UNIT_SIZE : 0;
    size_t losInactiveUnit = (losManager != nullptr && losInactive >= regionHeapStart)
        ? (losInactive - regionHeapStart) / RegionInfo::UNIT_SIZE : losUnitEnd;

    // --- State encoding ---
    // 0          = INACTIVE  (beyond active zone, never used)
    // 1          = FREE_DIRTY  (in FreeRegionManager dirty tree)
    // 2          = FREE_RELEASED  (in FreeRegionManager released tree)
    // 3          = FREE_UNKNOWN  (free head unit in active zone, not in either tree)
    // 4          = RESERVE_FREE  (unborrowed RESERVE space)
    // 16..31     = S_USED_BASE + RegionType  (used region)
    // 0x40 flag  = region has raw pointer objects
    static constexpr uint8_t S_INACTIVE      = 0;
    static constexpr uint8_t S_FREE_DIRTY    = 1;
    static constexpr uint8_t S_FREE_RELEASED = 2;
    static constexpr uint8_t S_FREE_UNKNOWN  = 3;
    static constexpr uint8_t S_RESERVE_FREE  = 4;
    static constexpr uint8_t S_USED_BASE     = 16;
    static constexpr uint8_t S_RAW_PTR_FLAG  = 0x40;

    // --- Allocate state array ---
    uint8_t* state = static_cast<uint8_t*>(calloc(totalUnits, 1)); // all INACTIVE
    if (state == nullptr) {
        VLOG(REPORT, "MEMORY-MAP: failed to allocate state array (%zu bytes)", totalUnits);
        return;
    }

    // --- Step 1: Mark dirty/released free blocks from FreeRegionManager trees ---
    // This uses the MarkFreeBlockUnits helper that recursively traverses the CartesianTree.
    freeRegionManager.MarkFreeUnits(state, totalUnits, S_FREE_DIRTY, S_FREE_RELEASED);
    if (losManager != nullptr) {
        losManager->GetFreeRegionManager().MarkFreeUnits(state, totalUnits, S_FREE_DIRTY, S_FREE_RELEASED);
    }

    // --- Step 2: Walk all units and mark used regions ---
    // For each head unit (SMALL_SIZED_UNITS or LARGE_SIZED_UNITS), mark its entire
    // unit range with the region's type. This overwrites any stale tree marking.
    for (size_t i = 0; i < totalUnits; ) {
        RegionInfo* region = RegionInfo::GetRegionInfo(static_cast<uint32_t>(i));
        auto role = region->GetUnitRole();

        if ((role == RegionInfo::UnitRole::SMALL_SIZED_UNITS ||
             role == RegionInfo::UnitRole::LARGE_SIZED_UNITS) &&
            region->GetUnitIdx() == i) {
            // This is a head unit — mark the entire region
            size_t nUnits = region->GetUnitCount();
            uint8_t val = S_USED_BASE + static_cast<uint8_t>(region->GetRegionType());
            if (region->GetRawPointerObjectCount() > 0) {
                val |= S_RAW_PTR_FLAG;
            }
            for (size_t j = 0; j < nUnits && (i + j) < totalUnits; j++) {
                state[i + j] = val;
            }
            i += nUnits;
        } else {
            // FREE_UNITS or SUBORDINATE_UNIT: advance one at a time.
            // (Free units were already marked by step 1; subordinates are handled
            //  as part of their head unit's block.)
            i++;
        }
    }

    // --- Step 3: Classify remaining INACTIVE units ---
    // Units still S_INACTIVE are either truly inactive (beyond the active frontier)
    // or are in an active zone but weren't in any tree (shouldn't normally happen).
    // Also mark unborrowed RESERVE space.
    for (size_t i = 0; i < totalUnits; i++) {
        if (state[i] != S_INACTIVE) {
            continue;
        }

        uintptr_t addr = RegionInfo::GetUnitAddress(i);

        // RESERVE zone: unborrowed space between watermarks
        if (i >= reserveUnitStart && i < reserveUnitEnd) {
            bool isFreeReserve = false;
            if (mainWm > 0 && losWm > 0) {
                isFreeReserve = (addr >= mainWm && addr < losWm);
            } else if (mainWm == 0 && losWm == 0) {
                isFreeReserve = true; // never borrowed
            } else if (mainWm > 0) {
                isFreeReserve = (addr >= mainWm);
            } else {
                isFreeReserve = (addr < losWm);
            }
            if (isFreeReserve) {
                state[i] = S_RESERVE_FREE;
            }
            continue;
        }

        // MAIN zone: below inactiveZone = active, should have been in a tree
        if (i < mainUnitEnd && addr < mainInactive) {
            state[i] = S_FREE_UNKNOWN;
            continue;
        }

        // LOS zone: above losInactiveZone = active
        if (losManager != nullptr && i >= losUnitStart && i < losUnitEnd && addr >= losInactive) {
            state[i] = S_FREE_UNKNOWN;
            continue;
        }

        // Else: truly inactive, stays S_INACTIVE
    }

    // ======== Output ========

    VLOG(REPORT, "=== HEAP MEMORY MAP ===");

    // Header: total size, unit size
    size_t totalBytes = totalUnits * RegionInfo::UNIT_SIZE;
    VLOG(REPORT, "MEMORY-MAP: %zu units (%zu.%zu MB), UNIT_SIZE=%zu B",
         totalUnits, totalBytes / (1024 * 1024), (totalBytes * 10 / (1024 * 1024)) % 10,
         RegionInfo::UNIT_SIZE);

    // Space boundaries
    VLOG(REPORT, "MEMORY-MAP: MAIN [0, %zu)  RESERVE [%zu, %zu)  LOS [%zu, %zu)",
         mainUnitEnd, reserveUnitStart, reserveUnitEnd, losUnitStart, losUnitEnd);

    // Active zone watermarks
    VLOG(REPORT, "MEMORY-MAP: MAIN active=[0, %zu)  LOS active=[%zu, %zu)",
         mainInactiveUnit, losInactiveUnit, losUnitEnd);
    if (reserveManager != nullptr) {
        size_t mwUnit = (mainWm >= regionHeapStart)
            ? (mainWm - regionHeapStart) / RegionInfo::UNIT_SIZE : reserveUnitStart;
        size_t lwUnit = (losWm >= regionHeapStart)
            ? (losWm - regionHeapStart) / RegionInfo::UNIT_SIZE : reserveUnitEnd;
        VLOG(REPORT, "MEMORY-MAP: RESERVE main_watermark=%zu  los_watermark=%zu", mwUnit, lwUnit);
    }

    VLOG(REPORT, "MEMORY-MAP: legend: T=ThreadLocal R=RecentFull F=From f=LoneFrom U=Unmovable "
         "O=To P=Pinned p=RecentPin X=RawPtr L=Large l=RecentLarge G=Garbage "
         "d=FreeDirty r=FreeReleased ?=FreeUnknown ==ReserveFree _=Inactive");
    VLOG(REPORT, "MEMORY-MAP: cell format [units x type_char], 10 blocks per row");

    // --- Walk state array per space, consolidate consecutive same-state into blocks ---
    // Output 10 compact block cells per line: [  8 T] [  8 R] [128 _] ...
    struct SpaceRange {
        const char* name;
        size_t start;
        size_t end;
    };
    SpaceRange spaces[3];
    size_t nSpaces = 0;
    spaces[nSpaces++] = {"MAIN", 0, mainUnitEnd};
    if (reserveUnitStart < reserveUnitEnd) {
        spaces[nSpaces++] = {"RESERVE", reserveUnitStart, reserveUnitEnd};
    }
    if (losUnitStart < losUnitEnd) {
        spaces[nSpaces++] = {"LOS", losUnitStart, losUnitEnd};
    }

    // Helper: decode state byte into a type character
    auto stateToChar = [&](uint8_t st) -> char {
        if (st == S_INACTIVE)       return '_';
        if (st == S_FREE_DIRTY)     return 'd';
        if (st == S_FREE_RELEASED)  return 'r';
        if (st == S_FREE_UNKNOWN)   return '?';
        if (st == S_RESERVE_FREE)   return '=';
        uint8_t base = st & static_cast<uint8_t>(~S_RAW_PTR_FLAG);
        bool rawPtr = (st & S_RAW_PTR_FLAG) != 0;
        auto rtype = static_cast<RegionInfo::RegionType>(base - S_USED_BASE);
        return RegionTypeToChar(rtype, rawPtr);
    };

    // Line buffer: accumulate up to 10 cells per line.
    // Each cell: "[%4zu %c] " = max 9 chars, 10 cells + padding + null < 128.
    static constexpr size_t CELLS_PER_LINE = 10;
    char lineBuf[256];
    size_t linePos = 0;
    size_t cellCount = 0;

    // Lambda to flush the current line buffer.
    auto flushLine = [&]() {
        if (linePos > 0) {
            lineBuf[linePos] = '\0';
            VLOG(REPORT, "  %s", lineBuf);
            linePos = 0;
            cellCount = 0;
        }
    };

    // Lambda to append one block cell to the line buffer.
    auto appendCell = [&](size_t unitStart, size_t unitCount, char typeChar) {
        // Format: [startIdx +count char]   e.g. [  120 +    8 T]
        int n = snprintf(lineBuf + linePos, sizeof(lineBuf) - linePos,
                         "[%5zu +%4zu %c] ", unitStart, unitCount, typeChar);
        if (n > 0) {
            linePos += static_cast<size_t>(n);
        }
        cellCount++;
        if (cellCount >= CELLS_PER_LINE) {
            flushLine();
        }
    };

    for (size_t s = 0; s < nSpaces; s++) {
        size_t sStart = spaces[s].start;
        size_t sEnd = spaces[s].end;
        if (sStart >= sEnd) {
            continue;
        }

        // Flush any pending cells from previous space before printing separator
        flushLine();
        VLOG(REPORT, "--- %s (units %zu - %zu) ---", spaces[s].name, sStart, sEnd - 1);

        size_t blockStart = sStart;
        uint8_t blockState = state[sStart];

        for (size_t i = sStart + 1; i <= sEnd; i++) {
            uint8_t curState = (i < sEnd) ? state[i] : 0xFF; // sentinel to flush last block
            if (curState == blockState && i < sEnd) {
                continue;
            }

            // Emit block [blockStart, i)
            size_t blockCount = i - blockStart;
            char typeChar = stateToChar(blockState);
            appendCell(blockStart, blockCount, typeChar);

            blockStart = i;
            blockState = curState;
        }
    }

    // Flush any remaining cells
    flushLine();

    VLOG(REPORT, "=== END MEMORY MAP ===");

    free(state);
}

RegionInfo* RegionManager::AllocateThreadLocalRegion(bool expectPhysicalMem)
{
    RegionInfo* region = TakeRegion(maxUnitCountPerRegion, RegionInfo::UnitRole::SMALL_SIZED_UNITS, expectPhysicalMem);
    if (region != nullptr) {
        {
            GCPhase phase = Heap::GetHeap().GetCollector().GetGCPhase();
            if (phase == GC_PHASE_TRACE || phase == GC_PHASE_CLEAR_SATB_BUFFER) {
                region->SetTraceRegionFlag(1);
            }
            tlRegionList.PrependRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION);
            DLOG(REGION, "alloc tl-region %p @[0x%zx+%zu, 0x%zx) units[%zu+%zu, %zu) type %u",
                region, region->GetRegionStart(), region->GetRegionSize(), region->GetRegionEnd(),
                region->GetUnitIdx(), region->GetUnitCount(), region->GetUnitIdx() + region->GetUnitCount(),
                region->GetRegionType());
        }
    }

    return region;
}

void RegionManager::RequestForRegion(size_t size)
{
    if (IsGcThread()) {
        // gc thread is always permitted for allocation.
        return;
    }

    Heap& heap = Heap::GetHeap();
    GCStats& gcstats = heap.GetCollector().GetGCStats();
    size_t allocatedBytes = GetAllocatedSize() - gcstats.liveBytesAfterGC;
    constexpr double pi = 3.14;
    size_t availableBytesAfterGC = heap.GetMaxCapacity() - gcstats.liveBytesAfterGC;
    double heuAllocRate = std::cos((pi / 2.0) * allocatedBytes / availableBytesAfterGC) * gcstats.collectionRate;
    // for maximum performance, choose the larger one.
    double allocRate = std::max(
        static_cast<double>(CangjieRuntime::GetHeapParam().allocationRate) * MB / SECOND_TO_NANO_SECOND, heuAllocRate);
    size_t waitTime = static_cast<size_t>(size / allocRate);
    uint64_t now = TimeUtil::NanoSeconds();
    if (prevRegionAllocTime + waitTime <= now) {
        prevRegionAllocTime = TimeUtil::NanoSeconds();
        return;
    }

    uint64_t sleepTime = std::min<uint64_t>(CangjieRuntime::GetHeapParam().allocationWaitTime,
                                  prevRegionAllocTime + waitTime - now);
    DLOG(ALLOC, "wait %zu ns to alloc %zu(B)", sleepTime, size);
    std::this_thread::sleep_for(std::chrono::nanoseconds{ sleepTime });
    prevRegionAllocTime = TimeUtil::NanoSeconds();
}

bool RegionManager::RouteOrCompactRegionImpl(RegionInfo* region)
{
    CHECK(region->IsRoutingState());
    CHECK_DETAIL(region->GetRawPointerObjectCount() <= 0, "pinned region shouldn't be moved");
    size_t fromBytes = region->GetLiveByteCount();
    AllocBuffer* buffer = AllocBuffer::GetOrCreateAllocBuffer();
    RegionInfo* toRegion1 = buffer->GetRegion();
    CHECK(region != toRegion1);
    bool result;
    if (toRegion1 == RegionInfo::NullRegion()) {
        toRegion1 = AllocateThreadLocalRegion();
        if (toRegion1 == nullptr) {
            CompactRegion(region);
            toRegion1 = region;
            result = false;
        } else {
            toRegion1->Alloc(fromBytes);
            result = true;
        }
        buffer->SetRegion(toRegion1);
        size_t toRegion1Start = toRegion1->GetRegionStart();
        region->SetRouteInfo(toRegion1Start, fromBytes);
        DLOG(FORWARD, "route region %p@[%#zx+%zu, %#zx) => %p@[%#zx~%#zx, %#zx)",
            region, region->GetRegionStart(), fromBytes, region->GetRegionEnd(), toRegion1,
            toRegion1Start, toRegion1Start + fromBytes, toRegion1->GetRegionEnd());
        return result;
    }

    size_t toRegion1Capacity = toRegion1->GetAvailableSize();
    MAddress toRegion1Addr = toRegion1->GetRegionAllocPtr();
    if (fromBytes <= toRegion1Capacity) {
        toRegion1->Alloc(fromBytes);
        region->SetRouteInfo(toRegion1Addr, fromBytes);
        DLOG(FORWARD, "route region %p@[%#zx+%zu, %#zx) => %p@[%#zx, %#zx~%#zx, %#zx)",
            region, region->GetRegionStart(), fromBytes, region->GetRegionEnd(), toRegion1,
            toRegion1->GetRegionStart(), toRegion1Addr, toRegion1Addr + fromBytes, toRegion1->GetRegionEnd());
        return true;
    }
    size_t toRegion1Waste = toRegion1Capacity;
    BaseObject* leftObject = nullptr;
    (void)region->VisitLiveObjectsUntilFalse([&toRegion1Waste, &leftObject](BaseObject* obj) {
        size_t objSz = RegionSpace::GetAllocSize(*obj);
        if (toRegion1Waste >= objSz) {
            toRegion1Waste -= objSz;
            return true;
        } else {
            leftObject = obj;
            return false;
        }
    });
    MAddress usedBytes1 = toRegion1Capacity - toRegion1Waste;
    MAddress usedBytes2 = fromBytes - usedBytes1;
    CHECK(toRegion1->IsThreadLocalRegion());
    {
        RemoveThreadLocalRegion(toRegion1);
        EnlistFullThreadLocalRegion(toRegion1);
    }

    RegionInfo* toRegion2 = AllocateThreadLocalRegion();
    CHECK(region != toRegion2);
    if (toRegion2 != nullptr) {
        toRegion1->Alloc(usedBytes1);
        CHECK(toRegion2->Alloc(usedBytes2) != 0);
        result = true;
    } else {
        CompactRegion(region, toRegion1);
        toRegion2 = region; // region is partially compacted into itself.
        result = false;
    }
    buffer->SetRegion(toRegion2);
    uint32_t toRegion2Idx = toRegion2->GetUnitIdx();
    region->SetRouteInfo(toRegion1Addr, usedBytes1, toRegion2Idx);
    DLOG(FORWARD, "route region %p@[%#zx+%zu, %#zx) => %p@[%#zx, %#zx~%#zx, %#zx) & %p@[%#zx~%#zx, %#zx)", region,
        region->GetRegionStart(), fromBytes, region->GetRegionEnd(), toRegion1, toRegion1->GetRegionStart(),
        toRegion1Addr, toRegion1Addr + usedBytes1, toRegion1->GetRegionEnd(), toRegion2,
        toRegion2->GetRegionStart(), toRegion2->GetRegionStart() + usedBytes2, toRegion2->GetRegionEnd());
    return result;
}

void RegionManager::CompactRegion(RegionInfo* region)
{
    MAddress regionStart = region->GetRegionStart();
    DLOG(REGION, "compact region %p@[%#zx+%zu, %#zx) type %u", region, regionStart,
        region->GetLiveByteCount(), region->GetRegionEnd(), region->GetRegionType());
    MAddress regionLimit = region->GetRegionAllocPtr();
    region->SetRegionAllocPtr(regionStart);
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    for (MAddress currentPtr = regionStart; currentPtr < regionLimit;) {
        BaseObject* currentObj = reinterpret_cast<BaseObject*>(currentPtr);
        size_t size = currentObj->GetSize();
        size_t offset = currentPtr - regionStart;
        if (region->IsSurvivedObject(offset)) {
            MAddress toAddress = region->Alloc(size);
            BaseObject* toObj = reinterpret_cast<BaseObject*>(toAddress);
            DLOG(FORWARD, "compact obj %p<%p>(%zu) to %p", currentObj, currentObj->GetTypeInfo(), size, toObj);
            collector.CopyObject(*currentObj, *toObj, size);
            toObj->SetStateCode(ObjectState::NORMAL);
        }
        currentPtr += size;
    }
    std::atomic_thread_fence(std::memory_order_release);

    // clear unused space which is free after compaction.
    MAddress cur = region->GetRegionAllocPtr();
    if (regionLimit > cur) {
        size_t reclaimSize = regionLimit - cur;
        CHECK_DETAIL(memset_s(reinterpret_cast<void*>(cur), reclaimSize, 0, reclaimSize) == EOK, "clear buffer failed");
    }

    if (region->IsFromRegion()) {
        fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
            RegionInfo::RegionType::THREAD_LOCAL_REGION);
    }
    tlRegionList.PrependRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION);
}

void RegionManager::CompactRegion(RegionInfo* region, RegionInfo* toRegion1)
{
    MAddress regionStart = region->GetRegionStart();
    DLOG(REGION, "compact region %p@[%#zx+%zu, %#zx) type %u to region %p@%#zx:%#zx",
        region, regionStart, region->GetLiveByteCount(), region->GetRegionEnd(), region->GetRegionType(),
        toRegion1, toRegion1->GetRegionStart(), toRegion1->GetRegionAllocPtr());
    MAddress currentPtr = regionStart;
    BaseObject* currentObj = reinterpret_cast<BaseObject*>(currentPtr);
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    while (true) {
        CHECK(currentPtr>=regionStart);
        size_t offset = currentPtr - regionStart;
        size_t size = currentObj->GetSize();
        if (region->IsSurvivedObject(offset)) {
            MAddress toAddress = toRegion1->Alloc(size);
            if (toAddress == 0) {
                break;
            }
            BaseObject* toObj = reinterpret_cast<BaseObject*>(toAddress);
            DLOG(FORWARD, "compact obj %p<%p>(%zu) to %p", currentObj, currentObj->GetTypeInfo(), size, toObj);
            collector.CopyObject(*currentObj, *toObj, size);
            toObj->SetStateCode(ObjectState::NORMAL);
            std::atomic_thread_fence(std::memory_order_release);
        }
        currentPtr += size;
        currentObj = reinterpret_cast<BaseObject*>(currentPtr);
    };

    MAddress regionLimit = region->GetRegionAllocPtr();
    region->SetRegionAllocPtr(regionStart);
    while (currentPtr < regionLimit) {
        CHECK(currentPtr >= regionStart);
        size_t offset = currentPtr - regionStart;
        BaseObject* currentObj = reinterpret_cast<BaseObject*>(currentPtr);
        size_t size = currentObj->GetSize();
        if (region->IsSurvivedObject(offset)) {
            MAddress toAddress = region->Alloc(size);
            BaseObject* toObj = reinterpret_cast<BaseObject*>(toAddress);
            DLOG(FORWARD, "compact obj %p<%p>(%zu) to %p", currentObj, currentObj->GetTypeInfo(), size, toObj);
            collector.CopyObject(*currentObj, *toObj, size);
            toObj->SetStateCode(ObjectState::NORMAL);
            std::atomic_thread_fence(std::memory_order_release);
        }
        currentPtr += size;
    }

    // clear unused space which is free after compaction.
    MAddress cur = region->GetRegionAllocPtr();
    if (regionLimit > cur) {
        size_t reclaimSize = regionLimit - cur;
        CHECK_DETAIL(memset_s(reinterpret_cast<void*>(cur), reclaimSize, 0, reclaimSize) == EOK, "clear buffer failed");
    }

    if (region->IsFromRegion()) {
        fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
            RegionInfo::RegionType::THREAD_LOCAL_REGION);
    }
    tlRegionList.PrependRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION);
}

void RegionManager::ForwardRegion(RegionInfo* region)
{
    CHECK_DETAIL(region->IsFromRegion() || region->IsLoneFromRegion() || (region->IsThreadLocalRegion() &&
        (region->IsRoutingState() || region->IsCompacted())), "region type %u", region->GetRegionType());

    DLOG(FORWARD, "try forward region %p @[0x%zx+%zu, 0x%zx) type %u, live bytes %u",
        region, region->GetRegionStart(), region->GetRegionAllocatedSize(), region->GetRegionEnd(),
        region->GetRegionType(), region->GetLiveByteCount());

    if (region->GetLiveByteCount() == 0) {
        CollectRegion(region);
        return;
    }

    if (!RouteRegion(region)) {
        return;
    }

    int32_t rawPointerCount = region->GetRawPointerObjectCount();
    CHECK(rawPointerCount == 0);
    Collector& collector = Heap::GetHeap().GetCollector();
    bool forwarded = region->VisitLiveObjectsUntilFalse(
        [&collector](BaseObject* obj) { return collector.ForwardObject(obj); });

    CHECK(forwarded);
    {
        region->SetRouteState(RegionInfo::RouteState::FORWARDED);
        CollectRegion(region);
    }
}

uintptr_t RegionManager::AllocPinnedFromFreeList(size_t size)
{
    std::lock_guard<std::mutex> lock(freePinnedSlotListMutex);
    GCPhase mutatorPhase = Mutator::GetMutator()->GetMutatorPhase();
    // For preventing missing mark, do not allocate object from slot list when gc phase is post trace.
    if (mutatorPhase == GCPhase::GC_PHASE_POST_TRACE) {
        return 0;
    }
    uintptr_t allocPtr = freePinnedSlotLists.PopFront(size);
    // For making bitmap comform with live object count, do not mark object repeated.
    if (allocPtr == 0 ||
        (mutatorPhase != GCPhase::GC_PHASE_ENUM &&
        mutatorPhase != GCPhase::GC_PHASE_TRACE &&
        mutatorPhase != GCPhase::GC_PHASE_CLEAR_SATB_BUFFER)) {
        return allocPtr;
    }
 
    // Mark new allocated pinned object.
    BaseObject* object = reinterpret_cast<BaseObject*>(allocPtr);
    (reinterpret_cast<CopyCollector*>(&Heap::GetHeap().GetCollector()))->MarkObject(object);
    return allocPtr;
}

// M4b: Shrink inactiveZone to reclaim free space at the high end of MAIN.
// After GC with low-address-first allocation, live regions cluster near regionHeapStart,
// and free regions accumulate near inactiveZone. This function:
//   1. Scans the active zone to find the highest live region end.
//   2. Rebuilds freeRegionManager with only chunks below that boundary.
//   3. Lowers inactiveZone to release high-end free space.
// Called during STW after every GC's CollectSmallSpace.
size_t RegionManager::ShrinkInactiveZone()
{
    uintptr_t oldInactive = inactiveZone.load();
    if (oldInactive <= regionHeapStart) {
        return 0;
    }

    // Pass 1: Scan active zone to find highest live region end.
    uintptr_t highestLiveEnd = regionHeapStart;
    for (uintptr_t addr = regionHeapStart; addr < oldInactive;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(addr);
        uintptr_t end = region->GetRegionEnd();
        if (region->IsValidRegion() && !region->IsFreeRegion() && !region->IsGarbageRegion()) {
            highestLiveEnd = end;
        }
        addr = end;
    }

    if (highestLiveEnd >= oldInactive) {
        return 0; // No free space at high end
    }

    // Pass 2: Rebuild freeRegionManager with only free chunks below highestLiveEnd.
    freeRegionManager.ClearTrees();
    for (uintptr_t addr = regionHeapStart; addr < highestLiveEnd;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(addr);
        uintptr_t end = region->GetRegionEnd();
        if (region->IsFreeRegion()) {
            freeRegionManager.AddGarbageUnits(region->GetUnitIdx(), region->GetUnitCount());
        }
        addr = end;
    }

    // Shrink inactiveZone.
    inactiveZone.store(highestLiveEnd, std::memory_order_release);

    size_t reclaimedBytes = oldInactive - highestLiveEnd;
    VLOG(REPORT, "ShrinkInactiveZone: 0x%zx -> 0x%zx, reclaimed %zu bytes (%zu units)",
         oldInactive, highestLiveEnd, reclaimedBytes, reclaimedBytes / RegionInfo::UNIT_SIZE);
    return reclaimedBytes;
}

// M5: Return RESERVE-borrowed free MAIN regions to RESERVE.
// Dead RESERVE-borrowed regions are intercepted in ReclaimRegion/ReleaseRegion and
// stored in pendingReserveReturns instead of freeRegionManager.
// With hole bitmap, ReturnFromMain always succeeds (marks bitmap + coalesces watermark).
size_t RegionManager::ReturnBorrowedToReserve()
{
    // Lock to safely read/clear pendingReserveReturns; producers (ReclaimRegion/ReleaseRegion)
    // may push_back concurrently from mutator or FP threads.
    std::lock_guard<std::mutex> lock(pendingReserveReturnsMutex);
    if (reserveManager == nullptr || pendingReserveReturns.empty()) {
        return 0;
    }

    // M5 hysteresis: skip proactive return if MAIN return is in cooldown
    // (unless there are many pending returns, suggesting heavy churn)
    if (reserveManager->IsMainReturnInCooldown() && pendingReserveReturns.size() < 4) {
        // Cooldown active — put pending returns into freeRegionManager for reuse
        for (auto& entry : pendingReserveReturns) {
            freeRegionManager.AddGarbageUnits(entry.first, entry.second);
        }
        DLOG(REGION, "MAIN ReturnBorrowedToReserve: cooldown, %zu entries kept in freeRegionManager",
             pendingReserveReturns.size());
        pendingReserveReturns.clear();
        return 0;
    }

    // With hole bitmap, no sorting needed — ReturnFromMain accepts any position.
    // Bitmap records the return; watermark coalesces automatically when contiguous.
    size_t returnedBytes = 0;
    for (auto& entry : pendingReserveReturns) {
        uintptr_t addr = RegionInfo::GetUnitAddress(entry.first);
        size_t size = entry.second * RegionInfo::UNIT_SIZE;
        reserveManager->ReturnFromMain(addr, size);
        returnedBytes += size;
        DLOG(REGION, "MAIN ReturnBorrowedToReserve: returned %zu bytes @0x%zx", size, addr);
    }

    pendingReserveReturns.clear();

    if (returnedBytes > 0) {
        DLOG(REPORT, "MAIN ReturnBorrowedToReserve: returned %zu bytes total to RESERVE", returnedBytes);
    }
    return returnedBytes;
}

} // namespace MapleRuntime

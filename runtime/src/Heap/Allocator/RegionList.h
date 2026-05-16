// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_REGION_LIST_H
#define MRT_REGION_LIST_H

#include <cstdio>
#include <time.h>
#include <unistd.h>

#include "RegionInfo.h"
#include "Heap/Heap.h"
#include "Mutator/ThreadLocal.h"

namespace MapleRuntime {
static inline void RegionListWriteProbe(const char* msg, const void* list, const void* region)
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ThreadLocalData* tl = ThreadLocal::GetThreadLocalData();
    GCPhase gcPhase = Heap::GetHeap().GetGCPhase();
    int threadType = tl == nullptr ? -1 : static_cast<int>(tl->threadType);
    std::printf("[%lld.%06ld] %s list=%p region=%p tl=%p mutator=%p buffer=%p threadType=%d cjthread=%p schedule=%p tid=%llu gcPhase=%d\n",
        static_cast<long long>(ts.tv_sec), ts.tv_nsec / 1000, msg, list, region, tl,
        tl == nullptr ? nullptr : tl->mutator, tl == nullptr ? nullptr : tl->buffer,
        threadType, tl == nullptr ? nullptr : tl->cjthread,
        tl == nullptr ? nullptr : tl->schedule,
        static_cast<unsigned long long>(tl == nullptr ? 0 : tl->tid), static_cast<int>(gcPhase));
    if (threadType == 0 && gcPhase != GCPhase::GC_PHASE_IDLE) {
        std::printf("[%lld.%06ld] HOUDINI_RACE mutator_write_during_gc list=%p region=%p tl=%p mutator=%p buffer=%p threadType=%d cjthread=%p schedule=%p tid=%llu gcPhase=%d\n",
            static_cast<long long>(ts.tv_sec), ts.tv_nsec / 1000, list, region, tl,
            tl == nullptr ? nullptr : tl->mutator, tl == nullptr ? nullptr : tl->buffer,
            threadType, tl == nullptr ? nullptr : tl->cjthread,
            tl == nullptr ? nullptr : tl->schedule,
            static_cast<unsigned long long>(tl == nullptr ? 0 : tl->tid), static_cast<int>(gcPhase));
    }
    std::fflush(stdout);
}

class RegionList {
public:
    friend void RemoveRegionLocked(RegionList*, RegionInfo*);
    RegionList(const char* name) : listName(name) {}

    void PrependRegion(RegionInfo* region, RegionInfo::RegionType type);
    void PrependRegionLocked(RegionInfo* region, RegionInfo::RegionType type);

    void MergeRegionList(RegionList& regionList, RegionInfo::RegionType regionType);

    void DeleteRegion(RegionInfo* del)
    {
        if (del == nullptr) {
            return;
        }

        RegionListWriteProbe("HOUDINI_WRITE DeleteRegion.before_lock", this, del);
        std::lock_guard<std::mutex> lock(listMutex);
        RegionListWriteProbe("HOUDINI_WRITE DeleteRegion.after_lock", this, del);
        DeleteRegionLocked(del);
        RegionListWriteProbe("HOUDINI_WRITE DeleteRegion.after_delete_locked", this, del);
    }

    bool TryDeleteRegion(RegionInfo* del, RegionInfo::RegionType oldType, RegionInfo::RegionType newType)
    {
        if (del == nullptr) {
            return false;
        }

        CHECK(oldType != newType);
        RegionListWriteProbe("HOUDINI_WRITE TryDeleteRegion.before_lock", this, del);
        std::lock_guard<std::mutex> lock(listMutex);
        RegionListWriteProbe("HOUDINI_WRITE TryDeleteRegion.after_lock", this, del);
        if (del->GetRegionType() == oldType) {
            DeleteRegionLocked(del);
            del->SetRegionType(newType);
            RegionListWriteProbe("HOUDINI_WRITE TryDeleteRegion.success", this, del);
            return true;
        }
        RegionListWriteProbe("HOUDINI_WRITE TryDeleteRegion.noop", this, del);
        return false;
    }

#ifdef MRT_DEBUG
    void DumpRegionList(const char*);
#endif

    void DecCounts(size_t nRegion, size_t nUnit)
    {
        if (regionCount >= nRegion && unitCount >= nUnit) {
            regionCount -= nRegion;
            unitCount -= nUnit;
        } else {
            LOG(RTLOG_FATAL, "region list %p error count %zu-%zu %zu-%zu", regionCount, nRegion, unitCount, nUnit);
        }
    }

    void IncCounts(size_t nRegion, size_t nUnit)
    {
        CHECK((nRegion <= std::numeric_limits<size_t>::max() - regionCount) &&
              (nUnit <= std::numeric_limits<size_t>::max() - unitCount));
        regionCount += nRegion;
        unitCount += nUnit;
    }

    RegionInfo* GetHeadRegion() const
    {
        LOG(RTLOG_ERROR, "HOUDINI_GETTER GetHeadRegion this=%p listHead=%p listTail=%p regionCount=%zu unitCount=%zu",
            this, listHead, listTail, regionCount, unitCount);
        return listHead;
    }

    void ClearList()
    {
        LOG(RTLOG_ERROR,
            "HOUDINI_CLEARLIST ClearList.enter this=%p listHead=%p listTail=%p regionCount=%zu unitCount=%zu",
            this, listHead, listTail, regionCount, unitCount);
        listHead = nullptr;
        LOG(RTLOG_ERROR, "HOUDINI_CLEARLIST ClearList.after_listHead this=%p listHead=%p", this, listHead);
        listTail = nullptr;
        LOG(RTLOG_ERROR, "HOUDINI_CLEARLIST ClearList.after_listTail this=%p listTail=%p", this, listTail);
        regionCount = 0;
        LOG(RTLOG_ERROR, "HOUDINI_CLEARLIST ClearList.after_regionCount this=%p regionCount=%zu", this, regionCount);
        unitCount = 0;
        LOG(RTLOG_ERROR, "HOUDINI_CLEARLIST ClearList.after_unitCount this=%p unitCount=%zu", this, unitCount);
    }

    RegionInfo* GetTailRegion()
    {
        LOG(RTLOG_ERROR, "HOUDINI_GETTER GetTailRegion this=%p listHead=%p listTail=%p regionCount=%zu unitCount=%zu",
            this, listHead, listTail, regionCount, unitCount);
        return listTail;
    }

    RegionInfo* TakeHeadRegion()
    {
        std::lock_guard<std::mutex> lg(listMutex);
        if (listHead == nullptr) { return nullptr; }
        RegionInfo* currentHead = listHead;
        DeleteRegionLocked(currentHead);
        return currentHead;
    }

    RegionInfo* TakeHeadRegion(RegionInfo::RegionType newType)
    {
        std::lock_guard<std::mutex> lg(listMutex);
        if (listHead == nullptr) { return nullptr; }
        RegionInfo* currentHead = listHead;
        DeleteRegionLocked(currentHead);
        currentHead->SetRegionType(newType);
        return currentHead;
    }

    size_t GetUnitCount() const { return unitCount; }

    size_t GetRegionCount() const { return regionCount; }

    size_t GetAllocatedSize(bool count = false) const
    {
        if (!count) {
            return GetUnitCount() * RegionInfo::UNIT_SIZE;
        }
        return CountAllocatedSize();
    }

    void VisitAllRegions(const std::function<void(RegionInfo*)>& visitor)
    {
        std::lock_guard<std::mutex> lock(listMutex);
        RegionInfo* node = listHead;
        RegionInfo* next = node;
        while (node != nullptr) {
            next = node->GetNextRegion();
            visitor(node);
            node = next;
        }
    }

    void VisitAllGhostRegions(const std::function<void(RegionInfo*)>& visitor)
    {
        for (RegionInfo* node = listHead; node != nullptr; node = node->GetNextGhostRegion()) {
            visitor(node);
        }
    }

    void SetElementType(RegionInfo::RegionType type)
    {
        std::lock_guard<std::mutex> lock(listMutex);
        for (RegionInfo* node = listHead; node != nullptr; node = node->GetNextRegion()) {
            node->SetRegionType(type);
        }
    }

    void ClearTraceRegionFlag()
    {
        std::lock_guard<std::mutex> lock(listMutex);
        for (RegionInfo *node = listHead; node != nullptr; node = node->GetNextRegion()) {
            node->SetTraceRegionFlag(0);
        }
    }

    std::mutex& GetListMutex() { return listMutex; }

    void MoveTo(RegionList& targetList)
    {
        RegionListWriteProbe("HOUDINI_WRITE MoveTo.before_lock", this, &targetList);
        (void)::write(1, "HOUDINI_WRITE MoveTo.enter_raw\n", sizeof("HOUDINI_WRITE MoveTo.enter_raw\n") - 1);
        LOG(RTLOG_ERROR, "HOUDINI_MOVETO MoveTo.enter this=%p targetList=%p listHead=%p listTail=%p regionCount=%zu unitCount=%zu",
            this, &targetList, listHead, listTail, regionCount, unitCount);
        std::lock_guard<std::mutex> lock(listMutex);
        RegionListWriteProbe("HOUDINI_WRITE MoveTo.after_lock", this, &targetList);
        LOG(RTLOG_ERROR, "HOUDINI_MOVETO MoveTo.after_lock this=%p targetList=%p", this, &targetList);
        targetList.AssignWith(*this);
        RegionListWriteProbe("HOUDINI_WRITE MoveTo.after_assign_with", this, &targetList);
        LOG(RTLOG_ERROR, "HOUDINI_MOVETO MoveTo.after_AssignWith this=%p targetList=%p this->listHead=%p this->listTail=%p",
            this, &targetList, listHead, listTail);
        this->ClearList();
        RegionListWriteProbe("HOUDINI_WRITE MoveTo.after_clear_list", this, &targetList);
        LOG(RTLOG_ERROR, "HOUDINI_MOVETO MoveTo.after_ClearList this=%p this->listHead=%p this->listTail=%p",
            this, listHead, listTail);
    }

    void CopyListTo(RegionList& dstList)
    {
        std::lock_guard<std::mutex> lock(listMutex);
        dstList.listHead = this->listHead;
        dstList.listTail = this->listTail;
        dstList.regionCount = this->regionCount;
        dstList.unitCount = this->unitCount;
    }

protected:
    std::mutex listMutex;
    size_t regionCount = 0;
    size_t unitCount = 0;
    RegionInfo* listHead = nullptr; // the start region for iteration, i.e., the first region
    RegionInfo* listTail = nullptr; // help to merge region list
    const char* listName = nullptr;
private:
    void DeleteRegionLocked(RegionInfo* del);

    void AssignWith(const RegionList& srcList)
    {
        RegionListWriteProbe("HOUDINI_WRITE AssignWith.before_lock", this, &srcList);
        LOG(RTLOG_ERROR, "HOUDINI_ASSIGN AssignWith.enter this=%p srcList=%p srcList.listHead=%p srcList.listTail=%p srcList.regionCount=%zu srcList.unitCount=%zu",
            this, &srcList, srcList.listHead, srcList.listTail, srcList.regionCount, srcList.unitCount);
        std::lock_guard<std::mutex> lock(listMutex);
        RegionListWriteProbe("HOUDINI_WRITE AssignWith.after_lock", this, &srcList);
        LOG(RTLOG_ERROR, "HOUDINI_ASSIGN AssignWith.after_lock this=%p srcList=%p", this, &srcList);
        listHead = srcList.listHead;
        LOG(RTLOG_ERROR, "HOUDINI_ASSIGN AssignWith.after_listHead this=%p listHead=%p", this, listHead);
        listTail = srcList.listTail;
        LOG(RTLOG_ERROR, "HOUDINI_ASSIGN AssignWith.after_listTail this=%p listTail=%p", this, listTail);
        regionCount = srcList.regionCount;
        LOG(RTLOG_ERROR, "HOUDINI_ASSIGN AssignWith.after_regionCount this=%p regionCount=%zu", this, regionCount);
        unitCount = srcList.unitCount;
        LOG(RTLOG_ERROR, "HOUDINI_ASSIGN AssignWith.after_unitCount this=%p unitCount=%zu", this, unitCount);
        RegionListWriteProbe("HOUDINI_WRITE AssignWith.after_copy", this, &srcList);
    }

    // allocated-size of to-region list must be calculated on the fly.
    size_t CountAllocatedSize() const
    {
        size_t allocCnt = 0;
        std::lock_guard<std::mutex> lock(const_cast<RegionList*>(this)->listMutex);
        for (RegionInfo* region = listHead; region != nullptr; region = region->GetNextRegion()) {
            allocCnt += region->GetRegionAllocatedSize();
        }
        return allocCnt;
    }

#ifdef MRT_DEBUG
    void VerifyRegion(RegionInfo* region)
    {
        RegionInfo* prev = region->GetPrevRegion();
        RegionInfo* next = region->GetNextRegion();
        if (prev != nullptr && prev->GetNextRegion() != region) {
            LOG(RTLOG_FATAL, "illegal region node");
        }

        if (next != nullptr && next->GetPrevRegion() != region) {
            LOG(RTLOG_FATAL, "illegal region node");
        }
    }
#endif
};

class RegionCache : public RegionList {
public:
    RegionCache(const char* name) : RegionList(name) {}

    bool TryPrependRegion(RegionInfo *region, RegionInfo::RegionType type)
    {
        std::lock_guard<std::mutex> lock(listMutex);
        if (active) {
            PrependRegionLocked(region, type);
            return true;
        }
        return false;
    }

    void ActivateRegionCache()
    {
        std::lock_guard<std::mutex> lock(listMutex);
        active = true;
    }

    void DeactivateRegionCache()
    {
        std::lock_guard<std::mutex> lock(listMutex);
        for (RegionInfo *node = listHead; node != nullptr; node = node->GetNextRegion()) {
            node->SetTraceRegionFlag(0);
        }
        active = false;
    }
private:
    bool active = false;
};
} // namespace MapleRuntime
#endif // MRT_REGION_LIST_H

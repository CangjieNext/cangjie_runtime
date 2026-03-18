// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_REGION_SPACE_H
#define MRT_REGION_SPACE_H

#include <cassert>
#include <list>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "AllocUtil.h"
#include "Allocator.h"
#include "ExceptionManager.h"
#include "Mutator/Mutator.h"
#include "RegionManager.h"
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

#ifdef MRT_USE_MARK_SWEEP
#include "MSRegionManager.h"
#include "CentralRegionPool.h"
#include "LargeObjectSpace.h"
#include "AllocBufferMS.h"
#endif

namespace MapleRuntime {
// RegionSpace aims to be the API for other components of runtime
// the complication of implementation is delegated to RegionManager
// allocator should not depend on any assumptions on the details of RegionManager
class RegionSpace : public Allocator {
public:
    static size_t ToAllocSize(size_t objSize)
    {
        size_t size = objSize + HEADER_SIZE;
        return RoundUp<size_t>(size, ALLOC_ALIGN);
    }

    static size_t GetAllocSize(const BaseObject& obj)
    {
        size_t objSize = obj.GetSize();
        return ToAllocSize(objSize);
    }

    RegionSpace() = default;
    ATTR_NO_INLINE ~RegionSpace() override
    {
        if (allocBufferManager != nullptr) {
            delete allocBufferManager;
            allocBufferManager = nullptr;
        }
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
        Sanitizer::OnHeapDeallocated(map->GetBaseAddr(), map->GetMappedSize());
#endif
        MemMap::DestroyMemMap(map);
    }

    void Init(const HeapParam&) override;

    MAddress Allocate(size_t size, AllocType allocType) override;

    RegionManager& GetRegionManager() noexcept { return regionManager; }

    MAddress GetSpaceStartAddress() const override { return reservedStart; }

    MAddress GetSpaceEndAddress() const override
    {
#ifdef MRT_USE_MARK_SWEEP
        // In MS mode, committed end may grow dynamically.
        if (msRegionManager_ != nullptr) {
            return msRegionManager_->GetHeapEnd();
        }
#endif
        return reservedEnd;
    }

    size_t GetCurrentCapacity() const override
    {
#ifdef MRT_USE_MARK_SWEEP
        if (msRegionManager_ != nullptr) {
            return msRegionManager_->GetInactiveZone() - reservedStart;
        }
#endif
        return regionManager.GetInactiveZone() - reservedStart;
    }
    size_t GetMaxCapacity() const override
    {
#ifdef MRT_USE_MARK_SWEEP
        // Max capacity = full virtual reservation.
        if (msRegionManager_ != nullptr) {
            return msRegionManager_->GetMaxHeapEnd() - reservedStart;
        }
#endif
        return reservedEnd - reservedStart;
    }

    inline size_t GetRecentAllocatedSize() const
    {
#ifdef MRT_USE_MARK_SWEEP
        return 0; // MS mode: CentralRegionPool manages regions, not RegionManager lists
#else
        return regionManager.GetRecentAllocatedSize();
#endif
    }

    // size of objects survived in previous gc.
    inline size_t GetSurvivedSize() const
    {
#ifdef MRT_USE_MARK_SWEEP
        return 0; // MS mode: no from-space / pinned concepts
#else
        return regionManager.GetSurvivedSize();
#endif
    }

    size_t GetUsedPageSize() const override
    {
#ifdef MRT_USE_MARK_SWEEP
        if (msCentralPool_ != nullptr) {
            return msCentralPool_->GetTotalMSRegionBytes();
        }
        return 0;
#else
        return regionManager.GetUsedRegionSize();
#endif
    }

    inline size_t GetTargetSize() const
    {
        double heapUtilization = CangjieRuntime::GetHeapParam().heapUtilization;
        return static_cast<size_t>(GetUsedPageSize() / heapUtilization);
    }

    size_t AllocatedBytes() const override;

    size_t LargeObjectBytes() const override
    {
#ifdef MRT_USE_MARK_SWEEP
        return 0; // MS mode: large objects tracked via CentralRegionPool
#else
        return regionManager.GetLargeObjectSize();
#endif
    }

    size_t FromSpaceSize() const
    {
#ifdef MRT_USE_MARK_SWEEP
        return 0;
#else
        return regionManager.GetFromSpaceSize();
#endif
    }

    size_t PinnedSpaceSize() const
    {
#ifdef MRT_USE_MARK_SWEEP
        return 0;
#else
        return regionManager.GetPinnedSpaceSize();
#endif
    }

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool IsHeapObject(MAddress addr) const override;
#endif

    // info dump
    void GetInstances(const TypeInfo*, bool, size_t, std::vector<MObject*>&) const {}
    void ClassInstanceNum(std::map<CString, long>&) const {}

    size_t ReclaimGarbageMemory(bool releaseAll) override
    {
#ifdef MRT_USE_MARK_SWEEP
        if (msRegionManager_ == nullptr) {
            return 0;
        }
        if (releaseAll) {
            return msRegionManager_->ReleaseGarbageRegions(0);
        } else {
            size_t dirtyBytes = static_cast<size_t>(msRegionManager_->GetFreeUnitCount()) *
                                msRegionManager_->GetUnitsPerMSRegion() * MS_REGION_SIZE /
                                msRegionManager_->GetUnitsPerMSRegion();
            double cachedRatio = 1 - CangjieRuntime::GetHeapParam().heapUtilization;
            size_t targetCachedSize = static_cast<size_t>(AllocatedBytes() * cachedRatio);
            size_t released = msRegionManager_->ReleaseGarbageRegions(targetCachedSize);
            msRegionManager_->ShrinkHeap();
            return released;
        }
#else
        {
            MRT_PHASE_TIMER("ReclaimGarbageRegions");
            regionManager.ReclaimGarbageRegions();
        }

        MRT_PHASE_TIMER("ReleaseGarbageMemory");
        if (releaseAll) {
            return regionManager.ReleaseGarbageRegions(0);
        } else {
            size_t size = regionManager.GetAllocatedSize();
            double cachedRatio = 1 - CangjieRuntime::GetHeapParam().heapUtilization;
            size_t targetCachedSize = static_cast<size_t>(size * cachedRatio);
            return regionManager.ReleaseGarbageRegions(targetCachedSize);
        }
#endif
    }
#if defined(__EULER__)
    void TryReclaimGarbageMemory() override
    {
        double cachedRatio = regionManager.GetCacheRatio();
        if (cachedRatio == 1.0) { // 1.0 is the default value
            return;
        }
        {
            MRT_PHASE_TIMER("TryReclaimGarbageRegions");
            regionManager.ReclaimGarbageRegions();
        }
        MRT_PHASE_TIMER("TryReleaseGarbageMemory");
        size_t size = regionManager.GetAllocatedSize();
        size_t targetCachedSize = static_cast<size_t>(size * cachedRatio);
        regionManager.ReleaseGarbageRegions(targetCachedSize);
        return;
    }
#endif
    bool ForEachObj(const std::function<void(BaseObject*)>& visitor, bool safe) const override
    {
#ifdef MRT_USE_MARK_SWEEP
        // In MS mode, RegionManager region lists are not populated.
        // Object iteration would go through CentralRegionPool if needed.
        (void)visitor;
        (void)safe;
        return true;
#else
        if (UNLIKELY(safe)) {
            regionManager.ForEachObjSafe(visitor);
        } else {
            regionManager.ForEachObjUnsafe(visitor);
        }
        return true;
#endif
    }

    // Return the garbage size of from space.
#ifndef MRT_USE_MARK_SWEEP
    size_t RefineFromSpace()
    {
        MRT_PHASE_TIMER("ExemptFromRegions");
        return regionManager.ExemptFromRegions();
    }

    BaseObject* RouteObject(BaseObject* fromObj) { return regionManager.RouteObject(fromObj); }

    void PrepareFromSpace() { regionManager.PrepareFromRegionList(); }

    void ClearAllLiveInfo() { regionManager.ClearAllLiveInfo(); }

    void ForwardFromSpace(GCThreadPool* threadPool)
    {
        MRT_PHASE_TIMER("ForwardFromRegions");
        regionManager.ForwardFromRegions(threadPool);
    }

    size_t CollectLargeGarbage() { return regionManager.CollectLargeGarbage(); }

    size_t CollectPinnedGarbage() { return regionManager.CollectPinnedGarbage(); }

    void CollectFromSpaceGarbage()
    {
        regionManager.CollectFromSpaceGarbage();
        regionManager.ReassembleFromSpace();
    }

    void AssembleGarbageCandidates(bool collectAll = false)
    {
        regionManager.AssembleSmallGarbageCandidates();
        regionManager.AssemblePinnedGarbageCandidates(collectAll);
        regionManager.AssembleLargeGarbageCandidates();
    }
#endif // !MRT_USE_MARK_SWEEP

#ifdef MRT_USE_MARK_SWEEP
    void MSExemptLiveRegions()
    {
        // In the new MSRegionManager design, region lists are not used.
        // Live region exemption is a no-op.
    }
#endif

#ifndef MRT_USE_MARK_SWEEP
    void DumpRegionStats(const char* msg) const { regionManager.DumpRegionStats(msg); }

    void CountLiveObject(const BaseObject* obj) { regionManager.CountLiveObject(obj); }

    void PrepareTrace() { regionManager.PrepareTrace(); }
#endif
    void FeedHungryBuffers() override;

#ifdef MRT_USE_MARK_SWEEP
    static bool MarkObject(const BaseObject* obj); // defined in RegionSpace.cpp
#else
    static bool MarkObject(const BaseObject* obj)
    {
        RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        return regionInfo->MarkObject(obj);
    }
#endif

    static bool IsMarkedObject(const BaseObject* obj);

    static bool ShouldEnqueue(const BaseObject* obj)
    {
#ifdef MRT_USE_MARK_SWEEP
        // MS mode: always enqueue old value into SATB buffer (pure SATB approach).
        // The marker will skip already-marked objects.
        (void)obj;
        return true;
#else
        RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        if (regionInfo->IsTraceRegion()) {
            return false;
        }
        size_t offset = regionInfo->GetAddressOffset(reinterpret_cast<MAddress>(obj));
        if (regionInfo->IsMarkedObject(offset)) {
            return false;
        }
        return !regionInfo->EnqueueObject(obj, offset);
#endif
    }

    static bool IsResurrectedObject([[maybe_unused]] const BaseObject* obj)
    {
#ifdef MRT_USE_MARK_SWEEP
        // MS mode: no object resurrection (no copying/forwarding).
        return false;
#else
        RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        return regionInfo->IsResurrectedObject(obj);
#endif
    }

#ifndef MRT_USE_MARK_SWEEP
    void AddRawPointerObject(BaseObject* obj) { regionManager.AddRawPointerObject(obj); }

    void RemoveRawPointerObject(BaseObject* obj) { regionManager.RemoveRawPointerObject(obj); }
#endif

#ifdef MRT_USE_MARK_SWEEP
    CentralRegionPool* GetCentralPool() const { return msCentralPool_; }
    LargeObjectSpace* GetLargeObjectSpace() const { return msLargeObjectSpace_; }
    MSRegionManager* GetMSRegionManager() const { return msRegionManager_; }
    AllocBufferMS* GetOrCreateAllocBufferMS();
#endif

    friend class Allocator;

private:
#ifdef MRT_USE_MARK_SWEEP
    // Mark-Sweep allocation entry point: handles large/huge/small objects via MS subsystem.
    MAddress TryAllocateOnceMS(size_t allocSize);
    // Register a large object (allocated via RegionManager::TakeRegion) in MSRegionInfo
    // so that MS GC can mark/sweep it. Called from TryAllocateOnceMS.
    void RegisterLargeObjectMS(MAddress addr, size_t regionBytes);
#endif
    enum class TryAllocationThreshold {
        RESCHEDULE = 3,
        TRIGGER_OOM = 5,
    };
    MAddress TryAllocateOnce(size_t allocSize, AllocType allocType);
    bool ShouldRetryAllocation(size_t& tryTimes, size_t size) const;
    MAddress reservedStart = 0;
    MAddress reservedEnd = 0;
    RegionManager regionManager;
    MemMap* map{ nullptr };

#ifdef MRT_USE_MARK_SWEEP
    // Mark-Sweep allocation subsystem (initialized in Init when MS mode is active)
    // Per-thread AllocBufferMS instances are managed via TLS and registered
    // in CentralRegionPool for GC FlushAll.
    CentralRegionPool* msCentralPool_ = nullptr;
    LargeObjectSpace* msLargeObjectSpace_ = nullptr;
    MSRegionManager* msRegionManager_ = nullptr;  // Owns MSRegionInfo[] at mmap front
#endif
};
} // namespace MapleRuntime
#endif // MRT_REGION_SPACE_H

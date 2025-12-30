// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Allocator/LocalObjectAllocator.h"

#include "Allocator/Allocator.h"
#include "Allocator/RegionInfo.h"
#include "Allocator/RegionManager.h"
#include "Base/Globals.h"
#include "Base/Log.h"
#include "Common/NativeAllocator.h"
#include "ExceptionManager.inline.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MObject.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_set>
#include <vector>

namespace MapleRuntime {
class LocalObjectRootRegistry {
public:
    // TODO(localmode): replace std::function with a lightweight function_ref/template when this path is hot.
    using LocalObjectChecker = std::function<bool(BaseObject*)>;

    void AddFinalizer(BaseObject* obj);
    bool RunFinalizer(Mutator& mutator, BaseObject* obj);
    void AddLocalRoot(BaseObject* obj);
    void VisitLocalObjectRefFields(const RootVisitor& visitor, const LocalObjectChecker& isLocalObject,
                                   std::unordered_set<BaseObject*>& visited) const;
    bool RunPendingFinalizers(Mutator& mutator);

private:
    enum class FinalizerState : uint8_t {
        PENDING,
        RUNNING,
        DONE,
    };

    struct FinalizerEntry {
        BaseObject* obj;
        FinalizerState state;
    };

    void VisitOneLocalObject(BaseObject* obj, const RootVisitor& visitor, const LocalObjectChecker& isLocalObject,
                             std::unordered_set<BaseObject*>& visited) const;
    bool RunFinalizerAt(Mutator& mutator, size_t idx);

    // DONE entries retain the object identity to reject accidental re-registration,
    // but are not visited as roots and can be compacted if churn becomes measurable.
    std::vector<FinalizerEntry> finalizers;
    std::vector<BaseObject*> localRoots;
};

// Heap-backed local allocator. It preserves the current local region
// implementation while hiding RegionInfo stack manipulation behind
// LocalObjectAllocator.
class HeapLocalObjectAllocator final : public LocalObjectAllocator {
public:
    explicit HeapLocalObjectAllocator(RegionManager& regionManager) : regionManager(regionManager) {}
    ~HeapLocalObjectAllocator() override = default;

    bool StartRegion(Mutator& mutator, FrameAddress* ownerFA) override;
    void EndRegion(Mutator& mutator, FrameAddress* ownerFA) override;
    void EndRegionsForFrame(Mutator& mutator, FrameAddress* ownerFA) override;
    MAddress Allocate(Mutator& mutator, size_t size) override;
    bool IsLocalAddress(MAddress addr, const Mutator* mutator = nullptr) const override;
    size_t GetHeapAllocatedBytes() const override
    {
        return heapAllocatedBytes.load(std::memory_order_relaxed);
    }
    void AddFinalizer(Mutator& mutator, BaseObject* obj) override;
    void RemoveFinalizer(Mutator& mutator, BaseObject* obj) override;
    void AddLocalRoot(Mutator& mutator, BaseObject* obj) override;
    void VisitLocalObjectRefFields(Mutator& mutator, const RootVisitor& visitor) override;

private:
    struct LocalHeapFirstBlock;

    struct LocalHeapBlock {
        LocalHeapBlock(RegionInfo* region, LocalHeapBlock* prev, LocalHeapFirstBlock* firstBlock)
            : region(region), prev(prev), firstBlock(firstBlock) {}

        RegionInfo* region;
        LocalHeapBlock* prev;
        size_t allocatedBytes = 0;
        LocalHeapFirstBlock* firstBlock;
    };

    struct LocalHeapFirstBlock final : LocalHeapBlock {
        LocalHeapFirstBlock(RegionInfo* region, LocalHeapBlock* prev, FrameAddress* ownerFA)
            : LocalHeapBlock(region, prev, this), ownerFA(ownerFA) {}

        FrameAddress* ownerFA;
        LocalObjectRootRegistry rootRegistry;
    };

    LocalHeapBlock* GetBlockStackTop(const Mutator& mutator) const;
    void SetBlockStackTop(Mutator& mutator, LocalHeapBlock* block);
    bool PushBlock(Mutator& mutator, RegionInfo* region, bool firstOfRegion, FrameAddress* ownerFA);
    LocalHeapBlock* PopRegionBlocks(Mutator& mutator, FrameAddress* ownerFA);
    LocalHeapBlock* FindBlockOfObject(Mutator& mutator, BaseObject* obj);
    void RunLocalFinalizers(Mutator& mutator, FrameAddress* ownerFA);
    void RecordAllocation(LocalHeapBlock& block, size_t size);
    void RecordReclamation(const LocalHeapBlock& block);

    RegionManager& regionManager;
    std::atomic<size_t> heapAllocatedBytes = { 0 };
};

// Native-backed local allocator. It allocates one or more native arenas for
// each local region and indexes each backing region by its 4KB logical slot.
// GC treats native local objects as scan sources and only visits heap reference
// fields reachable from them.
class NativeLocalObjectAllocator final : public LocalObjectAllocator {
public:
    NativeLocalObjectAllocator() = default;
    ~NativeLocalObjectAllocator() override = default;

    bool StartRegion(Mutator& mutator, FrameAddress* ownerFA) override;
    void EndRegion(Mutator& mutator, FrameAddress* ownerFA) override;
    void EndRegionsForFrame(Mutator& mutator, FrameAddress* ownerFA) override;
    MAddress Allocate(Mutator& mutator, size_t size) override;
    bool IsLocalAddress(MAddress addr, const Mutator* mutator = nullptr) const override;
    size_t GetHeapAllocatedBytes() const override { return 0; }
    void AddFinalizer(Mutator& mutator, BaseObject* obj) override;
    void RemoveFinalizer(Mutator& mutator, BaseObject* obj) override;
    void AddLocalRoot(Mutator& mutator, BaseObject* obj) override;
    void VisitLocalObjectRefFields(Mutator& mutator, const RootVisitor& visitor) override;

private:
    struct LocalNativeFirstRegion;
    struct NativeMutatorLocalData;

    struct LocalNativeRegion {
        LocalNativeRegion(MAddress base, size_t size, LocalNativeRegion* prev, LocalNativeFirstRegion* firstRegion)
            : base(base), cursor(base), end(base + size), size(size), prev(prev), firstRegion(firstRegion) {}

        MAddress base;
        MAddress cursor;
        MAddress end;
        size_t size;
        LocalNativeRegion* prev;
        MAddress largeObjectStart = 0;
        bool hasObjects = false;
        LocalNativeFirstRegion* firstRegion;
    };

    // A compact open-addressed table avoids the node allocation and per-node
    // pointers of std::unordered_map while retaining expected O(1) lookup.
    class LocalRegionIndex {
    public:
        LocalNativeRegion* Find(MAddress slot) const
        {
            if (entries.empty()) {
                return nullptr;
            }
            size_t idx = Hash(slot) & (entries.size() - 1);
            for (size_t probe = 0; probe < entries.size(); ++probe) {
                const Entry& entry = entries[idx];
                if (entry.region == nullptr) {
                    return nullptr;
                }
                if (entry.region != Tombstone() && entry.slot == slot) {
                    return entry.region;
                }
                idx = (idx + 1) & (entries.size() - 1);
            }
            return nullptr;
        }

        bool Insert(MAddress slot, LocalNativeRegion* region)
        {
            CHECK_DETAIL(region != nullptr && region != Tombstone(), "insert invalid native local region");
            EnsureInsertCapacity();
            return InsertWithoutResize(slot, region);
        }

        bool Erase(MAddress slot)
        {
            if (entries.empty()) {
                return false;
            }
            size_t idx = Hash(slot) & (entries.size() - 1);
            for (size_t probe = 0; probe < entries.size(); ++probe) {
                Entry& entry = entries[idx];
                if (entry.region == nullptr) {
                    return false;
                }
                if (entry.region != Tombstone() && entry.slot == slot) {
                    entry.region = Tombstone();
                    --entryCount;
                    ++tombstoneCount;
                    CompactAfterErase();
                    return true;
                }
                idx = (idx + 1) & (entries.size() - 1);
            }
            return false;
        }

    private:
        struct Entry {
            MAddress slot = 0;
            LocalNativeRegion* region = nullptr;
        };

        static constexpr size_t INITIAL_CAPACITY = 4;

        static LocalNativeRegion* Tombstone()
        {
            return reinterpret_cast<LocalNativeRegion*>(static_cast<uintptr_t>(1));
        }

        static size_t Hash(MAddress slot)
        {
            // Mix adjacent 4KB slots so clustered native regions do not form
            // long linear-probe runs after table growth.
            size_t value = static_cast<size_t>(slot);
            value ^= value >> 30;
            value *= static_cast<size_t>(0xbf58476d1ce4e5b9ULL);
            value ^= value >> 27;
            value *= static_cast<size_t>(0x94d049bb133111ebULL);
            value ^= value >> 31;
            return value;
        }

        void EnsureInsertCapacity()
        {
            if (entries.empty()) {
                Rehash(INITIAL_CAPACITY);
                return;
            }
            // Keep at least 30% empty slots, including tombstones.
            if ((entryCount + tombstoneCount + 1) * 10 >= entries.size() * 7) {
                Rehash(entries.size() * 2);
            }
        }

        bool InsertWithoutResize(MAddress slot, LocalNativeRegion* region)
        {
            size_t idx = Hash(slot) & (entries.size() - 1);
            size_t firstTombstone = entries.size();
            for (size_t probe = 0; probe < entries.size(); ++probe) {
                Entry& entry = entries[idx];
                if (entry.region == nullptr) {
                    size_t insertIdx = firstTombstone == entries.size() ? idx : firstTombstone;
                    entries[insertIdx] = { slot, region };
                    ++entryCount;
                    if (firstTombstone != entries.size()) {
                        --tombstoneCount;
                    }
                    return true;
                }
                if (entry.region == Tombstone()) {
                    if (firstTombstone == entries.size()) {
                        firstTombstone = idx;
                    }
                } else if (entry.slot == slot) {
                    return false;
                }
                idx = (idx + 1) & (entries.size() - 1);
            }
            CHECK_DETAIL(firstTombstone != entries.size(), "native local region index is full");
            entries[firstTombstone] = { slot, region };
            ++entryCount;
            --tombstoneCount;
            return true;
        }

        void CompactAfterErase()
        {
            if (entryCount == 0) {
                std::fill(entries.begin(), entries.end(), Entry {});
                tombstoneCount = 0;
                return;
            }
            if (entries.size() > INITIAL_CAPACITY && entryCount * 8 <= entries.size()) {
                Rehash(entries.size() / 2);
            } else if (tombstoneCount > entryCount) {
                Rehash(entries.size());
            }
        }

        void Rehash(size_t capacity)
        {
            CHECK_DETAIL((capacity & (capacity - 1)) == 0, "native local region index capacity is not power of two");
            std::vector<Entry> oldEntries;
            oldEntries.swap(entries);
            entries.resize(capacity);
            entryCount = 0;
            tombstoneCount = 0;
            for (const Entry& entry : oldEntries) {
                if (entry.region != nullptr && entry.region != Tombstone()) {
                    CHECK_DETAIL(InsertWithoutResize(entry.slot, entry.region),
                                 "duplicate native local region while rebuilding index");
                }
            }
        }

        std::vector<Entry> entries;
        size_t entryCount = 0;
        size_t tombstoneCount = 0;
    };

    struct NativeMutatorLocalData {
        LocalNativeRegion* stackTop = nullptr;
        LocalRegionIndex localRegionIndex;
    };

    struct LocalNativeFirstRegion final : LocalNativeRegion {
        LocalNativeFirstRegion(MAddress base, size_t size, LocalNativeRegion* prev, FrameAddress* ownerFA,
                               NativeMutatorLocalData* ownerData)
            : LocalNativeRegion(base, size, prev, this), ownerFA(ownerFA), ownerData(ownerData) {}

        FrameAddress* ownerFA;
        NativeMutatorLocalData* ownerData;
        LocalObjectRootRegistry rootRegistry;
    };

    NativeMutatorLocalData* GetMutatorData(const Mutator& mutator) const;
    NativeMutatorLocalData* CreateMutatorData(Mutator& mutator);
    void ReleaseMutatorDataIfEmpty(Mutator& mutator);
    LocalNativeRegion* GetRegionStackTop(const Mutator& mutator) const;
    void SetRegionStackTop(Mutator& mutator, LocalNativeRegion* region);
    LocalNativeRegion* AllocateRegion(Mutator& mutator, size_t minSize, LocalNativeRegion* prev, bool firstOfRegion,
                                      FrameAddress* ownerFA);
    void FreeRegion(LocalNativeRegion* region);
    MAddress TryAllocateInRegion(LocalNativeRegion& region, size_t size);
    LocalNativeRegion* PopRegionChain(Mutator& mutator, FrameAddress* ownerFA);
    LocalNativeRegion* FindRegionOfObject(Mutator& mutator, BaseObject* obj);
    bool IsAddressInRegion(MAddress addr, const LocalNativeRegion& region) const;
    void RegisterRegion(LocalNativeRegion& region);
    void UnregisterRegion(LocalNativeRegion& region);
    void RegisterObject(MAddress obj, LocalNativeRegion& region);
    void WriteObjectMarker(MAddress obj);
    void RunLocalFinalizers(Mutator& mutator, FrameAddress* ownerFA);

    // Mutator-local queries use their private index without this lock. The lock
    // only synchronizes region lifetime with rare all-mutator queries.
    mutable std::mutex activeMutatorDataMtx;
    std::vector<NativeMutatorLocalData*> activeMutatorData;
    // Empty regions do not make an address a live local object. Keep a global
    // summary so normal mode avoids locking and scanning every mutator.
    std::atomic<size_t> nonEmptyRegionCount = { 0 };
};

namespace {
constexpr size_t DEFAULT_NATIVE_LOCAL_REGION_SIZE = 4 * KB;
constexpr size_t NATIVE_LOCAL_REGION_SLOT_SHIFT = 12;
static_assert((static_cast<size_t>(1) << NATIVE_LOCAL_REGION_SLOT_SHIFT) == DEFAULT_NATIVE_LOCAL_REGION_SIZE,
              "native local region slot size must match normal region size");
constexpr size_t LOCAL_OBJECT_MARKER_SIZE = sizeof(uint64_t);
constexpr uint64_t LOCAL_OBJECT_MARKER = 0x4c4f43414c4f424aULL; // "LOCALOBJ"

void RunLocalObjectFinalizer(Mutator& mutator, BaseObject* obj)
{
    TypeInfo* classInfo = reinterpret_cast<MObject*>(obj)->GetTypeInfo();
    FuncRef finalizerMethod = classInfo->GetFinalizeMethod();
    CHECK_DETAIL(finalizerMethod != nullptr, "%p has no finalize method", obj);
    void (*finalizer)(BaseObject*, TypeInfo*) = reinterpret_cast<void (*)(BaseObject*, TypeInfo*)>(finalizerMethod);

    bool oldManagedContext = mutator.IsManagedContext();
    mutator.SetManagedContext(true);
    DLOG(FINALIZE, "tid %u finalize local object %p", mutator.GetTid(), obj);
    uintptr_t threadData = MapleRuntime::MRT_GetThreadLocalData();
    ExecuteCangjieStub(obj, obj->GetTypeInfo(), 0, reinterpret_cast<void*>(finalizer),
                       reinterpret_cast<void*>(threadData), 0);
    mutator.SetManagedContext(oldManagedContext);

    if (ExceptionManager::HasFatalException()) {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        ExceptionManager::DumpException();
#endif
        LOG(RTLOG_FATAL, "FatalException happened in local finalizer");
    }
    ExceptionManager::ClearPendingException();
}
}

void LocalObjectRootRegistry::AddFinalizer(BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    for (const FinalizerEntry& entry : finalizers) {
        if (entry.obj == obj) {
            return;
        }
    }
    finalizers.push_back({ obj, FinalizerState::PENDING });
}

bool LocalObjectRootRegistry::RunFinalizer(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return false;
    }
    for (size_t idx = 0; idx < finalizers.size(); ++idx) {
        if (finalizers[idx].obj == obj) {
            return RunFinalizerAt(mutator, idx);
        }
    }
    return false;
}

void LocalObjectRootRegistry::AddLocalRoot(BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    for (BaseObject* localRoot : localRoots) {
        if (localRoot == obj) {
            return;
        }
    }
    localRoots.push_back(obj);
}

void LocalObjectRootRegistry::VisitLocalObjectRefFields(const RootVisitor& visitor,
                                                        const LocalObjectChecker& isLocalObject,
                                                        std::unordered_set<BaseObject*>& visited) const
{
    for (BaseObject* localRoot : localRoots) {
        VisitOneLocalObject(localRoot, visitor, isLocalObject, visited);
    }
    for (const FinalizerEntry& entry : finalizers) {
        if (entry.state != FinalizerState::DONE) {
            // RUNNING finalizers remain scan sources while user code executes,
            // so a re-entrant GC can still trace and update their heap fields.
            VisitOneLocalObject(entry.obj, visitor, isLocalObject, visited);
        }
    }
}

void LocalObjectRootRegistry::VisitOneLocalObject(BaseObject* obj, const RootVisitor& visitor,
                                                  const LocalObjectChecker& isLocalObject,
                                                  std::unordered_set<BaseObject*>& visited) const
{
    if (obj == nullptr || !visited.insert(obj).second) {
        return;
    }
    CHECK_DETAIL(isLocalObject(obj), "visit local object ref fields with non-local object %p", obj);
    obj->ForEachRefField([&visitor, &isLocalObject, &visited, this](RefField<>& field) {
        BaseObject* target = field.GetTargetObject();
        if (target == nullptr) {
            return;
        }
        if (isLocalObject(target)) {
            VisitOneLocalObject(target, visitor, isLocalObject, visited);
            return;
        }
        if (Heap::IsHeapAddress(target)) {
            visitor(reinterpret_cast<ObjectRef&>(field));
            return;
        }
        // TODO(localmode): decide whether raw/native non-heap references are legal in local object fields.
        LOG(RTLOG_FATAL, "local object field references non-heap and non-local object %p", target);
    });
}

bool LocalObjectRootRegistry::RunPendingFinalizers(Mutator& mutator)
{
    bool ranFinalizer = false;
    for (size_t idx = 0; idx < finalizers.size(); ++idx) {
        if (finalizers[idx].state != FinalizerState::PENDING) {
            continue;
        }
        ranFinalizer = RunFinalizerAt(mutator, idx) || ranFinalizer;
    }
    return ranFinalizer;
}

bool LocalObjectRootRegistry::RunFinalizerAt(Mutator& mutator, size_t idx)
{
    CHECK_DETAIL(idx < finalizers.size(), "invalid local finalizer index %zu", idx);
    if (finalizers[idx].state != FinalizerState::PENDING) {
        return false;
    }

    BaseObject* finalizerObj = finalizers[idx].obj;
    finalizers[idx].state = FinalizerState::RUNNING;
    RunLocalObjectFinalizer(mutator, finalizerObj);

    // User code may register more finalizers and reallocate the vector, so use
    // the stable index instead of retaining a reference across the call.
    CHECK_DETAIL(idx < finalizers.size() && finalizers[idx].obj == finalizerObj,
                 "local finalizer registry changed running entry %p", finalizerObj);
    finalizers[idx].state = FinalizerState::DONE;
    return true;
}

LocalObjectAllocator* LocalObjectAllocator::InitLocalObjectAllocator(RegionManager& regionManager)
{
    (void)regionManager;
    // Native local regions are the production implementation. Keep the heap
    // allocator above for comparison and future fallback, but do not select it
    // through an environment variable.
    LocalObjectAllocator* localObjectAllocator = new (std::nothrow) NativeLocalObjectAllocator();
    VLOG(LOCAL_REGION, "use native local object allocator");
    CHECK_DETAIL(localObjectAllocator != nullptr, "new local object allocator failed");
    return localObjectAllocator;
}

bool HeapLocalObjectAllocator::StartRegion(Mutator& mutator, FrameAddress* ownerFA)
{
    RegionInfo* region = regionManager.AllocateLocalModeRegion();
    if (region == nullptr) {
        // todo gc or oom
        VLOG(LOCAL_REGION, "Start local object region failed, region is null");
        return false;
    }
    if (!PushBlock(mutator, region, true, ownerFA)) {
        regionManager.ReclaimLocalModeRegion(region);
        return false;
    }
    VLOG(LOCAL_REGION, "StartLocalRegion: mutator %p, region %p, ownerFA %p, size %zu, stackTop %p", &mutator,
         region, ownerFA, region->GetRegionSize(), GetBlockStackTop(mutator));
    return true;
}

void HeapLocalObjectAllocator::EndRegion(Mutator& mutator, FrameAddress* ownerFA)
{
    RunLocalFinalizers(mutator, ownerFA);
    LocalHeapBlock* headBlock = PopRegionBlocks(mutator, ownerFA);
    while (headBlock != nullptr) {
        LocalHeapBlock* del = headBlock;
        headBlock = headBlock->prev;
        VLOG(LOCAL_REGION, "CollectLocalGarbage: region %p, ownerFA %p", del->region, del->firstBlock->ownerFA);
        RecordReclamation(*del);
        regionManager.ReclaimLocalModeRegion(del->region);
        if (del->firstBlock == del) {
            delete static_cast<LocalHeapFirstBlock*>(del);
        } else {
            delete del;
        }
    }
}

void HeapLocalObjectAllocator::EndRegionsForFrame(Mutator& mutator, FrameAddress* ownerFA)
{
    if (ownerFA == nullptr) {
        return;
    }
    while (true) {
        LocalHeapBlock* block = GetBlockStackTop(mutator);
        if (block == nullptr || block->firstBlock->ownerFA != ownerFA) {
            return;
        }
        EndRegion(mutator, ownerFA);
    }
}

MAddress HeapLocalObjectAllocator::Allocate(Mutator& mutator, size_t size)
{
    MAddress addr = 0;
    LocalHeapBlock* block = GetBlockStackTop(mutator);
    if (UNLIKELY(block == nullptr)) {
        LOG(RTLOG_ERROR, "alloc heap local object without local object region");
        return 0;
    }

    addr = block->region->Alloc(size);
    if (addr != 0) {
        RecordAllocation(*block, size);
        VLOG(LOCAL_REGION,
             "AllocLocalMode in old region: mutator %p, regoin %p(isFirst %d), "
             "ownerFA %p, size %zu, stackTop %p, obj %p, objSize %zu",
             &mutator, block->region, block->firstBlock == block, block->firstBlock->ownerFA,
             block->region->GetRegionSize(),
             GetBlockStackTop(mutator), addr, size);
        return addr;
    }

    RegionInfo* region = regionManager.AllocateLocalModeRegion(size);
    if (region == nullptr) {
        DLOG(REGION, "cannot take a local mode region from heap");
        return 0;
    }
    if (!PushBlock(mutator, region, false, block->firstBlock->ownerFA)) {
        regionManager.ReclaimLocalModeRegion(region);
        return 0;
    }
    addr = region->Alloc(size);
    CHECK_DETAIL(addr != 0, "alloc local object failed");
    LocalHeapBlock* newBlock = GetBlockStackTop(mutator);
    CHECK_DETAIL(newBlock != nullptr && newBlock->region == region, "invalid local heap block for region %p", region);
    RecordAllocation(*newBlock, size);
    VLOG(LOCAL_REGION,
         "AllocLocalMode in new region: mutator %p, regoin %p(isFirst %d), "
         "ownerFA %p, size %zu, stackTop %p, obj %p, objSize %zu",
         &mutator, region, false, block->firstBlock->ownerFA, region->GetRegionSize(),
         GetBlockStackTop(mutator), addr, size);
    return addr;
}

bool HeapLocalObjectAllocator::IsLocalAddress(MAddress addr, const Mutator* mutator) const
{
    if (addr == 0 || !Heap::IsHeapAddress(addr)) {
        return false;
    }
    RegionInfo* region = RegionInfo::GetRegionInfoAt(addr);
    if (!region->IsLocalModeRegion()) {
        return false;
    }
    if (mutator == nullptr) {
        return true;
    }
    for (LocalHeapBlock* block = GetBlockStackTop(*mutator); block != nullptr; block = block->prev) {
        if (block->region == region) {
            return true;
        }
    }
    return false;
}

void HeapLocalObjectAllocator::AddFinalizer(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    LocalHeapBlock* block = GetBlockStackTop(mutator);
    CHECK_DETAIL(block != nullptr, "add local finalizer without current local region: %p", obj);
    block->firstBlock->rootRegistry.AddFinalizer(obj);
    VLOG(LOCAL_REGION, "AddLocalFinalizer: mutator %p, block %p, obj %p", &mutator, block, obj);
}

void HeapLocalObjectAllocator::RemoveFinalizer(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    LocalHeapBlock* block = FindBlockOfObject(mutator, obj);
    if (block != nullptr && block->firstBlock->rootRegistry.RunFinalizer(mutator, obj)) {
        VLOG(LOCAL_REGION, "RemoveLocalFinalizer: mutator %p, block %p, obj %p", &mutator, block, obj);
    }
}

void HeapLocalObjectAllocator::AddLocalRoot(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    LocalHeapBlock* block = FindBlockOfObject(mutator, obj);
    CHECK_DETAIL(block != nullptr, "add local root with non-local object %p", obj);
    block->firstBlock->rootRegistry.AddLocalRoot(obj);
    VLOG(LOCAL_REGION, "AddLocalRoot: mutator %p, block %p, obj %p", &mutator, block, obj);
}

void HeapLocalObjectAllocator::VisitLocalObjectRefFields(Mutator& mutator, const RootVisitor& visitor)
{
    // TODO(localmode): reuse a per-mutator scratch visited set to avoid allocating during every root visit.
    std::unordered_set<BaseObject*> visited;
    for (LocalHeapBlock* block = GetBlockStackTop(mutator); block != nullptr; block = block->prev) {
        if (block->firstBlock != block) {
            continue;
        }
        block->firstBlock->rootRegistry.VisitLocalObjectRefFields(visitor, [this, &mutator](BaseObject* obj) {
            return IsLocalAddress(reinterpret_cast<MAddress>(obj), &mutator);
        }, visited);
    }
}

HeapLocalObjectAllocator::LocalHeapBlock* HeapLocalObjectAllocator::GetBlockStackTop(const Mutator& mutator) const
{
    return reinterpret_cast<LocalHeapBlock*>(mutator.GetLocalObjectAllocatorData());
}

void HeapLocalObjectAllocator::SetBlockStackTop(Mutator& mutator, LocalHeapBlock* block)
{
    mutator.SetLocalObjectAllocatorData(block);
}

bool HeapLocalObjectAllocator::PushBlock(Mutator& mutator, RegionInfo* region, bool firstOfRegion,
                                         FrameAddress* ownerFA)
{
    LocalHeapBlock* prev = GetBlockStackTop(mutator);
    CHECK_DETAIL(firstOfRegion || prev != nullptr, "push non-first local heap block without active region");
    LocalHeapBlock* block = firstOfRegion
        ? static_cast<LocalHeapBlock*>(new (std::nothrow) LocalHeapFirstBlock(region, prev, ownerFA))
        : new (std::nothrow) LocalHeapBlock(region, prev, prev->firstBlock);
    if (block == nullptr) {
        VLOG(LOCAL_REGION, "push local heap block failed, region %p", region);
        return false;
    }
    SetBlockStackTop(mutator, block);
    return true;
}

HeapLocalObjectAllocator::LocalHeapBlock* HeapLocalObjectAllocator::PopRegionBlocks(Mutator& mutator,
                                                                                   FrameAddress* ownerFA)
{
    LocalHeapBlock* curr = GetBlockStackTop(mutator);
    if (curr != nullptr && ownerFA != nullptr && curr->firstBlock->ownerFA != ownerFA) {
        LOG(RTLOG_FATAL, "end local heap region with mismatched frame: top ownerFA %p, target ownerFA %p",
            curr->firstBlock->ownerFA, ownerFA);
    }
    LocalHeapBlock* ret = curr;
    while (curr != nullptr) {
        LocalHeapBlock* prev = curr->prev;
        if (ownerFA != nullptr && curr->firstBlock->ownerFA != ownerFA) {
            LOG(RTLOG_FATAL, "local heap region crosses frame boundary: block ownerFA %p, target ownerFA %p",
                curr->firstBlock->ownerFA, ownerFA);
        }
        if (curr->firstBlock == curr) {
            SetBlockStackTop(mutator, prev);
            curr->prev = nullptr;
            break;
        }
        curr = prev;
    }
    return ret;
}

HeapLocalObjectAllocator::LocalHeapBlock* HeapLocalObjectAllocator::FindBlockOfObject(Mutator& mutator,
                                                                                      BaseObject* obj)
{
    if (!IsLocalAddress(reinterpret_cast<MAddress>(obj))) {
        return nullptr;
    }
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    for (LocalHeapBlock* block = GetBlockStackTop(mutator); block != nullptr; block = block->prev) {
        if (block->region == region) {
            return block;
        }
    }
    return nullptr;
}

void HeapLocalObjectAllocator::RunLocalFinalizers(Mutator& mutator, FrameAddress* ownerFA)
{
    LocalHeapBlock* block = GetBlockStackTop(mutator);
    if (block == nullptr) {
        return;
    }
    if (ownerFA != nullptr && block->firstBlock->ownerFA != ownerFA) {
        LOG(RTLOG_FATAL, "run local heap finalizers with mismatched frame: block ownerFA %p, target ownerFA %p",
            block->firstBlock->ownerFA, ownerFA);
    }
    while (block->firstBlock->rootRegistry.RunPendingFinalizers(mutator)) {
    }
}

void HeapLocalObjectAllocator::RecordAllocation(LocalHeapBlock& block, size_t size)
{
    CHECK_DETAIL(block.allocatedBytes <= std::numeric_limits<size_t>::max() - size,
                 "local heap block allocated bytes overflow");
    block.allocatedBytes += size;
    (void)heapAllocatedBytes.fetch_add(size, std::memory_order_relaxed);
}

void HeapLocalObjectAllocator::RecordReclamation(const LocalHeapBlock& block)
{
    size_t oldSize = heapAllocatedBytes.fetch_sub(block.allocatedBytes, std::memory_order_relaxed);
    CHECK_DETAIL(oldSize >= block.allocatedBytes,
                 "local heap allocated bytes underflow: old %zu, reclaimed %zu", oldSize, block.allocatedBytes);
}

bool NativeLocalObjectAllocator::StartRegion(Mutator& mutator, FrameAddress* ownerFA)
{
    LocalNativeRegion* region =
        AllocateRegion(mutator, DEFAULT_NATIVE_LOCAL_REGION_SIZE, GetRegionStackTop(mutator), true, ownerFA);
    if (region == nullptr) {
        VLOG(LOCAL_REGION, "Start native local object region failed");
        return false;
    }
    SetRegionStackTop(mutator, region);
    VLOG(LOCAL_REGION, "StartNativeLocalRegion: mutator %p, region %p, ownerFA %p, base %p, size %zu", &mutator,
         region, ownerFA, reinterpret_cast<void*>(region->base), region->size);
    return true;
}

void NativeLocalObjectAllocator::EndRegion(Mutator& mutator, FrameAddress* ownerFA)
{
    RunLocalFinalizers(mutator, ownerFA);
    LocalNativeRegion* headRegion = PopRegionChain(mutator, ownerFA);
    while (headRegion != nullptr) {
        LocalNativeRegion* del = headRegion;
        headRegion = headRegion->prev;
        VLOG(LOCAL_REGION, "CollectNativeLocalRegion: region %p, ownerFA %p, base %p, size %zu", del,
             del->firstRegion->ownerFA,
             reinterpret_cast<void*>(del->base), del->size);
        FreeRegion(del);
    }
    ReleaseMutatorDataIfEmpty(mutator);
}

void NativeLocalObjectAllocator::EndRegionsForFrame(Mutator& mutator, FrameAddress* ownerFA)
{
    if (ownerFA == nullptr) {
        return;
    }
    while (true) {
        LocalNativeRegion* region = GetRegionStackTop(mutator);
        if (region == nullptr || region->firstRegion->ownerFA != ownerFA) {
            return;
        }
        EndRegion(mutator, ownerFA);
    }
}

MAddress NativeLocalObjectAllocator::Allocate(Mutator& mutator, size_t size)
{
    LocalNativeRegion* region = GetRegionStackTop(mutator);
    if (UNLIKELY(region == nullptr)) {
        LOG(RTLOG_ERROR, "alloc native local object without local object region");
        return 0;
    }

    MAddress addr = TryAllocateInRegion(*region, size);
    if (addr == 0) {
        if (size > std::numeric_limits<size_t>::max() - LOCAL_OBJECT_MARKER_SIZE) {
            DLOG(REGION, "native local object size is too large");
            return 0;
        }
        size_t minRegionSize = size + LOCAL_OBJECT_MARKER_SIZE;
        region = AllocateRegion(mutator, minRegionSize, region, false, region->firstRegion->ownerFA);
        if (region == nullptr) {
            DLOG(REGION, "cannot allocate native local object region");
            return 0;
        }
        SetRegionStackTop(mutator, region);
        addr = TryAllocateInRegion(*region, size);
    }

    CHECK_DETAIL(addr != 0, "alloc native local object failed");
    WriteObjectMarker(addr);
    RegisterObject(addr, *region);
    if (!region->hasObjects) {
        region->hasObjects = true;
        nonEmptyRegionCount.fetch_add(1, std::memory_order_release);
    }
    VLOG(LOCAL_REGION, "AllocNativeLocalObject: mutator %p, region %p(isFirst %d), ownerFA %p, obj %p, objSize %zu",
         &mutator, region, region->firstRegion == region, region->firstRegion->ownerFA,
         reinterpret_cast<void*>(addr), size);
    return addr;
}

bool NativeLocalObjectAllocator::IsLocalAddress(MAddress addr, const Mutator* mutator) const
{
    if (addr == 0 || Heap::IsHeapAddress(addr)) {
        return false;
    }
    if (mutator != nullptr) {
        NativeMutatorLocalData* data = GetMutatorData(*mutator);
        if (data == nullptr) {
            return false;
        }
        LocalNativeRegion* region = data->localRegionIndex.Find(addr >> NATIVE_LOCAL_REGION_SLOT_SHIFT);
        return region != nullptr && IsAddressInRegion(addr, *region);
    }
    if (nonEmptyRegionCount.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(activeMutatorDataMtx);
    for (NativeMutatorLocalData* data : activeMutatorData) {
        if (data == nullptr) {
            continue;
        }
        LocalNativeRegion* region = data->localRegionIndex.Find(addr >> NATIVE_LOCAL_REGION_SLOT_SHIFT);
        if (region != nullptr && IsAddressInRegion(addr, *region)) {
            return true;
        }
    }
    return false;
}

void NativeLocalObjectAllocator::AddFinalizer(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    LocalNativeRegion* region = GetRegionStackTop(mutator);
    CHECK_DETAIL(region != nullptr, "add native local finalizer without current local region: %p", obj);
    region->firstRegion->rootRegistry.AddFinalizer(obj);
    VLOG(LOCAL_REGION, "AddNativeLocalFinalizer: mutator %p, region %p, obj %p", &mutator, region, obj);
}

void NativeLocalObjectAllocator::RemoveFinalizer(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    LocalNativeRegion* region = FindRegionOfObject(mutator, obj);
    if (region != nullptr && region->firstRegion->rootRegistry.RunFinalizer(mutator, obj)) {
        VLOG(LOCAL_REGION, "RemoveNativeLocalFinalizer: mutator %p, region %p, obj %p", &mutator, region, obj);
    }
}

void NativeLocalObjectAllocator::AddLocalRoot(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    LocalNativeRegion* region = FindRegionOfObject(mutator, obj);
    CHECK_DETAIL(region != nullptr, "add native local root with non-local object %p", obj);
    region->firstRegion->rootRegistry.AddLocalRoot(obj);
    VLOG(LOCAL_REGION, "AddNativeLocalRoot: mutator %p, region %p, obj %p", &mutator, region, obj);
}

void NativeLocalObjectAllocator::VisitLocalObjectRefFields(Mutator& mutator, const RootVisitor& visitor)
{
    // TODO(localmode): reuse a per-mutator scratch visited set to avoid allocating during every root visit.
    std::unordered_set<BaseObject*> visited;
    for (LocalNativeRegion* region = GetRegionStackTop(mutator); region != nullptr; region = region->prev) {
        if (region->firstRegion != region) {
            continue;
        }
        region->firstRegion->rootRegistry.VisitLocalObjectRefFields(visitor, [this, &mutator](BaseObject* obj) {
            return IsLocalAddress(reinterpret_cast<MAddress>(obj), &mutator);
        }, visited);
    }
}

NativeLocalObjectAllocator::LocalNativeRegion* NativeLocalObjectAllocator::GetRegionStackTop(
    const Mutator& mutator) const
{
    NativeMutatorLocalData* data = GetMutatorData(mutator);
    return data == nullptr ? nullptr : data->stackTop;
}

void NativeLocalObjectAllocator::SetRegionStackTop(Mutator& mutator, LocalNativeRegion* region)
{
    NativeMutatorLocalData* data = GetMutatorData(mutator);
    if (region != nullptr) {
        CHECK_DETAIL(data != nullptr, "set native local region stack without mutator data");
    }
    if (data != nullptr) {
        data->stackTop = region;
    }
}

NativeLocalObjectAllocator::NativeMutatorLocalData* NativeLocalObjectAllocator::GetMutatorData(
    const Mutator& mutator) const
{
    return static_cast<NativeMutatorLocalData*>(mutator.GetLocalObjectAllocatorData());
}

NativeLocalObjectAllocator::NativeMutatorLocalData* NativeLocalObjectAllocator::CreateMutatorData(Mutator& mutator)
{
    NativeMutatorLocalData* data = GetMutatorData(mutator);
    if (data != nullptr) {
        return data;
    }
    data = new (std::nothrow) NativeMutatorLocalData();
    CHECK_DETAIL(data != nullptr, "new native local mutator data failed");
    mutator.SetLocalObjectAllocatorData(data);
    std::lock_guard<std::mutex> lock(activeMutatorDataMtx);
    activeMutatorData.push_back(data);
    return data;
}

void NativeLocalObjectAllocator::ReleaseMutatorDataIfEmpty(Mutator& mutator)
{
    NativeMutatorLocalData* data = GetMutatorData(mutator);
    if (data == nullptr || data->stackTop != nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(activeMutatorDataMtx);
        auto it = std::find(activeMutatorData.begin(), activeMutatorData.end(), data);
        if (it != activeMutatorData.end()) {
            *it = activeMutatorData.back();
            activeMutatorData.pop_back();
        }
    }
    mutator.SetLocalObjectAllocatorData(nullptr);
    delete data;
}

NativeLocalObjectAllocator::LocalNativeRegion* NativeLocalObjectAllocator::AllocateRegion(
    Mutator& mutator, size_t minSize, LocalNativeRegion* prev, bool firstOfRegion, FrameAddress* ownerFA)
{
    size_t regionSize = std::max(DEFAULT_NATIVE_LOCAL_REGION_SIZE, RoundUp<size_t>(minSize, Allocator::ALLOC_ALIGN));
    void* memory = NativeAllocator::NativeAlloc(regionSize);
    if (memory == nullptr) {
        return nullptr;
    }
    CHECK_DETAIL(firstOfRegion || prev != nullptr, "allocate non-first native local region without active region");
    NativeMutatorLocalData* ownerData = CreateMutatorData(mutator);
    MAddress base = reinterpret_cast<MAddress>(memory);
    LocalNativeRegion* region = firstOfRegion
        ? static_cast<LocalNativeRegion*>(
              new (std::nothrow) LocalNativeFirstRegion(base, regionSize, prev, ownerFA, ownerData))
        : new (std::nothrow) LocalNativeRegion(base, regionSize, prev, prev->firstRegion);
    if (region == nullptr) {
        NativeAllocator::NativeFree(memory, regionSize);
        ReleaseMutatorDataIfEmpty(mutator);
        return nullptr;
    }
    if (regionSize == DEFAULT_NATIVE_LOCAL_REGION_SIZE) {
        RegisterRegion(*region);
    }
    return region;
}

void NativeLocalObjectAllocator::FreeRegion(LocalNativeRegion* region)
{
    CHECK_DETAIL(region != nullptr, "free null native local region");
    MAddress indexAddr = region->size == DEFAULT_NATIVE_LOCAL_REGION_SIZE ? region->base : region->largeObjectStart;
    if (indexAddr != 0) {
        UnregisterRegion(*region);
    }
    if (region->hasObjects) {
        size_t oldCount = nonEmptyRegionCount.fetch_sub(1, std::memory_order_release);
        CHECK_DETAIL(oldCount != 0, "native local non-empty region count underflow");
    }
    NativeAllocator::NativeFree(reinterpret_cast<void*>(region->base), region->size);
    if (region->firstRegion == region) {
        delete static_cast<LocalNativeFirstRegion*>(region);
    } else {
        delete region;
    }
}

MAddress NativeLocalObjectAllocator::TryAllocateInRegion(LocalNativeRegion& region, size_t size)
{
    size_t allocSize = RoundUp<size_t>(size, Allocator::ALLOC_ALIGN);
    MAddress markerAddr = RoundUp<MAddress>(region.cursor, Allocator::ALLOC_ALIGN);
    if (markerAddr > region.end || LOCAL_OBJECT_MARKER_SIZE > region.end - markerAddr) {
        return 0;
    }
    MAddress objAddr = markerAddr + LOCAL_OBJECT_MARKER_SIZE;
    if (allocSize > region.end - objAddr) {
        return 0;
    }
    region.cursor = objAddr + allocSize;
    return objAddr;
}

NativeLocalObjectAllocator::LocalNativeRegion* NativeLocalObjectAllocator::PopRegionChain(Mutator& mutator,
                                                                                        FrameAddress* ownerFA)
{
    LocalNativeRegion* curr = GetRegionStackTop(mutator);
    if (curr != nullptr && ownerFA != nullptr && curr->firstRegion->ownerFA != ownerFA) {
        LOG(RTLOG_FATAL, "end native local region with mismatched frame: top ownerFA %p, target ownerFA %p",
            curr->firstRegion->ownerFA, ownerFA);
    }
    LocalNativeRegion* ret = curr;
    while (curr != nullptr) {
        LocalNativeRegion* prev = curr->prev;
        if (ownerFA != nullptr && curr->firstRegion->ownerFA != ownerFA) {
            LOG(RTLOG_FATAL, "native local region crosses frame boundary: region ownerFA %p, target ownerFA %p",
                curr->firstRegion->ownerFA, ownerFA);
        }
        if (curr->firstRegion == curr) {
            SetRegionStackTop(mutator, prev);
            curr->prev = nullptr;
            break;
        }
        curr = prev;
    }
    return ret;
}

NativeLocalObjectAllocator::LocalNativeRegion* NativeLocalObjectAllocator::FindRegionOfObject(Mutator& mutator,
                                                                                              BaseObject* obj)
{
    MAddress addr = reinterpret_cast<MAddress>(obj);
    NativeMutatorLocalData* data = GetMutatorData(mutator);
    if (data == nullptr) {
        return nullptr;
    }
    LocalNativeRegion* top = data->stackTop;
    if (top != nullptr && IsAddressInRegion(addr, *top)) {
        return top;
    }
    LocalNativeRegion* region = data->localRegionIndex.Find(addr >> NATIVE_LOCAL_REGION_SLOT_SHIFT);
    if (region == nullptr || !IsAddressInRegion(addr, *region)) {
        return nullptr;
    }
    return region;
}

bool NativeLocalObjectAllocator::IsAddressInRegion(MAddress addr, const LocalNativeRegion& region) const
{
    if (region.size == DEFAULT_NATIVE_LOCAL_REGION_SIZE) {
        return addr >= region.base && addr < region.end;
    }
    // A large native region contains exactly one object. The marker occupies
    // the first bytes of the allocation, so the object starts after region.base.
    return addr == region.largeObjectStart;
}

void NativeLocalObjectAllocator::RegisterRegion(LocalNativeRegion& region)
{
    MAddress indexAddr = region.size == DEFAULT_NATIVE_LOCAL_REGION_SIZE ? region.base : region.largeObjectStart;
    CHECK_DETAIL(indexAddr != 0, "register native local region without index address");
    if (region.size == DEFAULT_NATIVE_LOCAL_REGION_SIZE) {
        CHECK_DETAIL((indexAddr & (DEFAULT_NATIVE_LOCAL_REGION_SIZE - 1)) == 0,
                     "normal native local region is not 4KB aligned: %p", reinterpret_cast<void*>(indexAddr));
    }
    MAddress slot = indexAddr >> NATIVE_LOCAL_REGION_SLOT_SHIFT;
    NativeMutatorLocalData* ownerData = region.firstRegion->ownerData;
    CHECK_DETAIL(ownerData != nullptr, "native local region has no owner mutator data");
    std::lock_guard<std::mutex> lock(activeMutatorDataMtx);
    CHECK_DETAIL(ownerData->localRegionIndex.Insert(slot, &region),
                 "native local region slot already registered: %zu",
                 static_cast<size_t>(slot));
}

void NativeLocalObjectAllocator::UnregisterRegion(LocalNativeRegion& region)
{
    MAddress indexAddr = region.size == DEFAULT_NATIVE_LOCAL_REGION_SIZE ? region.base : region.largeObjectStart;
    CHECK_DETAIL(indexAddr != 0, "unregister native local region without index address");
    MAddress slot = indexAddr >> NATIVE_LOCAL_REGION_SLOT_SHIFT;
    NativeMutatorLocalData* ownerData = region.firstRegion->ownerData;
    CHECK_DETAIL(ownerData != nullptr, "native local region has no owner mutator data");
    std::lock_guard<std::mutex> lock(activeMutatorDataMtx);
    CHECK_DETAIL(ownerData->localRegionIndex.Erase(slot), "native local region slot is not registered: %zu",
                 static_cast<size_t>(slot));
}

void NativeLocalObjectAllocator::RegisterObject(MAddress obj, LocalNativeRegion& region)
{
    if (region.size == DEFAULT_NATIVE_LOCAL_REGION_SIZE) {
        return;
    }
    CHECK_DETAIL(region.largeObjectStart == 0, "large native local region contains multiple objects");
    region.largeObjectStart = obj;
    RegisterRegion(region);
}

void NativeLocalObjectAllocator::WriteObjectMarker(MAddress obj)
{
    CHECK_DETAIL(obj >= LOCAL_OBJECT_MARKER_SIZE, "invalid native local object address %p", reinterpret_cast<void*>(obj));
    *reinterpret_cast<uint64_t*>(obj - LOCAL_OBJECT_MARKER_SIZE) = LOCAL_OBJECT_MARKER;
}

void NativeLocalObjectAllocator::RunLocalFinalizers(Mutator& mutator, FrameAddress* ownerFA)
{
    LocalNativeRegion* region = GetRegionStackTop(mutator);
    if (region == nullptr) {
        return;
    }
    if (ownerFA != nullptr && region->firstRegion->ownerFA != ownerFA) {
        LOG(RTLOG_FATAL, "run native local finalizers with mismatched frame: region ownerFA %p, target ownerFA %p",
            region->firstRegion->ownerFA, ownerFA);
    }
    while (region->firstRegion->rootRegistry.RunPendingFinalizers(mutator)) {
    }
}
} // namespace MapleRuntime
